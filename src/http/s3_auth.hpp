#pragma once

#include <string>
#include <unordered_map>

namespace aios {

struct S3AuthResult {
  bool ok{false};
  std::string error;
  std::string access_key;
  std::string region;
};

// Verify AWS Signature Version 4 (AWS4-HMAC-SHA256).
// expected_access_key must match the Credential AKID; secret_key is used for the HMAC.
// payload_hash_hex: value of x-amz-content-sha256 (or hash of body); "UNSIGNED-PAYLOAD" allowed.
S3AuthResult s3_sigv4_verify(const std::string& method, const std::string& canonical_uri,
                             const std::string& canonical_query,
                             const std::unordered_map<std::string, std::string>& headers,
                             const std::string& payload_hash_hex,
                             const std::string& expected_access_key,
                             const std::string& secret_key, int skew_ms);

// Parse Credential access key id from Authorization (empty if missing/malformed).
std::string s3_sigv4_access_key(const std::unordered_map<std::string, std::string>& headers);

// URI-encode for SigV4 (path encodes '/' as %2F when encode_slash; query always encodes).
std::string s3_uri_encode(const std::string& in, bool encode_slash);

}  // namespace aios
