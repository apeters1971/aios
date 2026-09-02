#pragma once

#include "util/ticket.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

// Bodies larger than this are streamed on the server (no in-memory HMAC).
// Clients sign those requests as UNSIGNED-PAYLOAD or with the body's sha256
// (proto/http.md); the server verifies a concrete hash after the body arrives.
inline constexpr std::size_t kHttpStreamBodyBytes = 256u * 1024u;

// Optional request nonce. When present it is appended to the canonical string
// (after the payload hash) and every accepted (date, nonce, signature) is
// remembered for the skew window, so an identical request cannot be replayed.
inline constexpr const char* kHttpNonceHeader = "x-aios-nonce";

// Remembers accepted signatures until the skew window has passed them by.
// Keys are only inserted after a signature verified, so the size is bounded by
// legitimate traffic; kMaxEntries is a hard ceiling on top of time eviction.
class HttpReplayCache {
 public:
  static constexpr std::size_t kMaxEntries = 200000;
  // Returns false when key was already accepted and has not expired.
  bool check_and_insert(const std::string& key, std::int64_t expires_at_ms, std::int64_t now_ms);
  void clear();
  std::size_t size() const;

 private:
  void evict_expired(std::int64_t now_ms);
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::int64_t> entries_;
  std::size_t inserts_since_sweep_{0};
};

// Process-wide cache used by http_auth_verify.
HttpReplayCache& http_replay_cache();

// Build canonical string and HMAC for AIOS-HMAC-SHA256.
std::string http_canonical(const std::string& method, const std::string& path_with_query,
                           const std::string& date, const std::string& signed_headers,
                           const std::unordered_map<std::string, std::string>& headers,
                           const std::string& payload_hash_hex);

std::string http_sign(const std::string& cluster_key, const std::string& canonical);

struct HttpAuthResult {
  bool ok{false};
  std::string error;
  // Stable error code for clients: bad_signature, ticket_expired, bad_ticket,
  // shared_key_refused, replayed, ...
  std::string code;
  std::string credential;
  // Who signed. Empty principal means the shared cluster key was used, which is
  // treated as PrincipalRole::Node (full access) for compatibility.
  std::string principal;
  PrincipalRole role{PrincipalRole::Node};
  std::optional<Ticket> ticket;

  bool is_admin() const { return role == PrincipalRole::Admin || role == PrincipalRole::Node; }
};

// What a verifier accepts. `sealer == nullptr` disables ticket credentials;
// `allow_shared_key == false` refuses the legacy cluster-key HMAC (the caller
// decides that per peer, e.g. loopback only).
struct HttpAuthPolicy {
  std::string cluster_key;
  const TicketSealer* sealer{nullptr};
  bool allow_shared_key{true};
  int skew_ms{300000};
};

// Authorization: AIOS-HMAC-SHA256 Credential=..., SignedHeaders=..., Signature=...
// Credential is either an opaque label (legacy: HMAC keyed by the cluster key)
// or a ticket "t1...." (HMAC keyed by the ticket's session key, see
// util/ticket.hpp). Requires x-aios-date or date header within skew.
// Replay protection: with an x-aios-nonce the (date, nonce, signature) tuple is
// single-use; without one, mutating methods (PUT/POST/DELETE) whose signature
// covers a concrete payload hash are single-use on (method, target, date,
// signature). UNSIGNED-PAYLOAD bodies are exempt so distinct writes issued in
// the same millisecond (kernel range PUTs, appends) are not mistaken for
// replays. replay == nullptr disables.
HttpAuthResult http_auth_verify(const std::string& method, const std::string& path_with_query,
                                const std::unordered_map<std::string, std::string>& headers,
                                const std::string& payload_hash_hex,
                                const HttpAuthPolicy& policy, HttpReplayCache* replay);
HttpAuthResult http_auth_verify(const std::string& method, const std::string& path_with_query,
                                const std::unordered_map<std::string, std::string>& headers,
                                const std::string& payload_hash_hex,
                                const std::string& cluster_key, int skew_ms,
                                HttpReplayCache* replay);
HttpAuthResult http_auth_verify(const std::string& method, const std::string& path_with_query,
                                const std::unordered_map<std::string, std::string>& headers,
                                const std::string& payload_hash_hex,
                                const std::string& cluster_key, int skew_ms);

// Lowercase header lookup helper.
std::string header_get(const std::unordered_map<std::string, std::string>& headers,
                       const std::string& name);

// Percent-encode an object id for /o/{oid}[/sub]. Encodes '/' as %2F so lock,
// versions, append, and watch subpaths stay unambiguous.
std::string http_url_encode_oid(const std::string& oid);

}  // namespace aios
