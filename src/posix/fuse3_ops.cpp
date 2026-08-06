#include "posix/fuse3_ops.hpp"
#include "posix/aios_posix.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/param.h>
#else
#include <sys/statvfs.h>
#endif

namespace {

template <typename F>
int guard(F&& f) {
  try {
    return f();
  } catch (const std::exception&) {
    return -EIO;
  } catch (...) {
    return -EIO;
  }
}

aios_posix_fs* fs_handle() {
  auto* ctx = fuse_get_context();
  auto* fs = static_cast<aios_posix_fs*>(ctx->private_data);
  if (fs) aios_posix_set_caller(fs, static_cast<uint32_t>(ctx->uid), static_cast<uint32_t>(ctx->gid));
  return fs;
}

int lookup_path(aios_posix_fs* fs, const char* path, aios_posix_stat* st_out) {
  if (!path || path[0] != '/') return -EINVAL;
  if (std::strcmp(path, "/") == 0) return aios_posix_getattr(fs, 1, st_out);

  uint64_t ino = 1;
  const char* p = path + 1;
  while (*p) {
    std::string component;
    while (*p && *p != '/') component.push_back(*p++);
    while (*p == '/') ++p;
    if (component.empty()) continue;
    aios_posix_stat cur{};
    int rc = aios_posix_lookup(fs, ino, component.c_str(), &cur);
    if (rc) return rc;
    ino = cur.ino;
    *st_out = cur;
  }
  return 0;
}

void fill_times(uint64_t ns, time_t* sec, long* nsec) {
  *sec = static_cast<time_t>(ns / 1000000000ull);
  *nsec = static_cast<long>(ns % 1000000000ull);
}

#ifdef __APPLE__
void copy_stat(const aios_posix_stat& st, fuse_darwin_attr* out) {
  std::memset(out, 0, sizeof(*out));
  out->ino = st.ino;
  out->mode = st.mode;
  out->nlink = st.nlink;
  out->uid = st.uid;
  out->gid = st.gid;
  out->size = static_cast<off_t>(st.size);
  fill_times(st.atime_ns, &out->atimespec.tv_sec, &out->atimespec.tv_nsec);
  fill_times(st.mtime_ns, &out->mtimespec.tv_sec, &out->mtimespec.tv_nsec);
  fill_times(st.ctime_ns, &out->ctimespec.tv_sec, &out->ctimespec.tv_nsec);
  out->btimespec = out->ctimespec;
}

int posix_getattr(const char* path, fuse_darwin_attr* attr, struct fuse_file_info* /*fi*/) {
  return guard([&] {
    auto* fs = fs_handle();
    if (!fs) return -EIO;
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    copy_stat(st, attr);
    return 0;
  });
}
#else
void copy_stat(const aios_posix_stat& st, struct stat* stbuf) {
  std::memset(stbuf, 0, sizeof(*stbuf));
  stbuf->st_ino = st.ino;
  stbuf->st_mode = st.mode;
  stbuf->st_nlink = st.nlink;
  stbuf->st_uid = st.uid;
  stbuf->st_gid = st.gid;
  stbuf->st_size = static_cast<off_t>(st.size);
  fill_times(st.atime_ns, &stbuf->st_atim.tv_sec, &stbuf->st_atim.tv_nsec);
  fill_times(st.mtime_ns, &stbuf->st_mtim.tv_sec, &stbuf->st_mtim.tv_nsec);
  fill_times(st.ctime_ns, &stbuf->st_ctim.tv_sec, &stbuf->st_ctim.tv_nsec);
}

int posix_getattr(const char* path, struct stat* stbuf, struct fuse_file_info* /*fi*/) {
  return guard([&] {
    auto* fs = fs_handle();
    if (!fs) return -EIO;
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    copy_stat(st, stbuf);
    return 0;
  });
}
#endif

int resolve_parent(aios_posix_fs* fs, const char* path, uint64_t* parent_out,
                   std::string* name_out) {
  std::string p = path;
  while (!p.empty() && p.back() == '/') p.pop_back();
  if (p.empty() || p == "/") return -EINVAL;
  const auto slash = p.rfind('/');
  std::string parent_path = (slash == 0) ? "/" : p.substr(0, slash);
  *name_out = p.substr(slash + 1);
  aios_posix_stat st{};
  int rc = lookup_path(fs, parent_path.c_str(), &st);
  if (rc) return rc;
  *parent_out = st.ino;
  return 0;
}

#ifdef __APPLE__
int posix_readdir(const char* path, void* buf, fuse_darwin_fill_dir_t filler, off_t offset,
                  struct fuse_file_info* /*fi*/, enum fuse_readdir_flags /*flags*/) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    uint64_t off = static_cast<uint64_t>(offset);
    aios_posix_dirent ents[64];
    while (true) {
      int n = aios_posix_readdir(fs, st.ino, &off, ents, 64);
      if (n < 0) return n;
      if (n == 0) break;
      for (int i = 0; i < n; ++i) {
        fuse_darwin_attr e{};
        e.ino = ents[i].ino;
        e.mode = ents[i].mode;
        const off_t cookie = static_cast<off_t>(off - static_cast<uint64_t>(n) + i + 1);
        if (filler(buf, ents[i].name, &e, cookie, static_cast<fuse_fill_dir_flags>(0))) {
          return 0;
        }
      }
    }
    return 0;
  });
}
#else
int posix_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset,
                  struct fuse_file_info* /*fi*/, enum fuse_readdir_flags /*flags*/) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    uint64_t off = static_cast<uint64_t>(offset);
    aios_posix_dirent ents[64];
    while (true) {
      int n = aios_posix_readdir(fs, st.ino, &off, ents, 64);
      if (n < 0) return n;
      if (n == 0) break;
      for (int i = 0; i < n; ++i) {
        struct stat e{};
        e.st_ino = ents[i].ino;
        e.st_mode = ents[i].mode;
        const off_t cookie = static_cast<off_t>(off - static_cast<uint64_t>(n) + i + 1);
        if (filler(buf, ents[i].name, &e, cookie, static_cast<fuse_fill_dir_flags>(0))) {
          return 0;
        }
      }
    }
    return 0;
  });
}
#endif

int posix_mkdir(const char* path, mode_t mode) {
  return guard([&] {
    auto* fs = fs_handle();
    uint64_t parent = 0;
    std::string name;
    int rc = resolve_parent(fs, path, &parent, &name);
    if (rc) return rc;
    return aios_posix_mkdir(fs, parent, name.c_str(), static_cast<uint32_t>(mode), nullptr);
  });
}

int posix_create(const char* path, mode_t mode, struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    uint64_t parent = 0;
    std::string name;
    int rc = resolve_parent(fs, path, &parent, &name);
    if (rc) return rc;
    aios_posix_stat st{};
    rc = aios_posix_create(fs, parent, name.c_str(), static_cast<uint32_t>(mode), &st);
    if (rc) return rc;
    if (fi) fi->fh = st.ino;
    return 0;
  });
}

int posix_unlink(const char* path) {
  return guard([&] {
    auto* fs = fs_handle();
    uint64_t parent = 0;
    std::string name;
    int rc = resolve_parent(fs, path, &parent, &name);
    if (rc) return rc;
    return aios_posix_unlink(fs, parent, name.c_str());
  });
}

int posix_rmdir(const char* path) {
  return guard([&] {
    auto* fs = fs_handle();
    uint64_t parent = 0;
    std::string name;
    int rc = resolve_parent(fs, path, &parent, &name);
    if (rc) return rc;
    return aios_posix_rmdir(fs, parent, name.c_str());
  });
}

int posix_rename(const char* from, const char* to, unsigned int /*flags*/) {
  return guard([&] {
    auto* fs = fs_handle();
    uint64_t op = 0, np = 0;
    std::string on, nn;
    int rc = resolve_parent(fs, from, &op, &on);
    if (rc) return rc;
    rc = resolve_parent(fs, to, &np, &nn);
    if (rc) return rc;
    return aios_posix_rename(fs, op, on.c_str(), np, nn.c_str());
  });
}

int posix_link(const char* from, const char* to) {
  return guard([&] {
    auto* fs = fs_handle();
    uint64_t op = 0, np = 0;
    std::string on, nn;
    int rc = resolve_parent(fs, from, &op, &on);
    if (rc) return rc;
    rc = resolve_parent(fs, to, &np, &nn);
    if (rc) return rc;
    return aios_posix_link(fs, op, on.c_str(), np, nn.c_str());
  });
}

int posix_open(const char* path, struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    if (!S_ISREG(st.mode)) return -EISDIR;
    int amode = 0;
    if (fi) {
      const int acc = fi->flags & O_ACCMODE;
      if (acc == O_RDONLY || acc == O_RDWR) amode |= R_OK;
      if (acc == O_WRONLY || acc == O_RDWR) amode |= W_OK;
    } else {
      amode = R_OK;
    }
    if (amode) {
      rc = aios_posix_access(fs, st.ino, amode);
      if (rc) return rc;
    }
    if (fi) fi->fh = st.ino;
    return 0;
  });
}

int posix_read(const char* /*path*/, char* buf, size_t size, off_t offset,
               struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    if (!fi) return -EIO;
    size_t out = 0;
    int rc = aios_posix_read(fs, fi->fh, static_cast<uint64_t>(offset), buf, size, &out);
    if (rc) return rc;
    return static_cast<int>(out);
  });
}

int posix_write(const char* /*path*/, const char* buf, size_t size, off_t offset,
                struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    if (!fi) return -EIO;
    size_t out = 0;
    int rc = aios_posix_write(fs, fi->fh, static_cast<uint64_t>(offset), buf, size, &out);
    if (rc) return rc;
    return static_cast<int>(out);
  });
}

int posix_truncate(const char* path, off_t size, struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    uint64_t ino = 0;
    if (fi && fi->fh) {
      ino = fi->fh;
    } else {
      aios_posix_stat st{};
      int rc = lookup_path(fs, path, &st);
      if (rc) return rc;
      ino = st.ino;
    }
    return aios_posix_truncate(fs, ino, static_cast<uint64_t>(size));
  });
}

int posix_fsync(const char* /*path*/, int /*datasync*/, struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    if (!fi) return -EIO;
    return aios_posix_fsync(fs, fi->fh);
  });
}

int posix_chmod(const char* path, mode_t mode, struct fuse_file_info* /*fi*/) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    aios_posix_stat ps{};
    ps.mode = static_cast<uint32_t>(mode);
    return aios_posix_setattr(fs, st.ino, &ps, AIOS_POSIX_SET_MODE);
  });
}

int posix_chown(const char* path, uid_t uid, gid_t gid, struct fuse_file_info* /*fi*/) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    aios_posix_stat ps{};
    uint32_t set = 0;
    if (uid != static_cast<uid_t>(-1)) {
      ps.uid = uid;
      set |= AIOS_POSIX_SET_UID;
    }
    if (gid != static_cast<gid_t>(-1)) {
      ps.gid = gid;
      set |= AIOS_POSIX_SET_GID;
    }
    return aios_posix_setattr(fs, st.ino, &ps, set);
  });
}

#ifdef __APPLE__
// High-level Darwin setattr uses the same bit mask as fuse_lowlevel.h.
#ifndef FUSE_SET_ATTR_MODE
#define FUSE_SET_ATTR_MODE (1 << 0)
#define FUSE_SET_ATTR_UID (1 << 1)
#define FUSE_SET_ATTR_GID (1 << 2)
#define FUSE_SET_ATTR_SIZE (1 << 3)
#define FUSE_SET_ATTR_ATIME (1 << 4)
#define FUSE_SET_ATTR_MTIME (1 << 5)
#endif

int posix_setattr(const char* path, fuse_darwin_attr* attr, int to_set,
                  struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    uint64_t ino = 0;
    if (fi && fi->fh) {
      ino = fi->fh;
    } else {
      aios_posix_stat st{};
      int rc = lookup_path(fs, path, &st);
      if (rc) return rc;
      ino = st.ino;
    }
    if (to_set & FUSE_SET_ATTR_SIZE) {
      int rc = aios_posix_truncate(fs, ino, static_cast<uint64_t>(attr->size));
      if (rc) return rc;
    }
    aios_posix_stat ps{};
    uint32_t set = 0;
    if (to_set & FUSE_SET_ATTR_MODE) {
      ps.mode = attr->mode;
      set |= AIOS_POSIX_SET_MODE;
    }
    if (to_set & FUSE_SET_ATTR_UID) {
      ps.uid = attr->uid;
      set |= AIOS_POSIX_SET_UID;
    }
    if (to_set & FUSE_SET_ATTR_GID) {
      ps.gid = attr->gid;
      set |= AIOS_POSIX_SET_GID;
    }
    if (to_set & FUSE_SET_ATTR_ATIME) {
      ps.atime_ns = static_cast<uint64_t>(attr->atimespec.tv_sec) * 1000000000ull +
                    static_cast<uint64_t>(attr->atimespec.tv_nsec);
      set |= AIOS_POSIX_SET_ATIME;
    }
    if (to_set & FUSE_SET_ATTR_MTIME) {
      ps.mtime_ns = static_cast<uint64_t>(attr->mtimespec.tv_sec) * 1000000000ull +
                    static_cast<uint64_t>(attr->mtimespec.tv_nsec);
      set |= AIOS_POSIX_SET_MTIME;
    }
    if (set) {
      int rc = aios_posix_setattr(fs, ino, &ps, set);
      if (rc) return rc;
    }
    aios_posix_stat st{};
    int rc = aios_posix_getattr(fs, ino, &st);
    if (rc) return rc;
    copy_stat(st, attr);
    return 0;
  });
}

int posix_statfs(const char* /*path*/, struct statfs* stbuf) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_statvfs st{};
    int rc = aios_posix_statfs(fs, &st);
    if (rc) return rc;
    std::memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize = static_cast<uint32_t>(st.bsize);
    stbuf->f_iosize = static_cast<uint32_t>(st.bsize);
    stbuf->f_blocks = st.blocks;
    stbuf->f_bfree = st.bfree;
    stbuf->f_bavail = st.bavail;
    stbuf->f_files = static_cast<uint32_t>(st.files);
    stbuf->f_ffree = static_cast<uint32_t>(st.ffree);
    return 0;
  });
}
#else
int posix_statfs(const char* /*path*/, struct statvfs* stbuf) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_statvfs st{};
    int rc = aios_posix_statfs(fs, &st);
    if (rc) return rc;
    std::memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize = st.bsize;
    stbuf->f_frsize = st.bsize;
    stbuf->f_blocks = st.blocks;
    stbuf->f_bfree = st.bfree;
    stbuf->f_bavail = st.bavail;
    stbuf->f_files = st.files;
    stbuf->f_ffree = st.ffree;
    stbuf->f_namemax = st.namemax;
    return 0;
  });
}
#endif

uint64_t path_ino(aios_posix_fs* fs, const char* path, struct fuse_file_info* fi) {
  if (fi && fi->fh) return fi->fh;
  aios_posix_stat st{};
  int rc = lookup_path(fs, path, &st);
  if (rc) return 0;
  return st.ino;
}

#ifdef __APPLE__
int posix_setxattr(const char* path, const char* name, const char* value, size_t size, int flags,
                   uint32_t /*position*/) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    return aios_posix_setxattr(fs, st.ino, name, value, size, flags);
  });
}

int posix_getxattr(const char* path, const char* name, char* value, size_t size,
                   uint32_t /*position*/) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    return aios_posix_getxattr(fs, st.ino, name, value, size);
  });
}
#else
int posix_setxattr(const char* path, const char* name, const char* value, size_t size, int flags) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    return aios_posix_setxattr(fs, st.ino, name, value, size, flags);
  });
}

int posix_getxattr(const char* path, const char* name, char* value, size_t size) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    return aios_posix_getxattr(fs, st.ino, name, value, size);
  });
}
#endif

int posix_listxattr(const char* path, char* list, size_t size) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    return aios_posix_listxattr(fs, st.ino, list, size);
  });
}

int posix_removexattr(const char* path, const char* name) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    return aios_posix_removexattr(fs, st.ino, name);
  });
}

int posix_flock(const char* path, struct fuse_file_info* fi, int op) {
  return guard([&] {
    auto* fs = fs_handle();
    const uint64_t ino = path_ino(fs, path, fi);
    if (!ino) return -ENOENT;
    return aios_posix_flock(fs, ino, op);
  });
}

}  // namespace

fuse_operations aios_fuse_operations() {
  fuse_operations ops{};
  ops.getattr = posix_getattr;
  ops.readdir = posix_readdir;
  ops.mkdir = posix_mkdir;
  ops.create = posix_create;
  ops.unlink = posix_unlink;
  ops.rmdir = posix_rmdir;
  ops.rename = posix_rename;
  ops.link = posix_link;
  ops.open = posix_open;
  ops.read = posix_read;
  ops.write = posix_write;
  ops.truncate = posix_truncate;
  ops.fsync = posix_fsync;
  ops.chmod = posix_chmod;
  ops.chown = posix_chown;
  ops.statfs = posix_statfs;
  ops.setxattr = posix_setxattr;
  ops.getxattr = posix_getxattr;
  ops.listxattr = posix_listxattr;
  ops.removexattr = posix_removexattr;
  ops.flock = posix_flock;
#ifdef __APPLE__
  ops.setattr = posix_setattr;
#endif
  return ops;
}
