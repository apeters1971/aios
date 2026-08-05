#include "posix/posix_internal.hpp"

#include <errno.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#ifdef __APPLE__
constexpr int kRstatMissing = ENOATTR;
#else
constexpr int kRstatMissing = ENODATA;
#endif

namespace aios {
namespace posix {
namespace {

constexpr std::size_t kMaxDirsPerFlush = 64;

bool recompute_dir_rstat(FsState& st, uint64_t dir_ino) {
  auto dir_meta = load_inode(st, dir_ino);
  if (!dir_meta.exists || !S_ISDIR(dir_meta.mode)) return false;

  DirTable dt(st.session, st.volume, dir_ino);
  dt.load();

  uint64_t rbytes = 0;
  uint64_t rfiles = 0;
  uint64_t rdirs = 0;
  uint64_t rtime = dir_meta.mtime_ns;

  for (const auto& [name, child_ino] : dt.entries()) {
    (void)name;
    auto child = load_inode(st, child_ino);
    if (!child.exists) continue;

    // Repair missing parent pointers on old volumes.
    if (child.parent_ino == 0 && child.ino != kRootIno) {
      child.parent_ino = dir_ino;
      try {
        store_inode(st, child);
      } catch (...) {
      }
    }

    if (S_ISDIR(child.mode)) {
      ++rdirs;
      rbytes += child.rbytes;
      rfiles += child.rfiles;
      rdirs += child.rdirs;
      rtime = std::max(rtime, child.rtime_ns);
      rtime = std::max(rtime, child.mtime_ns);
    } else if (S_ISREG(child.mode)) {
      ++rfiles;
      rbytes += child.size;
      rtime = std::max(rtime, child.mtime_ns);
    }
  }

  const bool changed = dir_meta.rbytes != rbytes || dir_meta.rfiles != rfiles ||
                       dir_meta.rdirs != rdirs || dir_meta.rtime_ns != rtime;
  if (!changed) return false;

  dir_meta.rbytes = rbytes;
  dir_meta.rfiles = rfiles;
  dir_meta.rdirs = rdirs;
  dir_meta.rtime_ns = rtime;
  store_inode(st, dir_meta);
  if (dir_meta.parent_ino != 0) mark_rstat_dirty(st, dir_meta.parent_ino);
  return true;
}

}  // namespace

void mark_rstat_dirty(FsState& st, uint64_t dir_ino) {
  if (dir_ino == 0) return;
  std::lock_guard lock(st.mu);
  st.rstat_dirty.insert(dir_ino);
}

void flush_rstats(FsState& st) {
  for (std::size_t round = 0; round < 8; ++round) {
    std::vector<uint64_t> batch;
    {
      std::lock_guard lock(st.mu);
      if (st.rstat_dirty.empty()) return;
      batch.reserve(std::min(kMaxDirsPerFlush, st.rstat_dirty.size()));
      for (auto it = st.rstat_dirty.begin();
           it != st.rstat_dirty.end() && batch.size() < kMaxDirsPerFlush;) {
        batch.push_back(*it);
        it = st.rstat_dirty.erase(it);
      }
    }
    if (batch.empty()) return;
    // Deepest dirs first when possible: higher ino often newer/deeper; also process
    // children before parents by sorting descending parent_ino chain length later.
    // Simple approach: recompute all; parent dirties cascade into next round.
    std::sort(batch.begin(), batch.end(), std::greater<uint64_t>());
    for (const auto ino : batch) {
      try {
        recompute_dir_rstat(st, ino);
      } catch (...) {
      }
    }
  }
}

void start_rstat_thread(FsState& st) {
  if (st.rstat_interval_ms <= 0) return;
  if (st.rstat_thread.joinable()) return;
  st.rstat_stop.store(false);
  const int interval = st.rstat_interval_ms;
  st.rstat_thread = std::thread([&st, interval] {
    while (!st.rstat_stop.load()) {
      for (int i = 0; i < interval / 50 && !st.rstat_stop.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (st.rstat_stop.load()) break;
      try {
        flush_rstats(st);
        if (st.quota) st.quota->tick();
      } catch (...) {
      }
    }
  });
}

void stop_rstat_thread(FsState& st) {
  st.rstat_stop.store(true);
  if (st.rstat_thread.joinable()) st.rstat_thread.join();
  try {
    flush_rstats(st);
  } catch (...) {
  }
}

bool is_rstat_xattr(const char* name) {
  if (!name) return false;
  return std::strcmp(name, kRstatXattrRbytes) == 0 || std::strcmp(name, kRstatXattrRfiles) == 0 ||
         std::strcmp(name, kRstatXattrRdirs) == 0 || std::strcmp(name, kRstatXattrRtime) == 0;
}

int get_rstat_xattr(const InodeMeta& m, const char* name, void* value, size_t size) {
  if (!S_ISDIR(m.mode)) return -kRstatMissing;
  uint64_t v = 0;
  if (std::strcmp(name, kRstatXattrRbytes) == 0) v = m.rbytes;
  else if (std::strcmp(name, kRstatXattrRfiles) == 0) v = m.rfiles;
  else if (std::strcmp(name, kRstatXattrRdirs) == 0) v = m.rdirs;
  else if (std::strcmp(name, kRstatXattrRtime) == 0) v = m.rtime_ns / 1000000000ull;
  else return -kRstatMissing;
  const std::string s = std::to_string(v);
  if (size == 0) return static_cast<int>(s.size());
  if (size < s.size()) return -ERANGE;
  if (value) std::memcpy(value, s.data(), s.size());
  return static_cast<int>(s.size());
}

}  // namespace posix
}  // namespace aios
