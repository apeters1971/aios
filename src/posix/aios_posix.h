#pragma once

/* C-portable POSIX filesystem ABI over AIOS objects.
 * Root directory is always inode 1. Designed so a future kernel port can
 * call the same surface from C without Boost/STL in this header.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aios_posix_fs aios_posix_fs;

typedef struct aios_posix_config {
  const char* endpoint;     /* HOST:PORT */
  const char* cluster_key;  /* required */
  const char* volume;       /* default "default" */
  const char* app_label;    /* optional */
  uint64_t stripe_unit;     /* 0 => 1 MiB */
  uint32_t stripe_width;    /* 0 => 4 (max in-flight chunk ops) */
  uint32_t uid;             /* default owner for creates */
  uint32_t gid;
} aios_posix_config;

typedef struct aios_posix_stat {
  uint64_t ino;
  uint32_t mode; /* POSIX mode including S_IF* */
  uint32_t nlink;
  uint32_t uid;
  uint32_t gid;
  uint64_t size;
  uint64_t atime_ns;
  uint64_t mtime_ns;
  uint64_t ctime_ns;
  uint64_t stripe_unit;
  uint32_t stripe_width;
} aios_posix_stat;

typedef struct aios_posix_dirent {
  uint64_t ino;
  uint32_t mode; /* file type bits in mode (S_IF*) */
  char name[256];
} aios_posix_dirent;

/* Negative return = -errno. 0 = success unless noted. */

aios_posix_fs* aios_posix_mount(const aios_posix_config* cfg, int* err_out);
void aios_posix_unmount(aios_posix_fs* fs);

int aios_posix_lookup(aios_posix_fs* fs, uint64_t parent, const char* name,
                      aios_posix_stat* st_out);
int aios_posix_getattr(aios_posix_fs* fs, uint64_t ino, aios_posix_stat* st_out);

/* readdir: fill up to max_entries starting at *offset (0-based).
 * Returns number of entries written (>=0), or -errno. Advances *offset. */
int aios_posix_readdir(aios_posix_fs* fs, uint64_t ino, uint64_t* offset,
                       aios_posix_dirent* buf, size_t max_entries);

int aios_posix_mkdir(aios_posix_fs* fs, uint64_t parent, const char* name, uint32_t mode,
                     aios_posix_stat* st_out);
int aios_posix_create(aios_posix_fs* fs, uint64_t parent, const char* name, uint32_t mode,
                      aios_posix_stat* st_out);
int aios_posix_unlink(aios_posix_fs* fs, uint64_t parent, const char* name);
int aios_posix_rmdir(aios_posix_fs* fs, uint64_t parent, const char* name);

/* Hard link. Directories are rejected (-EPERM). Cross-directory is best-effort. */
int aios_posix_link(aios_posix_fs* fs, uint64_t old_parent, const char* old_name,
                    uint64_t new_parent, const char* new_name);

/* Cross-directory rename is best-effort non-atomic (see proto/posix_fuse.md). */
int aios_posix_rename(aios_posix_fs* fs, uint64_t old_parent, const char* old_name,
                      uint64_t new_parent, const char* new_name);

int aios_posix_read(aios_posix_fs* fs, uint64_t ino, uint64_t offset, void* buf,
                    size_t len, size_t* out_len);
int aios_posix_write(aios_posix_fs* fs, uint64_t ino, uint64_t offset, const void* buf,
                     size_t len, size_t* out_len);
int aios_posix_truncate(aios_posix_fs* fs, uint64_t ino, uint64_t size);
int aios_posix_setattr(aios_posix_fs* fs, uint64_t ino, const aios_posix_stat* st,
                       uint32_t to_set);
int aios_posix_fsync(aios_posix_fs* fs, uint64_t ino);

/* to_set bits for setattr */
enum {
  AIOS_POSIX_SET_MODE = 1u << 0,
  AIOS_POSIX_SET_UID = 1u << 1,
  AIOS_POSIX_SET_GID = 1u << 2,
  AIOS_POSIX_SET_SIZE = 1u << 3,
  AIOS_POSIX_SET_MTIME = 1u << 4,
  AIOS_POSIX_SET_ATIME = 1u << 5,
};

typedef struct aios_posix_statvfs {
  uint64_t blocks;
  uint64_t bfree;
  uint64_t bavail;
  uint64_t files;
  uint64_t ffree;
  uint32_t bsize;
  uint32_t namemax;
} aios_posix_statvfs;

int aios_posix_statfs(aios_posix_fs* fs, aios_posix_statvfs* st_out);

/* Extended attributes (stored in inode JSON, values opaque bytes).
 * flags: 0, or AIOS_POSIX_XATTR_CREATE / AIOS_POSIX_XATTR_REPLACE.
 * get/list: size==0 returns required byte length; else write into buf or -ERANGE. */
enum {
  AIOS_POSIX_XATTR_CREATE = 1,
  AIOS_POSIX_XATTR_REPLACE = 2,
};

int aios_posix_setxattr(aios_posix_fs* fs, uint64_t ino, const char* name, const void* value,
                        size_t size, int flags);
int aios_posix_getxattr(aios_posix_fs* fs, uint64_t ino, const char* name, void* value,
                        size_t size);
int aios_posix_listxattr(aios_posix_fs* fs, uint64_t ino, char* list, size_t size);
int aios_posix_removexattr(aios_posix_fs* fs, uint64_t ino, const char* name);

/* Advisory flock via AIOS exclusive lock on the inode object.
 * op uses flock(2) bits: LOCK_SH, LOCK_EX, LOCK_UN, optionally OR LOCK_NB.
 * LOCK_SH is implemented as exclusive (cluster locks are exclusive-only). */
int aios_posix_flock(aios_posix_fs* fs, uint64_t ino, int op);

#ifdef __cplusplus
}
#endif
