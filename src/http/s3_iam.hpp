#pragma once

#include "config.hpp"
#include "object/object_service.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aios {

struct S3Credential {
  std::string access_key_id;
  std::string secret;
  std::uint32_t uid{0};
  std::uint32_t gid{0};
  std::vector<std::string> buckets;
};

// Cluster-shared S3 IAM credentials (CAS object oid s3iam/{s3_volume}).
class S3IamStore {
 public:
  S3IamStore(Config cfg, ObjectService& objects);

  static std::string oid_for_volume(const std::string& volume);

  std::optional<S3Credential> find(const std::string& access_key_id);
  bool allows_bucket(const S3Credential& cred, const std::string& bucket) const;

  // Secrets redacted as "***".
  nlohmann::json list_redacted();

  // Creates credential. Generates secret if empty. Returns full credential (secret plaintext).
  // On error sets err and returns nullopt.
  std::optional<S3Credential> create(S3Credential cred, std::string& err);

  bool remove(const std::string& access_key_id, std::string& err);

  void invalidate_cache();

 private:
  bool refresh_locked(std::string& err);
  bool save_locked(const std::vector<S3Credential>& creds, std::uint64_t expected_cas,
                   std::string& err);
  static bool valid_access_key_id(const std::string& id);
  static std::string random_secret();

  Config cfg_;
  ObjectService& objects_;
  std::mutex mu_;
  std::vector<S3Credential> cache_;
  std::uint64_t cache_cas_{0};
  std::int64_t cache_loaded_ms_{0};
  bool cache_valid_{false};
};

}  // namespace aios
