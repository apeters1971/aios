#pragma once

#include "store/object_store.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

// Owns one ObjectStore per local usable aios_path.
//
// Removal is deferred: a path must be missing from `retire_after_misses`
// consecutive sync_paths calls before its store is retired (default 1, so an
// explicit removal takes effect at once; the scanner may raise it to ride out
// transient scan glitches), and a retired store is kept alive for
// `retire_grace_ms` so callers still holding a raw pointer from get() finish
// safely. Prefer get_shared() for anything that outlives one call.
class LocalStores {
 public:
  // Open missing stores for paths; close stores no longer listed (deferred).
  void sync_paths(const std::vector<std::string>& aios_paths, ObjectStoreOptions opts);

  ObjectStore* get(const std::string& aios_path);
  std::shared_ptr<ObjectStore> get_shared(const std::string& aios_path);
  std::vector<std::string> paths() const;

  // Tunables (defaults: 1 miss, 60 s grace).
  void set_retire_policy(int retire_after_misses, std::int64_t retire_grace_ms);
  std::size_t retired_count() const;

 private:
  struct Retired {
    std::shared_ptr<ObjectStore> store;
    std::int64_t retired_at_ms{0};
  };

  void drop_expired_retired_locked(std::int64_t now);

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<ObjectStore>> stores_;
  std::unordered_map<std::string, int> miss_counts_;
  std::unordered_map<std::string, Retired> retired_;
  ObjectStoreOptions opts_{};
  int retire_after_misses_{1};
  std::int64_t retire_grace_ms_{60 * 1000};
};

}  // namespace aios
