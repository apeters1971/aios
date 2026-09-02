#pragma once

// Ticket-based authentication (cephx / Kerberos style), protocol version 1.
//
// Principals (client.alice, admin.ops, ...) hold a long-lived secret key that
// is never sent on the wire. To talk to the cluster a principal proves key
// possession once in a single round trip (POST /auth/ticket) and receives:
//
//   * a ticket: the principal's identity, role, capabilities, a per-session
//     key and an expiry, sealed with AES-256-GCM under a key only the daemons
//     hold (derived from the cluster key). Opaque to the client.
//   * enough material to derive the same session key locally:
//       session_key = HMAC(principal_key, "aios-session-v1\n" client_nonce "\n" server_nonce)
//
// Every later request is the existing AIOS-HMAC-SHA256 scheme with
// Credential=<ticket> and the HMAC keyed by the session key. A daemon opens the
// ticket, learns who is calling and what they may do, and checks the HMAC with
// the embedded session key — no lookup, no shared state between nodes beyond
// the cluster key, and a stolen ticket is worthless without the session key.
//
// Everything here needs only HMAC-SHA256 on the client side, so the kernel
// modules can take part with the crypto they already use.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aios {

// Ticket blobs on the wire start with this prefix so verifiers can tell them
// apart from the legacy shared-key credential ("stl", arbitrary label).
inline constexpr const char* kTicketPrefix = "t1.";

// Principal roles. `node` is what a daemon mints for itself; `admin` may use
// the admin API; `client` is limited to the object API (optionally to a set of
// oid prefixes).
enum class PrincipalRole { Client, Admin, Node };

const char* principal_role_name(PrincipalRole r);
std::optional<PrincipalRole> parse_principal_role(const std::string& s);

struct Principal {
  std::string name;  // e.g. "client.alice"; [A-Za-z0-9._-], max 64
  std::string key;   // 64 hex chars (32 random bytes)
  PrincipalRole role{PrincipalRole::Client};
  // Object-id prefixes the principal may touch. Empty = every oid.
  std::vector<std::string> caps;
  std::int64_t created_ms{0};
};

bool valid_principal_name(const std::string& name);
// 32 random bytes as 64 lowercase hex chars.
std::string generate_principal_key();

// Contents of an opened ticket.
struct Ticket {
  std::string principal;
  PrincipalRole role{PrincipalRole::Client};
  std::vector<std::string> caps;
  std::string session_key;  // 64 hex chars
  std::int64_t issued_ms{0};
  std::int64_t expires_ms{0};

  // True when `oid` is inside the caps (or caps are unrestricted).
  bool allows_oid(const std::string& oid) const;
  // True when a LIST with this prefix cannot return oids outside the caps.
  bool allows_prefix(const std::string& prefix) const;
};

// Seals tickets on behalf of a cluster. Constructed from the cluster key; the
// AES key is derived, never the cluster key itself.
class TicketSealer {
 public:
  explicit TicketSealer(const std::string& cluster_key);

  // Returns "t1.<base64(nonce || ciphertext || tag)>" or empty on failure.
  std::string seal(const Ticket& t, std::string& err) const;
  // Rejects malformed, forged and (unless allow_expired) expired tickets.
  std::optional<Ticket> open(const std::string& blob, std::int64_t now_ms, std::string& err,
                             bool allow_expired = false) const;

 private:
  std::vector<std::uint8_t> key_;
};

// --- Ticket request (client side) --------------------------------------------

// Body of POST /auth/ticket. The proof shows possession of the principal key;
// (ts, nonce) is single-use on the server within the skew window.
struct TicketRequest {
  std::string principal;
  std::int64_t ts_ms{0};
  std::string nonce;  // client nonce, >= 16 random bytes as hex
  std::string proof;  // HMAC(principal_key, canonical) hex
};

std::string ticket_request_canonical(const std::string& principal, std::int64_t ts_ms,
                                     const std::string& nonce);
TicketRequest make_ticket_request(const std::string& principal, const std::string& principal_key,
                                  std::int64_t now_ms);
nlohmann::json ticket_request_to_json(const TicketRequest& r);
std::optional<TicketRequest> ticket_request_from_json(const nlohmann::json& j);

// Both sides derive the session key from the two nonces under the principal key.
std::string derive_session_key(const std::string& principal_key, const std::string& client_nonce,
                               const std::string& server_nonce);

// Reply of POST /auth/ticket. server_proof = HMAC(session_key, canonical) lets
// the client confirm the server knows the principal key (mutual auth) before it
// trusts the ticket.
struct TicketReply {
  std::string ticket;
  std::string server_nonce;
  std::int64_t expires_ms{0};
  std::string server_proof;
  std::string principal;
  std::string role;
};

std::string ticket_reply_canonical(const std::string& client_nonce, const std::string& ticket,
                                   const std::string& server_nonce, std::int64_t expires_ms);
nlohmann::json ticket_reply_to_json(const TicketReply& r);
std::optional<TicketReply> ticket_reply_from_json(const nlohmann::json& j);

// Client-side verification of a reply. On success returns the session key.
std::optional<std::string> verify_ticket_reply(const TicketReply& reply,
                                               const std::string& principal_key,
                                               const std::string& client_nonce, std::string& err);

// Server side: validates proof/skew, mints a ticket. `lifetime_ms` bounds
// expires_ms. Does not handle replay — the caller keys its replay cache on
// (principal, ts, nonce).
std::optional<TicketReply> grant_ticket(const TicketRequest& req, const Principal& p,
                                        const TicketSealer& sealer, std::int64_t now_ms,
                                        int skew_ms, std::int64_t lifetime_ms, std::string& err);

// Random hex string of `bytes` random bytes.
std::string random_hex(std::size_t bytes);

}  // namespace aios
