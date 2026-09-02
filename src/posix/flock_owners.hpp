#pragma once

#include <cstdint>
#include <mutex>
#include <set>
#include <utility>

namespace aios {
namespace posix {

// Which open file descriptions (FUSE lock_owner) hold the advisory flock on an
// inode. The cluster lock is per inode and shared by every open of that inode in
// this mount, so release() must only drop it for the open that acquired it.
class FlockOwners {
 public:
  void note_locked(uint64_t ino, uint64_t owner) {
    std::lock_guard lock(mu_);
    held_.insert({ino, owner});
  }

  // Returns true when `owner` held the lock on ino (and is now forgotten).
  bool note_unlocked(uint64_t ino, uint64_t owner) {
    std::lock_guard lock(mu_);
    return held_.erase({ino, owner}) > 0;
  }

  bool holds(uint64_t ino, uint64_t owner) const {
    std::lock_guard lock(mu_);
    return held_.count({ino, owner}) > 0;
  }

  // Forget every owner of ino (inode removed).
  void forget(uint64_t ino) {
    std::lock_guard lock(mu_);
    auto it = held_.lower_bound({ino, 0});
    while (it != held_.end() && it->first == ino) it = held_.erase(it);
  }

 private:
  mutable std::mutex mu_;
  std::set<std::pair<uint64_t, uint64_t>> held_;
};

}  // namespace posix
}  // namespace aios
