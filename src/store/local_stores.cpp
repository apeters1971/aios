#include "store/local_stores.hpp"

#include "util/log.hpp"

#include <unordered_set>

namespace aios {

void LocalStores::drop_expired_retired_locked(std::int64_t now) {
  for (auto it = retired_.begin(); it != retired_.end();) {
    if (now - it->second.retired_at_ms >= retire_grace_ms_) {
      AIOS_LOG_INFO("closing retired store ", it->first);
      it = retired_.erase(it);
    } else {
      ++it;
    }
  }
}

void LocalStores::sync_paths(const std::vector<std::string>& aios_paths,
                             ObjectStoreOptions opts) {
  std::lock_guard lock(mu_);
  opts_ = opts;
  const auto now = now_ms();
  std::unordered_set<std::string> want(aios_paths.begin(), aios_paths.end());

  drop_expired_retired_locked(now);

  for (auto it = stores_.begin(); it != stores_.end();) {
    if (want.count(it->first)) {
      miss_counts_.erase(it->first);
      ++it;
      continue;
    }
    const int misses = ++miss_counts_[it->first];
    if (misses < retire_after_misses_) {
      AIOS_LOG_WARN("store ", it->first, " missing from scan (", misses, "/",
                    retire_after_misses_, ")");
      ++it;
      continue;
    }
    AIOS_LOG_INFO("retiring store ", it->first);
    miss_counts_.erase(it->first);
    retired_[it->first] = Retired{std::move(it->second), now};
    it = stores_.erase(it);
  }

  for (const auto& path : aios_paths) {
    if (stores_.count(path)) continue;
    auto retired = retired_.find(path);
    if (retired != retired_.end() && retired->second.store && retired->second.store->is_open()) {
      // Same directory came back within the grace period: reuse the live handle
      // instead of opening a second connection set on the same shards.
      AIOS_LOG_INFO("reviving retired store ", path);
      stores_[path] = std::move(retired->second.store);
      retired_.erase(retired);
      continue;
    }
    auto store = std::make_shared<ObjectStore>();
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

std::shared_ptr<ObjectStore> LocalStores::get_shared(const std::string& aios_path) {
  std::lock_guard lock(mu_);
  auto it = stores_.find(aios_path);
  if (it == stores_.end()) return nullptr;
  return it->second;
}

std::vector<std::string> LocalStores::paths() const {
  std::lock_guard lock(mu_);
  std::vector<std::string> out;
  out.reserve(stores_.size());
  for (const auto& [p, _] : stores_) out.push_back(p);
  return out;
}

void LocalStores::set_retire_policy(int retire_after_misses, std::int64_t retire_grace_ms) {
  std::lock_guard lock(mu_);
  retire_after_misses_ = retire_after_misses < 1 ? 1 : retire_after_misses;
  retire_grace_ms_ = retire_grace_ms < 0 ? 0 : retire_grace_ms;
}

std::size_t LocalStores::retired_count() const {
  std::lock_guard lock(mu_);
  return retired_.size();
}

}  // namespace aios
