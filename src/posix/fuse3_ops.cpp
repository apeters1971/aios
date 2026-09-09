#include "posix/fuse3_ops.hpp"
#include "posix/aios_posix.h"
#include "posix/flock_owners.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/param.h>
#else
#include <sys/statvfs.h>
#endif

namespace {

// fi->fh is the inode, shared by every open of the file; fi->lock_owner tells
// the opens apart so release() only drops a flock this open actually took.
aios::posix::FlockOwners& flock_owners() {
  static aios::posix::FlockOwners owners;
  return owners;
}

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
    /* Cluster .aios markers live on real disks. Stating one on this mount
     * from aiosd's scanner deadlocks (FUSE HTTP → same process). */
    if (component == ".aios") return -ENOENT;
    aios_posix_stat cur{};
    int rc = aios_posix_lookup(fs, ino, component.c_str(), &cur);
    if (rc) return rc;
    ino = cur.ino;
    *st_out = cur;
  }
  return 0;
}

// nullpath_ok skips the path lookup when fi is set (unlinked-but-open files,
// and writeback setattr-on-close which always carries FATTR_FH). lookup_path
// of a NULL path is -EINVAL, which GNU cp reports as "failed to close".
int load_stat_path_or_fh(aios_posix_fs* fs, const char* path, struct fuse_file_info* fi,
                         aios_posix_stat* st) {
  if (fi && fi->fh) return aios_posix_getattr(fs, fi->fh, st);
  return lookup_path(fs, path, st);
}

uint64_t path_ino(aios_posix_fs* fs, const char* path, struct fuse_file_info* fi);

void fill_times(uint64_t ns, time_t* sec, long* nsec) {
  *sec = static_cast<time_t>(ns / 1000000000ull);
  *nsec = static_cast<long>(ns % 1000000000ull);
}

blkcnt_t posix_st_blocks(uint64_t size) {
  return static_cast<blkcnt_t>((size + 511ull) / 512ull);
}

// fuse_attr.blksize is copied into inode->i_blkbits. The stripe unit (1 MiB) is
// a data layout, not a VFS block size; iomap writeback of 4 KiB pages then
// returns EINVAL and `cp` fails on close. Keep this at PAGE_SIZE.
blksize_t fuse_stat_blksize() { return 4096; }

#ifdef __APPLE__
void copy_stat(const aios_posix_stat& st, fuse_darwin_attr* out) {
  std::memset(out, 0, sizeof(*out));
  out->ino = st.ino;
  out->mode = st.mode;
  out->nlink = st.nlink;
  out->uid = st.uid;
  out->gid = st.gid;
  out->size = static_cast<off_t>(st.size);
  out->blksize = fuse_stat_blksize();
  out->blocks = posix_st_blocks(st.size);
  fill_times(st.atime_ns, &out->atimespec.tv_sec, &out->atimespec.tv_nsec);
  fill_times(st.mtime_ns, &out->mtimespec.tv_sec, &out->mtimespec.tv_nsec);
  fill_times(st.ctime_ns, &out->ctimespec.tv_sec, &out->ctimespec.tv_nsec);
  out->btimespec = out->ctimespec;
}

int posix_getattr(const char* path, fuse_darwin_attr* attr, struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    if (!fs) return -EIO;
    aios_posix_stat st{};
    int rc = load_stat_path_or_fh(fs, path, fi, &st);
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
  stbuf->st_blksize = fuse_stat_blksize();
  stbuf->st_blocks = posix_st_blocks(st.size);
  fill_times(st.atime_ns, &stbuf->st_atim.tv_sec, &stbuf->st_atim.tv_nsec);
  fill_times(st.mtime_ns, &stbuf->st_mtim.tv_sec, &stbuf->st_mtim.tv_nsec);
  fill_times(st.ctime_ns, &stbuf->st_ctim.tv_sec, &stbuf->st_ctim.tv_nsec);
}

int posix_getattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    if (!fs) return -EIO;
    aios_posix_stat st{};
    int rc = load_stat_path_or_fh(fs, path, fi, &st);
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
int posix_readdir(const char* path, void* buf, fuse_darwin_fill_dir_t filler, off_t /*offset*/,
                  struct fuse_file_info* fi, enum fuse_readdir_flags /*flags*/) {
  return guard([&] {
    auto* fs = fs_handle();
    const uint64_t dir_ino = path_ino(fs, path, fi);
    if (!dir_ino) return -ENOENT;
    /* Offset 0: one-shot directory (libfuse mode 1). Non-zero cookies hang ls. */
    uint64_t off = 0;
    aios_posix_dirent ents[64];
    while (true) {
      int n = aios_posix_readdir(fs, dir_ino, &off, ents, 64);
      if (n < 0) return n;
      if (n == 0) break;
      for (int i = 0; i < n; ++i) {
        fuse_darwin_attr e{};
        e.ino = ents[i].ino;
        e.mode = ents[i].mode;
        if (filler(buf, ents[i].name, &e, 0, static_cast<fuse_fill_dir_flags>(0))) return 0;
      }
    }
    return 0;
  });
}
#else
int posix_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t /*offset*/,
                  struct fuse_file_info* fi, enum fuse_readdir_flags /*flags*/) {
  return guard([&] {
    auto* fs = fs_handle();
    const uint64_t dir_ino = path_ino(fs, path, fi);
    if (!dir_ino) return -ENOENT;
    uint64_t off = 0;
    aios_posix_dirent ents[64];
    while (true) {
      int n = aios_posix_readdir(fs, dir_ino, &off, ents, 64);
      if (n < 0) return n;
      if (n == 0) break;
      for (int i = 0; i < n; ++i) {
        struct stat e{};
        e.st_ino = ents[i].ino;
        e.st_mode = ents[i].mode;
        if (filler(buf, ents[i].name, &e, 0, static_cast<fuse_fill_dir_flags>(0))) return 0;
      }
    }
    return 0;
  });
}
#endif

mode_t apply_umask(mode_t mode) {
  auto* ctx = fuse_get_context();
  const mode_t mask = ctx ? ctx->umask : 0;
  return static_cast<mode_t>(mode & ~mask);
}

int posix_mkdir(const char* path, mode_t mode) {
  return guard([&] {
    auto* fs = fs_handle();
    uint64_t parent = 0;
    std::string name;
    int rc = resolve_parent(fs, path, &parent, &name);
    if (rc) return rc;
    return aios_posix_mkdir(fs, parent, name.c_str(), static_cast<uint32_t>(apply_umask(mode)),
                            nullptr);
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
    rc = aios_posix_create(fs, parent, name.c_str(), static_cast<uint32_t>(apply_umask(mode)),
                           &st);
    if (rc) return rc;
    if (fi) {
      fi->fh = st.ino;
      fi->keep_cache = 1;
    }
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

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE AIOS_POSIX_RENAME_NOREPLACE
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE AIOS_POSIX_RENAME_EXCHANGE
#endif
#ifndef RENAME_WHITEOUT
#define RENAME_WHITEOUT AIOS_POSIX_RENAME_WHITEOUT
#endif

int posix_rename(const char* from, const char* to, unsigned int flags) {
  return guard([&] {
    auto* fs = fs_handle();
    uint64_t op = 0, np = 0;
    std::string on, nn;
    int rc = resolve_parent(fs, from, &op, &on);
    if (rc) return rc;
    rc = resolve_parent(fs, to, &np, &nn);
    if (rc) return rc;
    unsigned mapped = 0;
    if (flags & RENAME_NOREPLACE) mapped |= AIOS_POSIX_RENAME_NOREPLACE;
    if (flags & RENAME_EXCHANGE) mapped |= AIOS_POSIX_RENAME_EXCHANGE;
    if (flags & RENAME_WHITEOUT) mapped |= AIOS_POSIX_RENAME_WHITEOUT;
    return aios_posix_rename2(fs, op, on.c_str(), np, nn.c_str(), mapped);
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
    if (S_ISLNK(st.mode)) return -ELOOP;
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
    if (fi) {
      fi->fh = st.ino;
      fi->keep_cache = 1;
    }
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

int posix_ioctl(const char* /*path*/,
#if FUSE_USE_VERSION < 35
                int cmd,
#else
                unsigned int cmd,
#endif
                void* /*arg*/, struct fuse_file_info* fi, unsigned int flags, void* data) {
  return guard([&] {
    auto* fs = fs_handle();
    if (!fi) return -EIO;
#ifdef FUSE_IOCTL_DIR
    if (flags & FUSE_IOCTL_DIR) return -ENOTTY;
#endif
    (void)flags;
    if (static_cast<unsigned int>(cmd) != AIOS_IOC_PREFETCHV) return -ENOTTY;
    auto* req = static_cast<struct aios_prefetchv*>(data);
    if (!req) return -EFAULT;
    return aios_posix_prefetchv(fs, fi->fh, req);
  });
}

int posix_flush(const char* /*path*/, struct fuse_file_info* fi) {
  return posix_fsync(nullptr, 0, fi);
}

int posix_opendir(const char* path, struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    if (!S_ISDIR(st.mode)) return -ENOTDIR;
    if (fi) fi->fh = st.ino;
    return 0;
  });
}

// Directory records queued under a lease are committed here (POSIX: metadata is
// durable after fsync of the directory, not before).
int posix_fsyncdir(const char* path, int /*datasync*/, struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    const uint64_t ino = path_ino(fs, path, fi);
    if (!ino) return -ENOENT;
    return aios_posix_fsyncdir(fs, ino);
  });
}

int posix_release(const char* /*path*/, struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    if (!fi) return -EIO;
    int rc = aios_posix_fsync(fs, fi->fh);
    if (flock_owners().note_unlocked(fi->fh, fi->lock_owner)) {
      (void)aios_posix_flock(fs, fi->fh, LOCK_UN);
    }
    return rc;
  });
}

int posix_symlink(const char* target, const char* path) {
  return guard([&] {
    auto* fs = fs_handle();
    if (!target) return -EINVAL;
    uint64_t parent = 0;
    std::string name;
    int rc = resolve_parent(fs, path, &parent, &name);
    if (rc) return rc;
    return aios_posix_symlink(fs, parent, name.c_str(), target, nullptr);
  });
}

int posix_readlink(const char* path, char* buf, size_t size) {
  return guard([&] {
    auto* fs = fs_handle();
    if (!buf || size == 0) return -EINVAL;
    aios_posix_stat st{};
    int rc = lookup_path(fs, path, &st);
    if (rc) return rc;
    rc = aios_posix_readlink(fs, st.ino, buf, size);
    if (rc < 0) return rc;
    return 0;
  });
}

int posix_chmod(const char* path, mode_t mode, struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = load_stat_path_or_fh(fs, path, fi, &st);
    if (rc) return rc;
    aios_posix_stat ps{};
    ps.mode = static_cast<uint32_t>(mode);
    return aios_posix_setattr(fs, st.ino, &ps, AIOS_POSIX_SET_MODE);
  });
}

int posix_chown(const char* path, uid_t uid, gid_t gid, struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = load_stat_path_or_fh(fs, path, fi, &st);
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

int posix_utimens(const char* path, const struct timespec tv[2], struct fuse_file_info* fi) {
  return guard([&] {
    auto* fs = fs_handle();
    aios_posix_stat st{};
    int rc = load_stat_path_or_fh(fs, path, fi, &st);
    if (rc) return rc;
    aios_posix_stat ps{};
    uint32_t set = 0;
    struct timespec now_ts {};
    if (::clock_gettime(CLOCK_REALTIME, &now_ts) != 0) {
      now_ts.tv_sec = ::time(nullptr);
      now_ts.tv_nsec = 0;
    }
    const uint64_t now = static_cast<uint64_t>(now_ts.tv_sec) * 1000000000ull +
                         static_cast<uint64_t>(now_ts.tv_nsec);
    auto apply = [&](const struct timespec* ts, uint64_t* out_ns, uint32_t bit) {
      if (!ts || ts->tv_nsec == UTIME_NOW) {
        *out_ns = now;
        set |= bit;
      } else if (ts->tv_nsec != UTIME_OMIT) {
        *out_ns = static_cast<uint64_t>(ts->tv_sec) * 1000000000ull +
                  static_cast<uint64_t>(ts->tv_nsec);
        set |= bit;
      }
    };
    if (!tv) {
      ps.atime_ns = now;
      ps.mtime_ns = now;
      set = AIOS_POSIX_SET_ATIME | AIOS_POSIX_SET_MTIME;
    } else {
      apply(&tv[0], &ps.atime_ns, AIOS_POSIX_SET_ATIME);
      apply(&tv[1], &ps.mtime_ns, AIOS_POSIX_SET_MTIME);
    }
    if (!set) return 0;
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
    const uint64_t owner = fi ? fi->lock_owner : 0;
    const int cmd = op & (LOCK_SH | LOCK_EX | LOCK_UN);
    if (cmd == LOCK_UN) {
      // Another open of this inode may still hold the mount's single cluster lock.
      const bool was_holder = flock_owners().note_unlocked(ino, owner);
      if (!was_holder) return 0;
      return aios_posix_flock(fs, ino, op);
    }
    int rc = aios_posix_flock(fs, ino, op);
    if (rc == 0) flock_owners().note_locked(ino, owner);
    return rc;
  });
}

void* posix_init(struct fuse_conn_info* conn, struct fuse_config* cfg) {
  auto* ctx = fuse_get_context();
  auto* fs = ctx ? static_cast<aios_posix_fs*>(ctx->private_data) : nullptr;
  const unsigned io = aios_fuse_max_io(fs);
  if (conn) {
    conn->max_write = io;
    /* libfuse 3.10: must equal fuse_session_new's -o max_read (see aios-fuse). */
    conn->max_read = io;
    conn->max_readahead = io * 4;
#ifdef FUSE_CAP_WRITEBACK_CACHE
    if (conn->capable & FUSE_CAP_WRITEBACK_CACHE) conn->want |= FUSE_CAP_WRITEBACK_CACHE;
#endif
#ifdef FUSE_CAP_ASYNC_READ
    if (conn->capable & FUSE_CAP_ASYNC_READ) conn->want |= FUSE_CAP_ASYNC_READ;
#endif
#ifdef FUSE_CAP_MAX_PAGES
    if (conn->capable & FUSE_CAP_MAX_PAGES) conn->want |= FUSE_CAP_MAX_PAGES;
#endif
  }
  if (cfg) {
    cfg->kernel_cache = 1;
    // Directory nlink is kept correct in-core (cache_inode_locked). A 0s attr
    // timeout makes the writeback path GETATTR before every write(2), which
    // returns EINVAL on the kernel used in CI. mkdir still invalidates the
    // parent via fuse_dir_changed.
    cfg->attr_timeout = 1.0;
    cfg->entry_timeout = 1.0;
    /* Our inode numbers are stable and unique: hand them to the kernel instead of
     * libfuse's synthesized ones (hard links then share st_ino as they should). */
    cfg->use_ino = 1;
    cfg->readdir_ino = 1;
    /* Handlers that take fi must use fi->fh when path is NULL: libfuse skips
     * the path lookup for write/flush/fsync/release *and* for setattr/getattr
     * when the kernel sent FATTR_FH (writeback close). That also keeps
     * unlinked-but-open files usable. */
    cfg->nullpath_ok = 1;
  }
  return fs;
}

}  // namespace

unsigned aios_fuse_max_io(const aios_posix_fs* fs) {
  uint64_t su = aios_posix_stripe_unit(fs);
  if (su == 0 || su > 1024ull * 1024ull) su = 1024ull * 1024ull;
  return static_cast<unsigned>(su);
}

fuse_operations aios_fuse_operations() {
  fuse_operations ops{};
  ops.init = posix_init;
  ops.getattr = posix_getattr;
  ops.readdir = posix_readdir;
  ops.mkdir = posix_mkdir;
  ops.create = posix_create;
  ops.unlink = posix_unlink;
  ops.rmdir = posix_rmdir;
  ops.rename = posix_rename;
  ops.link = posix_link;
  ops.symlink = posix_symlink;
  ops.readlink = posix_readlink;
  ops.open = posix_open;
  ops.read = posix_read;
  ops.write = posix_write;
  ops.truncate = posix_truncate;
  ops.fsync = posix_fsync;
  ops.ioctl = posix_ioctl;
  ops.flush = posix_flush;
  ops.opendir = posix_opendir;
  ops.fsyncdir = posix_fsyncdir;
  ops.release = posix_release;
  ops.chmod = posix_chmod;
  ops.chown = posix_chown;
  ops.utimens = posix_utimens;
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
