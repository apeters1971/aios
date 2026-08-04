#include "http/s3_iam.hpp"

#include "util/log.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace aios {
namespace {

constexpr std::int64_t kCacheTtlMs = 5000;

std::uint64_t cas_from_attrs(const std::unordered_map<std::string, std::string>& attrs) {
  auto it = attrs.find("aios.posix.cas");
  if (it == attrs.end()) return 0;
  try {
    return static_cast<std::uint64_t>(std::stoull(it->second));
  } catch (...) {
    return 0;
  }
}

S3Credential from_json(const nlohmann::json& j) {
  S3Credential c;
  c.access_key_id = j.value("access_key_id", "");
  c.secret = j.value("secret", "");
  c.uid = j.value("uid", 0u);
  c.gid = j.value("gid", 0u);
  if (j.contains("buckets") && j["buckets"].is_array()) {
    for (const auto& b : j["buckets"]) {
      if (b.is_string()) c.buckets.push_back(b.get<std::string>());
    }
  }
  return c;
}

nlohmann::json to_json(const S3Credential& c, bool redact_secret) {
  nlohmann::json buckets = nlohmann::json::array();
  for (const auto& b : c.buckets) buckets.push_back(b);
  return {{"access_key_id", c.access_key_id},
          {"secret", redact_secret ? "***" : c.secret},
          {"uid", c.uid},
          {"gid", c.gid},
          {"buckets", buckets}};
}

}  // namespace

S3IamStore::S3IamStore(Config cfg, ObjectService& objects)
    : cfg_(std::move(cfg)), objects_(objects) {}

std::string S3IamStore::oid_for_volume(const std::string& volume) {
  return "s3iam/" + (volume.empty() ? std::string("s3") : volume);
}

bool S3IamStore::valid_access_key_id(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;
  for (unsigned char c : id) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.') continue;
    return false;
  }
  return true;
}

std::string S3IamStore::random_secret() {
  std::array<unsigned char, 32> buf{};
  if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
    throw std::runtime_error("RAND_bytes failed for S3 secret");
  }
  static const char* hex = "0123456789abcdef";
  std::string out(64, '\0');
  for (std::size_t i = 0; i < buf.size(); ++i) {
    out[i * 2] = hex[buf[i] >> 4];
    out[i * 2 + 1] = hex[buf[i] & 0xf];
  }
  return out;
}

bool S3IamStore::allows_bucket(const S3Credential& cred, const std::string& bucket) const {
  return std::find(cred.buckets.begin(), cred.buckets.end(), bucket) != cred.buckets.end();
}

void S3IamStore::invalidate_cache() {
  std::lock_guard lock(mu_);
  cache_valid_ = false;
}

bool S3IamStore::refresh_locked(std::string& err) {
  const auto oid = oid_for_volume(cfg_.s3_volume);
  auto head = objects_.api_get(oid, std::nullopt, std::nullopt, {});
  if (!head.ok) {
    if (head.code == "not_found" || head.error.find("not found") != std::string::npos ||
        head.code == "NoSuchKey") {
      cache_.clear();
      cache_cas_ = 0;
      cache_loaded_ms_ = now_ms();
      cache_valid_ = true;
      return true;
    }
    // Treat missing tip as empty store.
    if (!head.info) {
      cache_.clear();
      cache_cas_ = 0;
      cache_loaded_ms_ = now_ms();
      cache_valid_ = true;
      return true;
    }
    err = head.error.empty() ? head.code : head.error;
    return false;
  }
  cache_cas_ = cas_from_attrs(head.attrs);
  cache_.clear();
  if (!head.data || head.data->empty()) {
    cache_loaded_ms_ = now_ms();
    cache_valid_ = true;
    return true;
  }
  try {
    const std::string raw(reinterpret_cast<const char*>(head.data->data()), head.data->size());
    auto j = nlohmann::json::parse(raw);
    if (j.contains("credentials") && j["credentials"].is_array()) {
      for (const auto& item : j["credentials"]) {
        auto c = from_json(item);
        if (!c.access_key_id.empty() && !c.secret.empty()) cache_.push_back(std::move(c));
      }
    }
  } catch (const std::exception& e) {
    err = std::string("bad s3iam json: ") + e.what();
    return false;
  }
  cache_loaded_ms_ = now_ms();
  cache_valid_ = true;
  return true;
}

bool S3IamStore::save_locked(const std::vector<S3Credential>& creds, std::uint64_t expected_cas,
                             std::string& err) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& c : creds) arr.push_back(to_json(c, false));
  nlohmann::json doc = {{"version", 1}, {"credentials", arr}};
  const std::string body = doc.dump();
  const auto oid = oid_for_volume(cfg_.s3_volume);
  const std::uint64_t new_cas = expected_cas + 1;
  std::unordered_map<std::string, std::string> attrs{{"aios.posix.cas", std::to_string(new_cas)}};
  std::vector<AttrPrecondition> preds;
  if (expected_cas == 0) {
    auto head = objects_.api_head(oid, {});
    if (!head.ok || !head.info) {
      preds.push_back({AttrPrecondition::Kind::MustNotExist, {}, {}});
    } else if (cas_from_attrs(head.attrs) == 0) {
      preds.push_back({AttrPrecondition::Kind::Absent, "aios.posix.cas", {}});
    } else {
      err = "cas mismatch";
      return false;
    }
  } else {
    preds.push_back(
        {AttrPrecondition::Kind::Eq, "aios.posix.cas", std::to_string(expected_cas)});
  }
  auto r = objects_.api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                            attrs, true, preds);
  if (!r.ok) {
    err = r.error.empty() ? r.code : r.error;
    return false;
  }
  cache_ = creds;
  cache_cas_ = new_cas;
  cache_loaded_ms_ = now_ms();
  cache_valid_ = true;
  return true;
}

std::optional<S3Credential> S3IamStore::find(const std::string& access_key_id) {
  std::lock_guard lock(mu_);
  std::string err;
  if (!cache_valid_ || now_ms() - cache_loaded_ms_ > kCacheTtlMs) {
    if (!refresh_locked(err)) {
      AIOS_LOG_WARN("s3iam refresh failed: ", err);
      if (!cache_valid_) return std::nullopt;
    }
  }
  for (const auto& c : cache_) {
    if (c.access_key_id == access_key_id) return c;
  }
  return std::nullopt;
}

nlohmann::json S3IamStore::list_redacted() {
  std::lock_guard lock(mu_);
  std::string err;
  if (!cache_valid_ || now_ms() - cache_loaded_ms_ > kCacheTtlMs) {
    refresh_locked(err);
  }
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& c : cache_) arr.push_back(to_json(c, true));
  return {{"credentials", arr}, {"oid", oid_for_volume(cfg_.s3_volume)}};
}

std::optional<S3Credential> S3IamStore::create(S3Credential cred, std::string& err) {
  if (!valid_access_key_id(cred.access_key_id)) {
    err = "invalid access_key_id";
    return std::nullopt;
  }
  if (cred.buckets.empty()) {
    err = "buckets required";
    return std::nullopt;
  }
  for (const auto& b : cred.buckets) {
    if (b.empty() || b[0] == '.') {
      err = "invalid bucket name in allowlist";
      return std::nullopt;
    }
  }
  if (cred.access_key_id == cfg_.s3_access_key) {
    err = "access_key_id conflicts with global s3_access_key";
    return std::nullopt;
  }
  if (cred.secret.empty()) cred.secret = random_secret();

  std::lock_guard lock(mu_);
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (!refresh_locked(err)) return std::nullopt;
    for (const auto& c : cache_) {
      if (c.access_key_id == cred.access_key_id) {
        err = "access_key_id already exists";
        return std::nullopt;
      }
    }
    auto next = cache_;
    next.push_back(cred);
    const auto cas = cache_cas_;
    if (save_locked(next, cas, err)) return cred;
    if (err.find("conflict") == std::string::npos && err.find("precondition") == std::string::npos &&
        err.find("cas") == std::string::npos) {
      return std::nullopt;
    }
  }
  err = "cas conflict retry exhausted";
  return std::nullopt;
}

bool S3IamStore::remove(const std::string& access_key_id, std::string& err) {
  if (access_key_id.empty()) {
    err = "access_key_id required";
    return false;
  }
  std::lock_guard lock(mu_);
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (!refresh_locked(err)) return false;
    auto next = cache_;
    auto it = std::remove_if(next.begin(), next.end(), [&](const S3Credential& c) {
      return c.access_key_id == access_key_id;
    });
    if (it == next.end()) {
      err = "not found";
      return false;
    }
    next.erase(it, next.end());
    const auto cas = cache_cas_;
    if (save_locked(next, cas, err)) return true;
    if (err.find("conflict") == std::string::npos && err.find("precondition") == std::string::npos &&
        err.find("cas") == std::string::npos) {
      return false;
    }
  }
  err = "cas conflict retry exhausted";
  return false;
}

}  // namespace aios
