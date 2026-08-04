#include "util/auth.hpp"

#include "util/log.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace aios {

std::string hmac_sha256_raw(const std::string& key, const std::string& data) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           reinterpret_cast<const unsigned char*>(data.data()), data.size(), md,
           &md_len) == nullptr) {
    return {};
  }
  return std::string(reinterpret_cast<char*>(md), md_len);
}

std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
  const auto raw = hmac_sha256_raw(key, data);
  if (raw.empty()) return {};
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (unsigned char c : raw) {
    oss << std::setw(2) << static_cast<unsigned>(c);
  }
  return oss.str();
}

std::string sha256_hex(const std::string& data) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (EVP_Digest(data.data(), data.size(), md, &md_len, EVP_sha256(), nullptr) != 1) {
    return {};
  }
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < md_len; ++i) {
    oss << std::setw(2) << static_cast<unsigned>(md[i]);
  }
  return oss.str();
}

std::string auth_canonical(MsgType type, std::int64_t ts, const nlohmann::json& body) {
  nlohmann::json payload = body;
  payload.erase("ts");
  payload.erase("sig");
  // Stable dump: sorted keys, no extra whitespace.
  return std::string(msg_type_name(type)) + "\n" + std::to_string(ts) + "\n" +
         payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
}

void auth_sign(nlohmann::json& body, MsgType type, const std::string& cluster_key) {
  const auto ts = now_ms();
  body.erase("sig");
  body["ts"] = ts;
  const auto canon = auth_canonical(type, ts, body);
  body["sig"] = hmac_sha256_hex(cluster_key, canon);
}

bool auth_verify(const nlohmann::json& body, MsgType type, const std::string& cluster_key,
                 int max_skew_ms, std::string& err) {
  if (!body.contains("ts") || !body.contains("sig")) {
    err = "missing ts/sig";
    return false;
  }
  std::int64_t ts = 0;
  try {
    ts = body.at("ts").get<std::int64_t>();
  } catch (...) {
    err = "bad ts";
    return false;
  }
  const auto sig = body.value("sig", "");
  if (sig.size() != 64) {  // sha256 hex
    err = "bad sig length";
    return false;
  }
  const auto now = now_ms();
  if (std::llabs(now - ts) > max_skew_ms) {
    err = "ts skew too large";
    return false;
  }
  const auto expect = hmac_sha256_hex(cluster_key, auth_canonical(type, ts, body));
  if (expect.empty() || expect != sig) {
    err = "bad signature";
    return false;
  }
  return true;
}

}  // namespace aios
