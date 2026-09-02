#pragma once

#include <cstdint>
#include <mutex>
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
  std::string credential;
};

// Authorization: AIOS-HMAC-SHA256 Credential=..., SignedHeaders=..., Signature=...
// Requires x-aios-date or date header within skew. Replay protection: with an
// x-aios-nonce the (date, nonce, signature) tuple is single-use; without one,
// mutating methods (PUT/POST/DELETE) whose signature covers a concrete payload
// hash are single-use on (method, target, date, signature). UNSIGNED-PAYLOAD
// bodies are exempt so distinct writes issued in the same millisecond (kernel
// range PUTs, appends) are not mistaken for replays. replay == nullptr disables.
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
