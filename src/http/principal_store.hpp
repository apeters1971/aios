#pragma once

#include "object/object_service.hpp"
#include "util/ticket.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aios {

// Cluster-wide principal keyring, stored as one CAS-guarded object
// (kPrincipalKeyringOid) so every node grants tickets from the same set.
// The body is AES-256-GCM encrypted under a key derived from the cluster key:
// replicas on disk, backups and anyone who can read raw objects see only
// ciphertext. The HTTP object API additionally refuses the auth/ prefix.
inline constexpr const char* kPrincipalKeyringOid = "auth/principals";
inline constexpr const char* kReservedAuthOidPrefix = "auth/";

class PrincipalStore {
 public:
  PrincipalStore(std::string cluster_key, ObjectService& objects);

  std::optional<Principal> find(const std::string& name);

  // Keys redacted.
  nlohmann::json list_redacted();

  // Creates a principal. Generates a key if p.key is empty. Returns the stored
  // principal including the plaintext key — the only time it is shown.
  std::optional<Principal> create(Principal p, std::string& err);
  // Replaces the key (new random key unless new_key given); old tickets keep
  // working until they expire, new ticket requests need the new key.
  std::optional<Principal> rotate(const std::string& name, std::string new_key, std::string& err);
  bool remove(const std::string& name, std::string& err);

  void invalidate_cache();

  // Serialization helpers, exposed for tests.
  static std::string encrypt_keyring(const std::vector<Principal>& ps, const std::string& cluster_key,
                                     std::string& err);
  static bool decrypt_keyring(const std::string& body, const std::string& cluster_key,
                              std::vector<Principal>& out, std::string& err);

 private:
  bool refresh_locked(std::string& err);
  bool save_locked(const std::vector<Principal>& ps, std::uint64_t expected_cas, std::string& err);
  template <class Mutate>
  bool mutate_with_retry(Mutate&& fn, std::string& err);

  std::string cluster_key_;
  ObjectService& objects_;
  std::mutex mu_;
  std::vector<Principal> cache_;
  std::uint64_t cache_cas_{0};
  std::int64_t cache_loaded_ms_{0};
  bool cache_valid_{false};
};

}  // namespace aios
