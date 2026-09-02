#include "util/auth.hpp"

#include "util/log.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
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
//
// Entries expire by sender timestamp: anything older than now - max_skew_ms is no
// longer accepted by auth_verify anyway, so it can be dropped. The count cap is a
// memory bound sized for peak rate (65536 / 60 s ≈ 1000 msg/s) so legitimate
// entries are not evicted while still replayable.
constexpr std::size_t kReplayCacheCap = 65536;
constexpr std::size_t kReplayShards = 16;
constexpr std::size_t kReplayShardCap = kReplayCacheCap / kReplayShards;

struct ReplayEntry {
  std::int64_t ts;
  std::string digest;
};

struct ReplayShard {
  std::mutex mu;
  // Insertion order; ts values are roughly monotone, so expiry pops from the front.
  std::deque<ReplayEntry> order;
  std::unordered_set<std::string> seen;
};

std::array<ReplayShard, kReplayShards> g_replay;

bool replay_mark(const std::string& digest, std::int64_t ts, std::int64_t now,
                 int max_skew_ms) {
  auto& shard = g_replay[std::hash<std::string>{}(digest) % kReplayShards];
  std::lock_guard lock(shard.mu);
  if (shard.seen.count(digest)) return false;
  const std::int64_t expire_before = now - static_cast<std::int64_t>(max_skew_ms);
  while (!shard.order.empty() && shard.order.front().ts < expire_before) {
    shard.seen.erase(shard.order.front().digest);
    shard.order.pop_front();
  }
  if (shard.order.size() >= kReplayShardCap) {
    shard.seen.erase(shard.order.front().digest);
    shard.order.pop_front();
  }
  shard.order.push_back({ts, digest});
  shard.seen.insert(digest);
  return true;
}

void append_hex_lower(std::string& out, const unsigned char* p, std::size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  const auto at = out.size();
  out.resize(at + n * 2);
  char* w = out.data() + at;
  for (std::size_t i = 0; i < n; ++i) {
    w[i * 2] = kHex[p[i] >> 4];
    w[i * 2 + 1] = kHex[p[i] & 0x0f];
  }
}

std::string to_hex_lower(const unsigned char* p, std::size_t n) {
  std::string out;
  out.reserve(n * 2);
  append_hex_lower(out, p, n);
  return out;
}

std::string random_nonce_hex() {
  unsigned char raw[16];
  if (RAND_bytes(raw, sizeof(raw)) != 1) {
    // Fall back to a process-local counter so signing still progresses.
    static std::mutex mu;
    static std::uint64_t n = 0;
    std::lock_guard lock(mu);
    ++n;
    unsigned char fb[16]{};
    for (int i = 0; i < 8; ++i) {
      fb[i] = static_cast<unsigned char>((n >> ((7 - i) * 8)) & 0xff);
      fb[8 + i] = fb[i];
    }
    return to_hex_lower(fb, sizeof(fb));
  }
  return to_hex_lower(raw, sizeof(raw));
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
  return to_hex_lower(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

std::string sha256_hex(const std::string& data) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (EVP_Digest(data.data(), data.size(), md, &md_len, EVP_sha256(), nullptr) != 1) {
    return {};
  }
  return to_hex_lower(md, md_len);
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
  // Every read below is type-checked: this runs on unauthenticated input, and a
  // throwing nlohmann accessor would take down the RPC worker.
  if (!body.is_object() || !body.contains("ts") || !body.contains("sig")) {
    err = "missing ts/sig";
    return false;
  }
  const auto& ts_j = body.at("ts");
  if (!ts_j.is_number_integer()) {
    err = "bad ts";
    return false;
  }
  const auto ts = ts_j.get<std::int64_t>();
  const auto& sig_j = body.at("sig");
  if (!sig_j.is_string()) {
    err = "bad sig";
    return false;
  }
  const auto sig = sig_j.get<std::string>();
  if (sig.size() != 64) {  // sha256 hex
    err = "bad sig length";
    return false;
  }
  std::string nonce;
  if (auto it = body.find("nonce"); it != body.end()) {
    if (!it->is_string()) {
      err = "bad nonce";
      return false;
    }
    nonce = it->get<std::string>();
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
  const auto replay_key = std::string(msg_type_name(type)) + "\n" + std::to_string(ts) + "\n" +
                          nonce + "\n" + sig;
  if (!replay_mark(replay_key, ts, now, max_skew_ms)) {
    err = "replay";
    return false;
  }
  return true;
}

}  // namespace aios
