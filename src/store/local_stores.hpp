#pragma once

#include "store/object_store.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

// Owns one ObjectStore per local usable aios_path.
class LocalStores {
 public:
  // Open missing stores for paths; close stores no longer listed.
  void sync_paths(const std::vector<std::string>& aios_paths, ObjectStoreOptions opts);

  ObjectStore* get(const std::string& aios_path);
  std::vector<std::string> paths() const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<ObjectStore>> stores_;
  ObjectStoreOptions opts_{};
};

}  // namespace aios
