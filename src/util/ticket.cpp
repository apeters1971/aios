#include "util/ticket.hpp"

#include "util/aes_gcm.hpp"
#include "util/auth.hpp"
#include "util/base64.hpp"

#include <openssl/rand.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace aios {
namespace {

constexpr const char* kSealInfo = "aios-ticket-seal-v1";
constexpr const char* kTicketAad = "aios-ticket-v1";
constexpr std::size_t kMaxTicketBlob = 4096;

bool const_time_eq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

bool is_hex(const std::string& s, std::size_t len) {
  if (s.size() != len) return false;
  for (unsigned char c : s) {
    if (!std::isxdigit(c)) return false;
  }
  return true;
}

nlohmann::json caps_json(const std::vector<std::string>& caps) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& c : caps) arr.push_back(c);
  return arr;
}

std::vector<std::string> caps_from_json(const nlohmann::json& j) {
  std::vector<std::string> out;
  if (!j.is_array()) return out;
  for (const auto& c : j) {
    if (c.is_string()) out.push_back(c.get<std::string>());
  }
  return out;
}

}  // namespace

const char* principal_role_name(PrincipalRole r) {
  switch (r) {
    case PrincipalRole::Client: return "client";
    case PrincipalRole::Admin: return "admin";
    case PrincipalRole::Node: return "node";
  }
  return "client";
}

std::optional<PrincipalRole> parse_principal_role(const std::string& s) {
  if (s == "client") return PrincipalRole::Client;
  if (s == "admin") return PrincipalRole::Admin;
  if (s == "node") return PrincipalRole::Node;
  return std::nullopt;
}

bool valid_principal_name(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (unsigned char c : name) {
    if (std::isalnum(c) || c == '.' || c == '_' || c == '-') continue;
    return false;
  }
  return true;
}

std::string random_hex(std::size_t bytes) {
  std::vector<unsigned char> buf(bytes);
  if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
    throw std::runtime_error("RAND_bytes failed");
  }
  static const char* hex = "0123456789abcdef";
  std::string out(bytes * 2, '\0');
  for (std::size_t i = 0; i < bytes; ++i) {
    out[i * 2] = hex[buf[i] >> 4];
    out[i * 2 + 1] = hex[buf[i] & 0xf];
  }
  return out;
}

std::string generate_principal_key() { return random_hex(32); }

bool Ticket::allows_oid(const std::string& oid) const {
  if (caps.empty()) return true;
  for (const auto& c : caps) {
    if (oid.compare(0, c.size(), c) == 0) return true;
  }
  return false;
}

bool Ticket::allows_prefix(const std::string& prefix) const {
  if (caps.empty()) return true;
  // The listed prefix must itself be inside one cap; a shorter or unrelated
  // prefix could enumerate oids the principal may not see.
  for (const auto& c : caps) {
    if (prefix.size() >= c.size() && prefix.compare(0, c.size(), c) == 0) return true;
  }
  return false;
}

TicketSealer::TicketSealer(const std::string& cluster_key) {
  const std::string k = hmac_sha256_raw(cluster_key, kSealInfo);
  key_.assign(k.begin(), k.end());
}

std::string TicketSealer::seal(const Ticket& t, std::string& err) const {
  nlohmann::json j = {{"p", t.principal},
                      {"r", principal_role_name(t.role)},
                      {"k", t.session_key},
                      {"i", t.issued_ms},
                      {"e", t.expires_ms}};
  if (!t.caps.empty()) j["c"] = caps_json(t.caps);
  const std::string plain = j.dump();

  std::uint8_t nonce[kAesGcmNonceBytes];
  if (!random_aes_gcm_nonce(nonce, err)) return {};
  std::vector<std::uint8_t> ct;
  // AES-GCM AAD support is not exposed by aes_gcm.hpp; the version tag is bound
  // by prefixing it to the plaintext instead, so a ticket sealed under a future
  // format cannot be opened as v1.
  std::string bound = std::string(kTicketAad) + '\n' + plain;
  if (!aes_256_gcm_encrypt(key_.data(), key_.size(), nonce, sizeof(nonce),
                           reinterpret_cast<const std::uint8_t*>(bound.data()), bound.size(), ct,
                           err)) {
    return {};
  }
  std::vector<std::uint8_t> blob(nonce, nonce + sizeof(nonce));
  blob.insert(blob.end(), ct.begin(), ct.end());
  return std::string(kTicketPrefix) + base64_encode(blob);
}

std::optional<Ticket> TicketSealer::open(const std::string& blob, std::int64_t now_ms,
                                         std::string& err, bool allow_expired) const {
  const std::size_t plen = std::strlen(kTicketPrefix);
  if (blob.size() > kMaxTicketBlob || blob.compare(0, plen, kTicketPrefix) != 0) {
    err = "not a ticket";
    return std::nullopt;
  }
  std::vector<std::uint8_t> raw;
  if (!base64_decode(blob.substr(plen), raw, err) ||
      raw.size() < kAesGcmNonceBytes + kAesGcmTagBytes + 1) {
    err = "malformed ticket";
    return std::nullopt;
  }
  std::vector<std::uint8_t> plain;
  if (!aes_256_gcm_decrypt(key_.data(), key_.size(), raw.data(), kAesGcmNonceBytes,
                           raw.data() + kAesGcmNonceBytes, raw.size() - kAesGcmNonceBytes, plain,
                           err)) {
    err = "ticket not issued by this cluster";
    return std::nullopt;
  }
  const std::string bound(plain.begin(), plain.end());
  const std::string want = std::string(kTicketAad) + '\n';
  if (bound.compare(0, want.size(), want) != 0) {
    err = "ticket version mismatch";
    return std::nullopt;
  }
  Ticket t;
  try {
    auto j = nlohmann::json::parse(bound.substr(want.size()));
    t.principal = j.at("p").get<std::string>();
    auto role = parse_principal_role(j.at("r").get<std::string>());
    if (!role) throw std::runtime_error("role");
    t.role = *role;
    t.session_key = j.at("k").get<std::string>();
    t.issued_ms = j.at("i").get<std::int64_t>();
    t.expires_ms = j.at("e").get<std::int64_t>();
    if (j.contains("c")) t.caps = caps_from_json(j["c"]);
  } catch (const std::exception&) {
    err = "corrupt ticket payload";
    return std::nullopt;
  }
  if (!is_hex(t.session_key, 64)) {
    err = "corrupt ticket payload";
    return std::nullopt;
  }
  if (!allow_expired && now_ms >= t.expires_ms) {
    err = "ticket expired";
    return std::nullopt;
  }
  return t;
}

// --- request / reply ----------------------------------------------------------

std::string ticket_request_canonical(const std::string& principal, std::int64_t ts_ms,
                                     const std::string& nonce) {
  return "aios-ticket-req-v1\n" + principal + '\n' + std::to_string(ts_ms) + '\n' + nonce;
}

TicketRequest make_ticket_request(const std::string& principal, const std::string& principal_key,
                                  std::int64_t now_ms) {
  TicketRequest r;
  r.principal = principal;
  r.ts_ms = now_ms;
  r.nonce = random_hex(16);
  r.proof = hmac_sha256_hex(principal_key, ticket_request_canonical(principal, now_ms, r.nonce));
  return r;
}

nlohmann::json ticket_request_to_json(const TicketRequest& r) {
  return {{"principal", r.principal}, {"ts", r.ts_ms}, {"nonce", r.nonce}, {"proof", r.proof}};
}

std::optional<TicketRequest> ticket_request_from_json(const nlohmann::json& j) {
  if (!j.is_object()) return std::nullopt;
  TicketRequest r;
  try {
    r.principal = j.at("principal").get<std::string>();
    r.ts_ms = j.at("ts").get<std::int64_t>();
    r.nonce = j.at("nonce").get<std::string>();
    r.proof = j.at("proof").get<std::string>();
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (!valid_principal_name(r.principal) || r.nonce.size() < 16 || r.nonce.size() > 128 ||
      !is_hex(r.proof, 64)) {
    return std::nullopt;
  }
  for (unsigned char c : r.nonce) {
    if (!std::isalnum(c) && c != '-' && c != '_') return std::nullopt;
  }
  return r;
}

std::string derive_session_key(const std::string& principal_key, const std::string& client_nonce,
                               const std::string& server_nonce) {
  return hmac_sha256_hex(principal_key, "aios-session-v1\n" + client_nonce + '\n' + server_nonce);
}

std::string ticket_reply_canonical(const std::string& client_nonce, const std::string& ticket,
                                   const std::string& server_nonce, std::int64_t expires_ms) {
  return "aios-ticket-reply-v1\n" + client_nonce + '\n' + ticket + '\n' + server_nonce + '\n' +
         std::to_string(expires_ms);
}

nlohmann::json ticket_reply_to_json(const TicketReply& r) {
  return {{"ticket", r.ticket},
          {"server_nonce", r.server_nonce},
          {"expires_ms", r.expires_ms},
          {"server_proof", r.server_proof},
          {"principal", r.principal},
          {"role", r.role}};
}

std::optional<TicketReply> ticket_reply_from_json(const nlohmann::json& j) {
  if (!j.is_object()) return std::nullopt;
  TicketReply r;
  try {
    r.ticket = j.at("ticket").get<std::string>();
    r.server_nonce = j.at("server_nonce").get<std::string>();
    r.expires_ms = j.at("expires_ms").get<std::int64_t>();
    r.server_proof = j.at("server_proof").get<std::string>();
    r.principal = j.value("principal", "");
    r.role = j.value("role", "");
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (r.ticket.empty() || r.server_nonce.empty() || !is_hex(r.server_proof, 64)) {
    return std::nullopt;
  }
  return r;
}

std::optional<std::string> verify_ticket_reply(const TicketReply& reply,
                                               const std::string& principal_key,
                                               const std::string& client_nonce, std::string& err) {
  const auto session_key = derive_session_key(principal_key, client_nonce, reply.server_nonce);
  const auto expect = hmac_sha256_hex(
      session_key,
      ticket_reply_canonical(client_nonce, reply.ticket, reply.server_nonce, reply.expires_ms));
  if (!const_time_eq(expect, reply.server_proof)) {
    err = "server proof mismatch (wrong principal key or impostor server)";
    return std::nullopt;
  }
  return session_key;
}

std::optional<TicketReply> grant_ticket(const TicketRequest& req, const Principal& p,
                                        const TicketSealer& sealer, std::int64_t now_ms,
                                        int skew_ms, std::int64_t lifetime_ms, std::string& err) {
  if (req.principal != p.name) {
    err = "principal mismatch";
    return std::nullopt;
  }
  if (std::llabs(now_ms - req.ts_ms) > skew_ms) {
    err = "request timestamp outside skew window";
    return std::nullopt;
  }
  const auto expect =
      hmac_sha256_hex(p.key, ticket_request_canonical(req.principal, req.ts_ms, req.nonce));
  if (!const_time_eq(expect, req.proof)) {
    err = "bad proof";
    return std::nullopt;
  }

  TicketReply reply;
  reply.server_nonce = random_hex(16);
  reply.principal = p.name;
  reply.role = principal_role_name(p.role);
  reply.expires_ms = now_ms + lifetime_ms;

  Ticket t;
  t.principal = p.name;
  t.role = p.role;
  t.caps = p.caps;
  t.session_key = derive_session_key(p.key, req.nonce, reply.server_nonce);
  t.issued_ms = now_ms;
  t.expires_ms = reply.expires_ms;
  reply.ticket = sealer.seal(t, err);
  if (reply.ticket.empty()) return std::nullopt;
  reply.server_proof = hmac_sha256_hex(
      t.session_key,
      ticket_reply_canonical(req.nonce, reply.ticket, reply.server_nonce, reply.expires_ms));
  return reply;
}

}  // namespace aios
