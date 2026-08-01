#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

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
// Requires x-aios-date or date header within skew.
HttpAuthResult http_auth_verify(const std::string& method, const std::string& path_with_query,
                                const std::unordered_map<std::string, std::string>& headers,
                                const std::string& payload_hash_hex,
                                const std::string& cluster_key, int skew_ms);

// Lowercase header lookup helper.
std::string header_get(const std::unordered_map<std::string, std::string>& headers,
                       const std::string& name);

}  // namespace aios
