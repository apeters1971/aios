#include "fs/mounts.hpp"

#include "util/log.hpp"

#include <fstream>
#include <set>
#include <sstream>

#if defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#elif defined(__linux__)
#include <mntent.h>
#endif

namespace aios {

bool is_virtual_fstype(const std::string& fstype) {
  static const std::set<std::string> kVirtual = {
      "devfs",     "proc",      "procfs",    "sysfs",     "tmpfs",
      "devtmpfs",  "cgroup",    "cgroup2",   "pstore",    "securityfs",
      "debugfs",   "tracefs",   "configfs",  "fusectl",   "mqueue",
      "hugetlbfs", "binfmt_misc", "autofs",  "rpc_pipefs", "bpf",
      "overlay",   "nsfs",      "ramfs",     "none",
  };
  return kVirtual.count(fstype) > 0;
}

std::vector<MountPoint> list_mounts() {
  std::vector<MountPoint> out;

#if defined(__APPLE__)
  struct statfs* mntbuf = nullptr;
  const int count = getmntinfo(&mntbuf, MNT_NOWAIT);
  if (count <= 0 || !mntbuf) {
    AIOS_LOG_WARN("getmntinfo failed");
    return out;
  }
  for (int i = 0; i < count; ++i) {
    MountPoint m;
    m.path = mntbuf[i].f_mntonname;
    m.fstype = mntbuf[i].f_fstypename;
    if (is_virtual_fstype(m.fstype)) continue;
    out.push_back(std::move(m));
  }
#elif defined(__linux__)
  FILE* f = setmntent("/proc/self/mounts", "r");
  if (!f) {
    AIOS_LOG_WARN("cannot open /proc/self/mounts");
    return out;
  }
  struct mntent* ent;
  while ((ent = getmntent(f)) != nullptr) {
    MountPoint m;
    m.path = ent->mnt_dir;
    m.fstype = ent->mnt_type;
    if (is_virtual_fstype(m.fstype)) continue;
    out.push_back(std::move(m));
  }
  endmntent(f);
#else
  AIOS_LOG_WARN("mount enumeration not implemented on this platform");
#endif

  return out;
}

}  // namespace aios
