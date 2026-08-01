#include "store/local_stores.hpp"

#include "util/log.hpp"

#include <unordered_set>

namespace aios {

void LocalStores::sync_paths(const std::vector<std::string>& aios_paths,
                             ObjectStoreOptions opts) {
  std::lock_guard lock(mu_);
  opts_ = opts;
  std::unordered_set<std::string> want(aios_paths.begin(), aios_paths.end());

  for (auto it = stores_.begin(); it != stores_.end();) {
    if (!want.count(it->first)) {
      AIOS_LOG_INFO("closing store ", it->first);
      it = stores_.erase(it);
    } else {
      ++it;
    }
  }

  for (const auto& path : aios_paths) {
    if (stores_.count(path)) continue;
    auto store = std::make_unique<ObjectStore>();
    std::string err;
    if (!store->open(path, opts_, err)) {
      AIOS_LOG_WARN("open store failed ", path, ": ", err);
      continue;
    }
    AIOS_LOG_INFO("opened store ", path);
    stores_[path] = std::move(store);
  }
}

ObjectStore* LocalStores::get(const std::string& aios_path) {
  std::lock_guard lock(mu_);
  auto it = stores_.find(aios_path);
  if (it == stores_.end()) return nullptr;
  return it->second.get();
}

std::vector<std::string> LocalStores::paths() const {
  std::lock_guard lock(mu_);
  std::vector<std::string> out;
  out.reserve(stores_.size());
  for (const auto& [p, _] : stores_) out.push_back(p);
  return out;
}

}  // namespace aios
