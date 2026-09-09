#include "posix/fuse3_ll_ops.hpp"
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
#include <vector>

#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/param.h>
#else
#include <sys/statvfs.h>
#endif

#ifndef FUSE_SET_ATTR_ATIME_NOW
#define FUSE_SET_ATTR_ATIME_NOW 0
#endif
#ifndef FUSE_SET_ATTR_MTIME_NOW
#define FUSE_SET_ATTR_MTIME_NOW 0
#endif

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE AIOS_POSIX_RENAME_NOREPLACE
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE AIOS_POSIX_RENAME_EXCHANGE
#endif
#ifndef RENAME_WHITEOUT
#define RENAME_WHITEOUT AIOS_POSIX_RENAME_WHITEOUT
#endif

namespace {

constexpr double kTimeout = 1.0;

#ifdef __APPLE__
using Attr = fuse_darwin_attr;
using Entry = fuse_darwin_entry_param;
#else
using Attr = struct stat;
using Entry = fuse_entry_param;
#endif

aios::posix::FlockOwners& flock_owners() {
  static aios::posix::FlockOwners owners;
  return owners;
}

uint64_t now_ns() {
  struct timespec ts {};
  if (::clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    ts.tv_sec = ::time(nullptr);
    ts.tv_nsec = 0;
  }
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

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
void copy_stat(const aios_posix_stat& st, Attr* out) {
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
#else
void copy_stat(const aios_posix_stat& st, Attr* out) {
  std::memset(out, 0, sizeof(*out));
  out->st_ino = st.ino;
  out->st_mode = st.mode;
  out->st_nlink = st.nlink;
  out->st_uid = st.uid;
  out->st_gid = st.gid;
  out->st_size = static_cast<off_t>(st.size);
  out->st_blksize = fuse_stat_blksize();
  out->st_blocks = posix_st_blocks(st.size);
  fill_times(st.atime_ns, &out->st_atim.tv_sec, &out->st_atim.tv_nsec);
  fill_times(st.mtime_ns, &out->st_mtim.tv_sec, &out->st_mtim.tv_nsec);
  fill_times(st.ctime_ns, &out->st_ctim.tv_sec, &out->st_ctim.tv_nsec);
}
#endif

void fill_entry(Entry* e, const aios_posix_stat& st) {
  std::memset(e, 0, sizeof(*e));
  e->ino = st.ino;
  e->generation = 0;
  copy_stat(st, &e->attr);
  // Directory nlink/mtime change on mkdir/rmdir; a 1s cache would hide that.
  e->attr_timeout = S_ISDIR(st.mode) ? 0.0 : kTimeout;
  e->entry_timeout = kTimeout;
}

aios_posix_fs* fs_from(fuse_req_t req) {
  auto* fs = static_cast<aios_posix_fs*>(fuse_req_userdata(req));
  const struct fuse_ctx* ctx = fuse_req_ctx(req);
  if (fs && ctx) {
    aios_posix_set_caller(fs, static_cast<uint32_t>(ctx->uid), static_cast<uint32_t>(ctx->gid));
  }
  return fs;
}

mode_t apply_umask(fuse_req_t req, mode_t mode) {
  const struct fuse_ctx* ctx = fuse_req_ctx(req);
  const mode_t mask = ctx ? ctx->umask : 0;
  return static_cast<mode_t>(mode & ~mask);
}

template <typename F>
void guarded(fuse_req_t req, F&& f) {
  try {
    f();
  } catch (...) {
    fuse_reply_err(req, EIO);
  }
}

void reply_err(fuse_req_t req, int rc) {
  fuse_reply_err(req, rc < 0 ? -rc : rc);
}

int load_stat(aios_posix_fs* fs, fuse_ino_t ino, aios_posix_stat* st) {
  if (!fs) return -EIO;
  return aios_posix_getattr(fs, ino, st);
}

void reply_entry_stat(fuse_req_t req, const aios_posix_stat& st) {
  Entry e{};
  fill_entry(&e, st);
  fuse_reply_entry(req, &e);
}

void reply_attr_stat(fuse_req_t req, const aios_posix_stat& st) {
  Attr attr{};
  copy_stat(st, &attr);
  fuse_reply_attr(req, &attr, S_ISDIR(st.mode) ? 0.0 : kTimeout);
}

void ll_init(void* userdata, struct fuse_conn_info* conn) {
  auto* fs = static_cast<aios_posix_fs*>(userdata);
  const unsigned io = aios_fuse_ll_max_io(fs);
  if (!conn) return;
  conn->max_write = io;
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
#ifdef FUSE_CAP_FLOCK_LOCKS
  if (conn->capable & FUSE_CAP_FLOCK_LOCKS) conn->want |= FUSE_CAP_FLOCK_LOCKS;
#endif
}

void ll_destroy(void* /*userdata*/) {}

void ll_lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !name) {
      fuse_reply_err(req, EIO);
      return;
    }
    /* Cluster .aios markers live on real disks. Stating one on this mount
     * from aiosd's scanner deadlocks (FUSE HTTP → same process). */
    if (std::strcmp(name, ".aios") == 0) {
      Entry e{};
      e.entry_timeout = kTimeout;
      fuse_reply_entry(req, &e);
      return;
    }
    aios_posix_stat st{};
    int rc = 0;
    if (std::strcmp(name, ".") == 0) {
      rc = load_stat(fs, parent, &st);
    } else if (std::strcmp(name, "..") == 0) {
      rc = load_stat(fs, parent, &st);
      if (rc == 0 && st.parent_ino != 0) rc = load_stat(fs, st.parent_ino, &st);
    } else {
      rc = aios_posix_lookup(fs, parent, name, &st);
    }
    if (rc == -ENOENT) {
      Entry e{};
      e.entry_timeout = kTimeout;
      fuse_reply_entry(req, &e);
      return;
    }
    if (rc) {
      reply_err(req, rc);
      return;
    }
    reply_entry_stat(req, st);
  });
}

void ll_forget(fuse_req_t req, fuse_ino_t /*ino*/, uint64_t /*nlookup*/) {
  fuse_reply_none(req);
}

void ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* /*fi*/) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    aios_posix_stat st{};
    int rc = load_stat(fs, ino, &st);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    reply_attr_stat(req, st);
  });
}

int apply_setattr(aios_posix_fs* fs, uint64_t ino, const Attr* attr, int to_set) {
  if (to_set & FUSE_SET_ATTR_SIZE) {
#ifdef __APPLE__
    const uint64_t size = static_cast<uint64_t>(attr->size);
#else
    const uint64_t size = static_cast<uint64_t>(attr->st_size);
#endif
    int rc = aios_posix_truncate(fs, ino, size);
    if (rc) return rc;
  }
  aios_posix_stat ps{};
  uint32_t set = 0;
  if (to_set & FUSE_SET_ATTR_MODE) {
#ifdef __APPLE__
    ps.mode = attr->mode;
#else
    ps.mode = attr->st_mode;
#endif
    set |= AIOS_POSIX_SET_MODE;
  }
  if (to_set & FUSE_SET_ATTR_UID) {
#ifdef __APPLE__
    ps.uid = attr->uid;
#else
    ps.uid = attr->st_uid;
#endif
    set |= AIOS_POSIX_SET_UID;
  }
  if (to_set & FUSE_SET_ATTR_GID) {
#ifdef __APPLE__
    ps.gid = attr->gid;
#else
    ps.gid = attr->st_gid;
#endif
    set |= AIOS_POSIX_SET_GID;
  }
  const uint64_t now = now_ns();
  auto set_time = [&](uint64_t* out, uint32_t bit, bool now_bit, const struct timespec* ts) {
    if (now_bit) {
      *out = now;
      set |= bit;
    } else if (ts) {
      *out = static_cast<uint64_t>(ts->tv_sec) * 1000000000ull + static_cast<uint64_t>(ts->tv_nsec);
      set |= bit;
    }
  };
#ifdef __APPLE__
  if (to_set & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_ATIME_NOW)) {
    set_time(&ps.atime_ns, AIOS_POSIX_SET_ATIME, (to_set & FUSE_SET_ATTR_ATIME_NOW) != 0,
             &attr->atimespec);
  }
  if (to_set & (FUSE_SET_ATTR_MTIME | FUSE_SET_ATTR_MTIME_NOW)) {
    set_time(&ps.mtime_ns, AIOS_POSIX_SET_MTIME, (to_set & FUSE_SET_ATTR_MTIME_NOW) != 0,
             &attr->mtimespec);
  }
#else
  if (to_set & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_ATIME_NOW)) {
    set_time(&ps.atime_ns, AIOS_POSIX_SET_ATIME, (to_set & FUSE_SET_ATTR_ATIME_NOW) != 0,
             &attr->st_atim);
  }
  if (to_set & (FUSE_SET_ATTR_MTIME | FUSE_SET_ATTR_MTIME_NOW)) {
    set_time(&ps.mtime_ns, AIOS_POSIX_SET_MTIME, (to_set & FUSE_SET_ATTR_MTIME_NOW) != 0,
             &attr->st_mtim);
  }
#endif
  if (set) {
    int rc = aios_posix_setattr(fs, ino, &ps, set);
    if (rc) return rc;
  }
  return 0;
}

void ll_setattr(fuse_req_t req, fuse_ino_t ino, Attr* attr, int to_set, struct fuse_file_info* /*fi*/) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !attr) {
      fuse_reply_err(req, EIO);
      return;
    }
    int rc = apply_setattr(fs, ino, attr, to_set);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    aios_posix_stat st{};
    rc = load_stat(fs, ino, &st);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    reply_attr_stat(req, st);
  });
}

void ll_readlink(fuse_req_t req, fuse_ino_t ino) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs) {
      fuse_reply_err(req, EIO);
      return;
    }
    int need = aios_posix_readlink(fs, ino, nullptr, 0);
    if (need < 0) {
      reply_err(req, need);
      return;
    }
    if (need == 0) {
      fuse_reply_readlink(req, "");
      return;
    }
    std::vector<char> buf(static_cast<size_t>(need));
    int rc = aios_posix_readlink(fs, ino, buf.data(), buf.size());
    if (rc < 0) {
      reply_err(req, rc);
      return;
    }
    fuse_reply_readlink(req, buf.data());
  });
}

void ll_mkdir(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !name) {
      fuse_reply_err(req, EIO);
      return;
    }
    aios_posix_stat st{};
    int rc = aios_posix_mkdir(fs, parent, name, static_cast<uint32_t>(apply_umask(req, mode)), &st);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    reply_entry_stat(req, st);
  });
}

void ll_unlink(fuse_req_t req, fuse_ino_t parent, const char* name) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !name) {
      fuse_reply_err(req, EIO);
      return;
    }
    reply_err(req, aios_posix_unlink(fs, parent, name));
  });
}

void ll_rmdir(fuse_req_t req, fuse_ino_t parent, const char* name) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !name) {
      fuse_reply_err(req, EIO);
      return;
    }
    reply_err(req, aios_posix_rmdir(fs, parent, name));
  });
}

void ll_symlink(fuse_req_t req, const char* link, fuse_ino_t parent, const char* name) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !link || !name) {
      fuse_reply_err(req, EINVAL);
      return;
    }
    aios_posix_stat st{};
    int rc = aios_posix_symlink(fs, parent, name, link, &st);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    reply_entry_stat(req, st);
  });
}

void ll_rename(fuse_req_t req, fuse_ino_t parent, const char* name, fuse_ino_t newparent,
               const char* newname, unsigned int flags) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !name || !newname) {
      fuse_reply_err(req, EIO);
      return;
    }
    unsigned mapped = 0;
    if (flags & RENAME_NOREPLACE) mapped |= AIOS_POSIX_RENAME_NOREPLACE;
    if (flags & RENAME_EXCHANGE) mapped |= AIOS_POSIX_RENAME_EXCHANGE;
    if (flags & RENAME_WHITEOUT) mapped |= AIOS_POSIX_RENAME_WHITEOUT;
    reply_err(req, aios_posix_rename2(fs, parent, name, newparent, newname, mapped));
  });
}

void ll_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent, const char* newname) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !newname) {
      fuse_reply_err(req, EIO);
      return;
    }
    int rc = aios_posix_link_ino(fs, ino, newparent, newname);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    aios_posix_stat st{};
    rc = load_stat(fs, ino, &st);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    reply_entry_stat(req, st);
  });
}

void ll_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !fi) {
      fuse_reply_err(req, EIO);
      return;
    }
    aios_posix_stat st{};
    int rc = load_stat(fs, ino, &st);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    if (S_ISLNK(st.mode)) {
      fuse_reply_err(req, ELOOP);
      return;
    }
    if (!S_ISREG(st.mode)) {
      fuse_reply_err(req, EISDIR);
      return;
    }
    int amode = 0;
    const int acc = fi->flags & O_ACCMODE;
    if (acc == O_RDONLY || acc == O_RDWR) amode |= R_OK;
    if (acc == O_WRONLY || acc == O_RDWR) amode |= W_OK;
    if (amode) {
      rc = aios_posix_access(fs, ino, amode);
      if (rc) {
        reply_err(req, rc);
        return;
      }
    }
    fi->fh = ino;
    fi->keep_cache = 1;
    fuse_reply_open(req, fi);
  });
}

void ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info* fi) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs) {
      fuse_reply_err(req, EIO);
      return;
    }
    const uint64_t file = (fi && fi->fh) ? fi->fh : ino;
    std::vector<char> buf(size);
    size_t out = 0;
    int rc = aios_posix_read(fs, file, static_cast<uint64_t>(off), buf.data(), size, &out);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    fuse_reply_buf(req, buf.data(), out);
  });
}

void ll_write(fuse_req_t req, fuse_ino_t ino, const char* buf, size_t size, off_t off,
              struct fuse_file_info* fi) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !buf) {
      fuse_reply_err(req, EIO);
      return;
    }
    const uint64_t file = (fi && fi->fh) ? fi->fh : ino;
    size_t out = 0;
    int rc = aios_posix_write(fs, file, static_cast<uint64_t>(off), buf, size, &out);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    fuse_reply_write(req, out);
  });
}

void ll_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs) {
      fuse_reply_err(req, EIO);
      return;
    }
    const uint64_t file = (fi && fi->fh) ? fi->fh : ino;
    reply_err(req, aios_posix_fsync(fs, file));
  });
}

void ll_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !fi) {
      fuse_reply_err(req, EIO);
      return;
    }
    const uint64_t file = fi->fh ? fi->fh : ino;
    int rc = aios_posix_fsync(fs, file);
    if (flock_owners().note_unlocked(file, fi->lock_owner)) {
      (void)aios_posix_flock(fs, file, LOCK_UN);
    }
    reply_err(req, rc);
  });
}

void ll_fsync(fuse_req_t req, fuse_ino_t ino, int /*datasync*/, struct fuse_file_info* fi) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs) {
      fuse_reply_err(req, EIO);
      return;
    }
    const uint64_t file = (fi && fi->fh) ? fi->fh : ino;
    reply_err(req, aios_posix_fsync(fs, file));
  });
}

void ll_ioctl(fuse_req_t req, fuse_ino_t ino,
#if FUSE_USE_VERSION < 35
              int cmd,
#else
              unsigned int cmd,
#endif
              void* /*arg*/, struct fuse_file_info* fi, unsigned flags, const void* in_buf,
              size_t in_bufsz, size_t /*out_bufsz*/) {
  guarded(req, [&] {
#ifdef FUSE_IOCTL_DIR
    if (flags & FUSE_IOCTL_DIR) {
      fuse_reply_err(req, ENOTTY);
      return;
    }
#endif
    (void)flags;
    if (static_cast<unsigned int>(cmd) != AIOS_IOC_PREFETCHV) {
      fuse_reply_err(req, ENOTTY);
      return;
    }
    if (!in_buf || in_bufsz < sizeof(struct aios_prefetchv)) {
      fuse_reply_err(req, EINVAL);
      return;
    }
    auto* fs = fs_from(req);
    if (!fs) {
      fuse_reply_err(req, EIO);
      return;
    }
    const uint64_t file = (fi && fi->fh) ? fi->fh : ino;
    const auto* pref = static_cast<const struct aios_prefetchv*>(in_buf);
    int rc = aios_posix_prefetchv(fs, file, pref);
    if (rc < 0) {
      fuse_reply_err(req, -rc);
      return;
    }
    fuse_reply_ioctl(req, 0, nullptr, 0);
  });
}

void ll_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !fi) {
      fuse_reply_err(req, EIO);
      return;
    }
    aios_posix_stat st{};
    int rc = load_stat(fs, ino, &st);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    if (!S_ISDIR(st.mode)) {
      fuse_reply_err(req, ENOTDIR);
      return;
    }
    fi->fh = ino;
    fuse_reply_open(req, fi);
  });
}

void ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info* fi) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs) {
      fuse_reply_err(req, EIO);
      return;
    }
    const uint64_t dir = (fi && fi->fh) ? fi->fh : ino;
    std::vector<char> buf(size);
    size_t used = 0;
    uint64_t pos = static_cast<uint64_t>(off);
    aios_posix_dirent ents[32];
    while (used < size) {
      const uint64_t start = pos;
      int n = aios_posix_readdir(fs, dir, &pos, ents, 32);
      if (n < 0) {
        reply_err(req, n);
        return;
      }
      if (n == 0) break;
      bool full = false;
      for (int i = 0; i < n; ++i) {
        Attr st{};
#ifdef __APPLE__
        st.ino = ents[i].ino;
        st.mode = ents[i].mode;
#else
        st.st_ino = ents[i].ino;
        st.st_mode = ents[i].mode;
#endif
        const off_t next = static_cast<off_t>(start + static_cast<uint64_t>(i) + 1);
        const size_t entsize =
            fuse_add_direntry(req, buf.data() + used, size - used, ents[i].name, &st, next);
        if (entsize > size - used) {
          full = true;
          break;
        }
        used += entsize;
      }
      if (full) break;
    }
    fuse_reply_buf(req, buf.data(), used);
  });
}

void ll_releasedir(fuse_req_t req, fuse_ino_t /*ino*/, struct fuse_file_info* /*fi*/) {
  fuse_reply_err(req, 0);
}

void ll_fsyncdir(fuse_req_t req, fuse_ino_t ino, int /*datasync*/, struct fuse_file_info* fi) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs) {
      fuse_reply_err(req, EIO);
      return;
    }
    const uint64_t dir = (fi && fi->fh) ? fi->fh : ino;
    reply_err(req, aios_posix_fsyncdir(fs, dir));
  });
}

#ifdef __APPLE__
void ll_statfs(fuse_req_t req, fuse_ino_t /*ino*/) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs) {
      fuse_reply_err(req, EIO);
      return;
    }
    aios_posix_statvfs st{};
    int rc = aios_posix_statfs(fs, &st);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    struct statfs stbuf {};
    stbuf.f_bsize = static_cast<uint32_t>(st.bsize);
    stbuf.f_iosize = static_cast<uint32_t>(st.bsize);
    stbuf.f_blocks = st.blocks;
    stbuf.f_bfree = st.bfree;
    stbuf.f_bavail = st.bavail;
    stbuf.f_files = static_cast<uint32_t>(st.files);
    stbuf.f_ffree = static_cast<uint32_t>(st.ffree);
    fuse_reply_statfs(req, &stbuf);
  });
}
#else
void ll_statfs(fuse_req_t req, fuse_ino_t /*ino*/) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs) {
      fuse_reply_err(req, EIO);
      return;
    }
    aios_posix_statvfs st{};
    int rc = aios_posix_statfs(fs, &st);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    struct statvfs stbuf {};
    stbuf.f_bsize = st.bsize;
    stbuf.f_frsize = st.bsize;
    stbuf.f_blocks = st.blocks;
    stbuf.f_bfree = st.bfree;
    stbuf.f_bavail = st.bavail;
    stbuf.f_files = st.files;
    stbuf.f_ffree = st.ffree;
    stbuf.f_namemax = st.namemax;
    fuse_reply_statfs(req, &stbuf);
  });
}
#endif

void reply_xattr(fuse_req_t req, int rc, char* buf, size_t size) {
  if (rc < 0) {
    reply_err(req, rc);
    return;
  }
  if (size == 0) {
    fuse_reply_xattr(req, static_cast<size_t>(rc));
    return;
  }
  fuse_reply_buf(req, buf, static_cast<size_t>(rc));
}

#ifdef __APPLE__
void ll_setxattr(fuse_req_t req, fuse_ino_t ino, const char* name, const char* value, size_t size,
                 int flags, uint32_t /*position*/) {
#else
void ll_setxattr(fuse_req_t req, fuse_ino_t ino, const char* name, const char* value, size_t size,
                 int flags) {
#endif
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !name) {
      fuse_reply_err(req, EIO);
      return;
    }
    reply_err(req, aios_posix_setxattr(fs, ino, name, value, size, flags));
  });
}

#ifdef __APPLE__
void ll_getxattr(fuse_req_t req, fuse_ino_t ino, const char* name, size_t size, uint32_t /*position*/) {
#else
void ll_getxattr(fuse_req_t req, fuse_ino_t ino, const char* name, size_t size) {
#endif
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !name) {
      fuse_reply_err(req, EIO);
      return;
    }
    if (size == 0) {
      int rc = aios_posix_getxattr(fs, ino, name, nullptr, 0);
      reply_xattr(req, rc, nullptr, 0);
      return;
    }
    std::vector<char> buf(size);
    int rc = aios_posix_getxattr(fs, ino, name, buf.data(), size);
    reply_xattr(req, rc, buf.data(), size);
  });
}

void ll_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs) {
      fuse_reply_err(req, EIO);
      return;
    }
    if (size == 0) {
      int rc = aios_posix_listxattr(fs, ino, nullptr, 0);
      reply_xattr(req, rc, nullptr, 0);
      return;
    }
    std::vector<char> buf(size);
    int rc = aios_posix_listxattr(fs, ino, buf.data(), size);
    reply_xattr(req, rc, buf.data(), size);
  });
}

void ll_removexattr(fuse_req_t req, fuse_ino_t ino, const char* name) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !name) {
      fuse_reply_err(req, EIO);
      return;
    }
    reply_err(req, aios_posix_removexattr(fs, ino, name));
  });
}

void ll_access(fuse_req_t req, fuse_ino_t ino, int mask) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs) {
      fuse_reply_err(req, EIO);
      return;
    }
    reply_err(req, aios_posix_access(fs, ino, mask));
  });
}

void ll_create(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode,
               struct fuse_file_info* fi) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs || !name || !fi) {
      fuse_reply_err(req, EIO);
      return;
    }
    aios_posix_stat st{};
    int rc =
        aios_posix_create(fs, parent, name, static_cast<uint32_t>(apply_umask(req, mode)), &st);
    if (rc) {
      reply_err(req, rc);
      return;
    }
    fi->fh = st.ino;
    fi->keep_cache = 1;
    Entry e{};
    fill_entry(&e, st);
    fuse_reply_create(req, &e, fi);
  });
}

void ll_flock(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi, int op) {
  guarded(req, [&] {
    auto* fs = fs_from(req);
    if (!fs) {
      fuse_reply_err(req, EIO);
      return;
    }
    const uint64_t file = (fi && fi->fh) ? fi->fh : ino;
    const uint64_t owner = fi ? fi->lock_owner : 0;
    const int cmd = op & (LOCK_SH | LOCK_EX | LOCK_UN);
    if (cmd == LOCK_UN) {
      const bool was_holder = flock_owners().note_unlocked(file, owner);
      if (!was_holder) {
        fuse_reply_err(req, 0);
        return;
      }
      reply_err(req, aios_posix_flock(fs, file, op));
      return;
    }
    int rc = aios_posix_flock(fs, file, op);
    if (rc == 0) flock_owners().note_locked(file, owner);
    reply_err(req, rc);
  });
}

}  // namespace

unsigned aios_fuse_ll_max_io(const aios_posix_fs* fs) {
  uint64_t su = aios_posix_stripe_unit(fs);
  if (su == 0 || su > 1024ull * 1024ull) su = 1024ull * 1024ull;
  return static_cast<unsigned>(su);
}

fuse_lowlevel_ops aios_fuse_ll_operations() {
  fuse_lowlevel_ops ops{};
  ops.init = ll_init;
  ops.destroy = ll_destroy;
  ops.lookup = ll_lookup;
  ops.forget = ll_forget;
  ops.getattr = ll_getattr;
  ops.setattr = ll_setattr;
  ops.readlink = ll_readlink;
  ops.mkdir = ll_mkdir;
  ops.unlink = ll_unlink;
  ops.rmdir = ll_rmdir;
  ops.symlink = ll_symlink;
  ops.rename = ll_rename;
  ops.link = ll_link;
  ops.open = ll_open;
  ops.read = ll_read;
  ops.write = ll_write;
  ops.flush = ll_flush;
  ops.release = ll_release;
  ops.fsync = ll_fsync;
  ops.ioctl = ll_ioctl;
  ops.opendir = ll_opendir;
  ops.readdir = ll_readdir;
  ops.releasedir = ll_releasedir;
  ops.fsyncdir = ll_fsyncdir;
  ops.statfs = ll_statfs;
  ops.setxattr = ll_setxattr;
  ops.getxattr = ll_getxattr;
  ops.listxattr = ll_listxattr;
  ops.removexattr = ll_removexattr;
  ops.access = ll_access;
  ops.create = ll_create;
  ops.flock = ll_flock;
  return ops;
}
