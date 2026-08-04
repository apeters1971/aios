#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "net/framing.hpp"

namespace aios {

// HMAC-SHA256 hex digest of `data` with `key`.
std::string hmac_sha256_hex(const std::string& key, const std::string& data);

// Raw HMAC-SHA256 (32 bytes).
std::string hmac_sha256_raw(const std::string& key, const std::string& data);

// SHA-256 hex of data.
std::string sha256_hex(const std::string& data);

// Build canonical signing string for a message body (excludes ts/sig).
std::string auth_canonical(MsgType type, std::int64_t ts, const nlohmann::json& body);

// Insert ts + sig into body. Overwrites any existing ts/sig.
void auth_sign(nlohmann::json& body, MsgType type, const std::string& cluster_key);

// Verify ts skew and HMAC. On failure, err is set.
bool auth_verify(const nlohmann::json& body, MsgType type, const std::string& cluster_key,
                 int max_skew_ms, std::string& err);

}  // namespace aios
