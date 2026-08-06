#include "util/auth.hpp"

#include "util/log.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstdlib>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace aios {
namespace {

bool const_time_eq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

// Replay protection within the auth skew window. Each signed message carries a
// unique nonce so identical legitimate payloads in the same millisecond (hellos,
// empty acks) do not collide. Cache key is type+ts+nonce+sig.
constexpr std::size_t kReplayCacheCap = 4096;

std::mutex g_replay_mu;
std::deque<std::string> g_replay_order;
std::unordered_set<std::string> g_replay_seen;

bool replay_mark(const std::string& digest) {
  std::lock_guard lock(g_replay_mu);
  if (g_replay_seen.count(digest)) return false;
  if (g_replay_order.size() >= kReplayCacheCap) {
    g_replay_seen.erase(g_replay_order.front());
    g_replay_order.pop_front();
  }
  g_replay_order.push_back(digest);
  g_replay_seen.insert(digest);
  return true;
}

std::string random_nonce_hex() {
  unsigned char raw[16];
  if (RAND_bytes(raw, sizeof(raw)) != 1) {
    // Fall back to a process-local counter so signing still progresses.
    static std::mutex mu;
    static std::uint64_t n = 0;
    std::lock_guard lock(mu);
    ++n;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << n << std::setw(16) << n;
    return oss.str();
  }
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (unsigned char c : raw) oss << std::setw(2) << static_cast<unsigned>(c);
  return oss.str();
}

}  // namespace

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
  // Stable dump: sorted keys, no extra whitespace. Nonce remains in the payload.
  return std::string(msg_type_name(type)) + "\n" + std::to_string(ts) + "\n" +
         payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
}

void auth_sign(nlohmann::json& body, MsgType type, const std::string& cluster_key) {
  const auto ts = now_ms();
  body.erase("sig");
  body["ts"] = ts;
  body["nonce"] = random_nonce_hex();
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
  if (expect.empty() || !const_time_eq(expect, sig)) {
    err = "bad signature";
    return false;
  }
  // Prefer nonce when present (new peers); fall back to sig-only for older senders.
  const auto nonce = body.value("nonce", "");
  const auto replay_key = std::string(msg_type_name(type)) + "\n" + std::to_string(ts) + "\n" +
                          nonce + "\n" + sig;
  if (!replay_mark(replay_key)) {
    err = "replay";
    return false;
  }
  return true;
}

}  // namespace aios
