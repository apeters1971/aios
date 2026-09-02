#include "http/principal_store.hpp"

#include "util/aes_gcm.hpp"
#include "util/auth.hpp"
#include "util/base64.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace aios {
namespace {

constexpr std::int64_t kCacheTtlMs = 5000;
constexpr const char* kKeyringSealInfo = "aios-keyring-seal-v1";
constexpr const char* kKeyringMagic = "aios-keyring-v1";

std::uint64_t cas_from_attrs(const std::unordered_map<std::string, std::string>& attrs) {
  auto it = attrs.find("aios.posix.cas");
  if (it == attrs.end()) return 0;
  try {
    return static_cast<std::uint64_t>(std::stoull(it->second));
  } catch (...) {
    return 0;
  }
}

bool is_hex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (unsigned char c : s) {
    if (!std::isxdigit(c)) return false;
  }
  return true;
}

nlohmann::json to_json(const Principal& p, bool redact) {
  nlohmann::json caps = nlohmann::json::array();
  for (const auto& c : p.caps) caps.push_back(c);
  return {{"name", p.name},
          {"key", redact ? "***" : p.key},
          {"role", principal_role_name(p.role)},
          {"caps", caps},
          {"created_ms", p.created_ms}};
}

std::optional<Principal> from_json(const nlohmann::json& j) {
  Principal p;
  p.name = j.value("name", "");
  p.key = j.value("key", "");
  auto role = parse_principal_role(j.value("role", "client"));
  if (!role || !valid_principal_name(p.name) || !is_hex64(p.key)) return std::nullopt;
  p.role = *role;
  p.created_ms = j.value("created_ms", std::int64_t{0});
  if (j.contains("caps") && j["caps"].is_array()) {
    for (const auto& c : j["caps"]) {
      if (c.is_string()) p.caps.push_back(c.get<std::string>());
    }
  }
  return p;
}

bool is_cas_conflict(const std::string& err) {
  return err.find("conflict") != std::string::npos || err.find("precondition") != std::string::npos ||
         err.find("cas") != std::string::npos;
}

}  // namespace

PrincipalStore::PrincipalStore(std::string cluster_key, ObjectService& objects)
    : cluster_key_(std::move(cluster_key)), objects_(objects) {}

std::string PrincipalStore::encrypt_keyring(const std::vector<Principal>& ps,
                                            const std::string& cluster_key, std::string& err) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& p : ps) arr.push_back(to_json(p, false));
  const std::string plain = nlohmann::json{{"version", 1}, {"principals", arr}}.dump();
  const std::string key = hmac_sha256_raw(cluster_key, kKeyringSealInfo);
  std::uint8_t nonce[kAesGcmNonceBytes];
  if (!random_aes_gcm_nonce(nonce, err)) return {};
  std::vector<std::uint8_t> ct;
  if (!aes_256_gcm_encrypt(reinterpret_cast<const std::uint8_t*>(key.data()), key.size(), nonce,
                           sizeof(nonce), reinterpret_cast<const std::uint8_t*>(plain.data()),
                           plain.size(), ct, err)) {
    return {};
  }
  std::vector<std::uint8_t> blob(nonce, nonce + sizeof(nonce));
  blob.insert(blob.end(), ct.begin(), ct.end());
  return nlohmann::json{{"format", kKeyringMagic}, {"sealed", base64_encode(blob)}}.dump();
}

bool PrincipalStore::decrypt_keyring(const std::string& body, const std::string& cluster_key,
                                     std::vector<Principal>& out, std::string& err) {
  out.clear();
  std::vector<std::uint8_t> blob;
  try {
    auto j = nlohmann::json::parse(body);
    if (j.value("format", "") != kKeyringMagic) {
      err = "unknown keyring format";
      return false;
    }
    if (!base64_decode(j.value("sealed", ""), blob, err)) return false;
  } catch (const std::exception& e) {
    err = std::string("bad keyring json: ") + e.what();
    return false;
  }
  if (blob.size() < kAesGcmNonceBytes + kAesGcmTagBytes) {
    err = "keyring blob too short";
    return false;
  }
  const std::string key = hmac_sha256_raw(cluster_key, kKeyringSealInfo);
  std::vector<std::uint8_t> plain;
  if (!aes_256_gcm_decrypt(reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
                           blob.data(), kAesGcmNonceBytes, blob.data() + kAesGcmNonceBytes,
                           blob.size() - kAesGcmNonceBytes, plain, err)) {
    err = "keyring was sealed under a different cluster key";
    return false;
  }
  try {
    auto j = nlohmann::json::parse(plain.begin(), plain.end());
    if (j.contains("principals") && j["principals"].is_array()) {
      for (const auto& item : j["principals"]) {
        if (auto p = from_json(item)) out.push_back(std::move(*p));
      }
    }
  } catch (const std::exception& e) {
    err = std::string("bad keyring payload: ") + e.what();
    return false;
  }
  return true;
}

void PrincipalStore::invalidate_cache() {
  std::lock_guard lock(mu_);
  cache_valid_ = false;
}

bool PrincipalStore::refresh_locked(std::string& err) {
  auto head = objects_.api_get(kPrincipalKeyringOid, std::nullopt, std::nullopt, {});
  if (!head.ok) {
    if (head.code == "not_found" || !head.info ||
        head.error.find("not found") != std::string::npos) {
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
  if (head.data && !head.data->empty()) {
    const std::string raw(reinterpret_cast<const char*>(head.data->data()), head.data->size());
    std::vector<Principal> ps;
    if (!decrypt_keyring(raw, cluster_key_, ps, err)) return false;
    cache_ = std::move(ps);
  }
  cache_loaded_ms_ = now_ms();
  cache_valid_ = true;
  return true;
}

bool PrincipalStore::save_locked(const std::vector<Principal>& ps, std::uint64_t expected_cas,
                                 std::string& err) {
  const std::string body = encrypt_keyring(ps, cluster_key_, err);
  if (body.empty()) return false;
  const std::uint64_t new_cas = expected_cas + 1;
  std::unordered_map<std::string, std::string> attrs{{"aios.posix.cas", std::to_string(new_cas)}};
  std::vector<AttrPrecondition> preds;
  if (expected_cas == 0) {
    auto head = objects_.api_head(kPrincipalKeyringOid, {});
    if (!head.ok || !head.info) {
      preds.push_back({AttrPrecondition::Kind::MustNotExist, {}, {}});
    } else if (cas_from_attrs(head.attrs) == 0) {
      preds.push_back({AttrPrecondition::Kind::Absent, "aios.posix.cas", {}});
    } else {
      err = "cas mismatch";
      return false;
    }
  } else {
    preds.push_back({AttrPrecondition::Kind::Eq, "aios.posix.cas", std::to_string(expected_cas)});
  }
  auto r = objects_.api_put(kPrincipalKeyringOid, reinterpret_cast<const std::uint8_t*>(body.data()),
                            body.size(), attrs, true, preds);
  if (!r.ok) {
    err = r.error.empty() ? r.code : r.error;
    return false;
  }
  cache_ = ps;
  cache_cas_ = new_cas;
  cache_loaded_ms_ = now_ms();
  cache_valid_ = true;
  return true;
}

template <class Mutate>
bool PrincipalStore::mutate_with_retry(Mutate&& fn, std::string& err) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (!refresh_locked(err)) return false;
    auto next = cache_;
    if (!fn(next, err)) return false;
    const auto cas = cache_cas_;
    if (save_locked(next, cas, err)) return true;
    if (!is_cas_conflict(err)) return false;
  }
  err = "cas conflict retry exhausted";
  return false;
}

std::optional<Principal> PrincipalStore::find(const std::string& name) {
  std::lock_guard lock(mu_);
  std::string err;
  if (!cache_valid_ || now_ms() - cache_loaded_ms_ > kCacheTtlMs) {
    if (!refresh_locked(err)) {
      AIOS_LOG_WARN("principal keyring refresh failed: ", err);
      if (!cache_valid_) return std::nullopt;
    }
  }
  for (const auto& p : cache_) {
    if (p.name == name) return p;
  }
  return std::nullopt;
}

nlohmann::json PrincipalStore::list_redacted() {
  std::lock_guard lock(mu_);
  std::string err;
  if (!cache_valid_ || now_ms() - cache_loaded_ms_ > kCacheTtlMs) refresh_locked(err);
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& p : cache_) arr.push_back(to_json(p, true));
  return {{"principals", arr}, {"oid", kPrincipalKeyringOid}};
}

std::optional<Principal> PrincipalStore::create(Principal p, std::string& err) {
  if (!valid_principal_name(p.name)) {
    err = "invalid principal name (use [A-Za-z0-9._-], max 64)";
    return std::nullopt;
  }
  if (p.key.empty()) p.key = generate_principal_key();
  if (!is_hex64(p.key)) {
    err = "key must be 64 hex characters";
    return std::nullopt;
  }
  for (const auto& c : p.caps) {
    if (c.empty() || c.rfind(kReservedAuthOidPrefix, 0) == 0) {
      err = "invalid cap prefix";
      return std::nullopt;
    }
  }
  if (p.created_ms == 0) p.created_ms = now_ms();

  std::lock_guard lock(mu_);
  const bool ok = mutate_with_retry(
      [&](std::vector<Principal>& next, std::string& e) {
        for (const auto& q : next) {
          if (q.name == p.name) {
            e = "principal already exists";
            return false;
          }
        }
        next.push_back(p);
        return true;
      },
      err);
  if (!ok) return std::nullopt;
  return p;
}

std::optional<Principal> PrincipalStore::rotate(const std::string& name, std::string new_key,
                                                std::string& err) {
  if (new_key.empty()) new_key = generate_principal_key();
  if (!is_hex64(new_key)) {
    err = "key must be 64 hex characters";
    return std::nullopt;
  }
  Principal out;
  std::lock_guard lock(mu_);
  const bool ok = mutate_with_retry(
      [&](std::vector<Principal>& next, std::string& e) {
        for (auto& q : next) {
          if (q.name == name) {
            q.key = new_key;
            out = q;
            return true;
          }
        }
        e = "not found";
        return false;
      },
      err);
  if (!ok) return std::nullopt;
  return out;
}

bool PrincipalStore::remove(const std::string& name, std::string& err) {
  if (name.empty()) {
    err = "name required";
    return false;
  }
  std::lock_guard lock(mu_);
  return mutate_with_retry(
      [&](std::vector<Principal>& next, std::string& e) {
        auto it = std::remove_if(next.begin(), next.end(),
                                 [&](const Principal& q) { return q.name == name; });
        if (it == next.end()) {
          e = "not found";
          return false;
        }
        next.erase(it, next.end());
        return true;
      },
      err);
}

}  // namespace aios
