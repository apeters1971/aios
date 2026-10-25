#include "util/auth.hpp"

#include "util/log.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/params.h>
#endif

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

bool replay_mark(const std::string& digest, std::int64_t ts, std::int64_t now, int max_skew_ms) {
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

#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
void prime_openssl();
#endif

std::string random_nonce_hex() {
#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
  prime_openssl();
#endif
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

#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
// HMAC() on OpenSSL 3 is EVP_Q_mac(), which fetches "HMAC" and "SHA256" on
// every call. The namemap lookup is unlocked while another thread may be
// inserting the same name under the namemap lock, so the first concurrent
// burst (auth verify, HTTP signing) is a ThreadSanitizer race inside libcrypto.
// Fetch each method once; the objects are immutable afterwards and safe to share.
// Each call still gets its own context.
struct Sha256Method {
  EVP_MD* md{nullptr};
  Sha256Method() { md = EVP_MD_fetch(nullptr, "SHA256", nullptr); }
  ~Sha256Method() { EVP_MD_free(md); }
  Sha256Method(const Sha256Method&) = delete;
  Sha256Method& operator=(const Sha256Method&) = delete;
};

struct HmacMethod {
  EVP_MAC* mac{nullptr};
  HmacMethod() {
    mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    if (mac == nullptr) return;
    // Resolve SHA-256 in the provider before any other thread starts an HMAC.
    EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
    if (ctx == nullptr) return;
    char digest[] = "SHA256";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string("digest", digest, 0),
        OSSL_PARAM_construct_end(),
    };
    unsigned char key = 0;
    unsigned char byte = 0;
    unsigned char out[EVP_MAX_MD_SIZE];
    std::size_t out_len = 0;
    if (EVP_MAC_init(ctx, &key, 1, params) == 1 && EVP_MAC_update(ctx, &byte, 1) == 1) {
      EVP_MAC_final(ctx, out, &out_len, sizeof(out));
    }
    EVP_MAC_CTX_free(ctx);
  }
  ~HmacMethod() { EVP_MAC_free(mac); }
  HmacMethod(const HmacMethod&) = delete;
  HmacMethod& operator=(const HmacMethod&) = delete;
};

const EVP_MD* sha256_md() {
  static const Sha256Method method;
  return method.md;
}

const HmacMethod& hmac_method() {
  static const HmacMethod method;
  return method;
}

// One thread loads both methods and the DRBG. Callers block on this guard, so
// the namemap is populated before any concurrent HMAC, SHA-256, or RAND_bytes.
void prime_openssl() {
  static const int once = [] {
    OPENSSL_init_crypto(OPENSSL_INIT_ADD_ALL_CIPHERS | OPENSSL_INIT_ADD_ALL_DIGESTS, nullptr);
    (void)sha256_md();
    (void)hmac_method();
    unsigned char raw[16];
    RAND_bytes(raw, sizeof(raw));
    return 0;
  }();
  (void)once;
}
#endif

}  // namespace

std::string hmac_sha256_raw(const std::string& key, const std::string& data) {
#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
  prime_openssl();
  const auto& method = hmac_method();
  if (method.mac == nullptr) return {};
  EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(method.mac);
  if (ctx == nullptr) return {};
  char digest[] = "SHA256";
  OSSL_PARAM params[] = {
      OSSL_PARAM_construct_utf8_string("digest", digest, 0),
      OSSL_PARAM_construct_end(),
  };
  unsigned char md[EVP_MAX_MD_SIZE];
  std::size_t md_len = 0;
  const bool ok =
      EVP_MAC_init(ctx, reinterpret_cast<const unsigned char*>(key.data()), key.size(), params) ==
          1 &&
      EVP_MAC_update(ctx, reinterpret_cast<const unsigned char*>(data.data()), data.size()) == 1 &&
      EVP_MAC_final(ctx, md, &md_len, sizeof(md)) == 1;
  EVP_MAC_CTX_free(ctx);
  if (!ok || md_len == 0) return {};
  return std::string(reinterpret_cast<char*>(md), md_len);
#else
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           reinterpret_cast<const unsigned char*>(data.data()), data.size(), md,
           &md_len) == nullptr) {
    return {};
  }
  return std::string(reinterpret_cast<char*>(md), md_len);
#endif
}

std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
  const auto raw = hmac_sha256_raw(key, data);
  if (raw.empty()) return {};
  return to_hex_lower(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

std::string sha256_hex(const std::string& data) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
  prime_openssl();
  const EVP_MD* sha = sha256_md();
  if (sha == nullptr || EVP_Digest(data.data(), data.size(), md, &md_len, sha, nullptr) != 1) {
    return {};
  }
#else
  if (EVP_Digest(data.data(), data.size(), md, &md_len, EVP_sha256(), nullptr) != 1) {
    return {};
  }
#endif
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
  const auto replay_key =
      std::string(msg_type_name(type)) + "\n" + std::to_string(ts) + "\n" + nonce + "\n" + sig;
  if (!replay_mark(replay_key, ts, now, max_skew_ms)) {
    err = "replay";
    return false;
  }
  return true;
}

}  // namespace aios
