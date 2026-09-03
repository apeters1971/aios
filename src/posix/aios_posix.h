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
  uint32_t uid;             /* default owner / caller when unset on thread */
  uint32_t gid;
  int rstat_interval_ms;    /* recursive dir stats flush; 0 disables; typical 60000.
                               The deferred inode flusher (see aios_posix_write) runs
                               regardless of this value. */
  unsigned flags;           /* AIOS_POSIX_F_* */
} aios_posix_config;

/* Commit every directory operation synchronously instead of leasing the
 * directory and batching its changelog appends (see aios_posix_fsyncdir). */
#define AIOS_POSIX_F_NOLEASE 0x1u

/* Per-request caller identity (thread-local for this mount). */
typedef struct aios_posix_cred {
  uint32_t uid;
  uint32_t gid;
} aios_posix_cred;

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
  uint64_t parent_ino; /* primary parent directory; 0 for root */
} aios_posix_stat;

typedef struct aios_posix_dirent {
  uint64_t ino;
  uint32_t mode; /* file type bits in mode (S_IF*) */
  char name[256];
} aios_posix_dirent;

/* Negative return = -errno. 0 = success unless noted. */

aios_posix_fs* aios_posix_mount(const aios_posix_config* cfg, int* err_out);
void aios_posix_unmount(aios_posix_fs* fs);

/* Directory operations (create/mkdir/unlink/rename/symlink/link) on a directory
 * this mount leases are acknowledged before the changelog record is on the
 * server; a flusher commits them within milliseconds. fsyncdir waits for the
 * directory's queue and returns (and clears) the error of a record that could
 * not be committed; sync does so for every directory. Both are no-ops with
 * AIOS_POSIX_F_NOLEASE. */
int aios_posix_fsyncdir(aios_posix_fs* fs, uint64_t dir_ino);
int aios_posix_sync(aios_posix_fs* fs);
uint64_t aios_posix_stripe_unit(const aios_posix_fs* fs);

/* Recompute dirty recursive directory stats (aios.r*). Also runs on the
 * rstat timer and on unmount when rstat_interval_ms > 0. */
void aios_posix_flush_rstats(aios_posix_fs* fs);

/* Thread-scoped caller for subsequent ops on this mount (gateways/FUSE/OFS).
 * When unset, mount config uid/gid are used. uid 0 bypasses mode checks. */
void aios_posix_set_caller(aios_posix_fs* fs, uint32_t uid, uint32_t gid);
void aios_posix_clear_caller(aios_posix_fs* fs);
aios_posix_cred aios_posix_get_caller(const aios_posix_fs* fs);

/* amode: R_OK / W_OK / X_OK (from unistd.h); F_OK checks existence only. */
int aios_posix_access(aios_posix_fs* fs, uint64_t ino, int amode);

int aios_posix_lookup(aios_posix_fs* fs, uint64_t parent, const char* name,
                      aios_posix_stat* st_out);
int aios_posix_getattr(aios_posix_fs* fs, uint64_t ino, aios_posix_stat* st_out);

/* readdir: fill up to max_entries starting at *offset (0-based).
 * Returns number of entries written (>=0), or -errno. Advances *offset.
 * *offset is a positional index into the directory's name-sorted listing,
 * which is re-read and re-sorted on every call. Entries created or removed
 * between calls may therefore be skipped or repeated; callers wanting a
 * stable listing should drain the directory in one pass (offset 0 .. done). */
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
/* Same, when the source is already known by inode (FUSE lowlevel). */
int aios_posix_link_ino(aios_posix_fs* fs, uint64_t ino, uint64_t new_parent,
                        const char* new_name);

/* rename flags (linux renameat2). EXCHANGE/WHITEOUT are rejected (-EINVAL). */
enum {
  AIOS_POSIX_RENAME_NOREPLACE = 1u,
  AIOS_POSIX_RENAME_EXCHANGE = 2u,
  AIOS_POSIX_RENAME_WHITEOUT = 4u,
};

/* Cross-directory rename uses a multi-object /txn compact rewrite of both
 * directory tips (see proto/posix_fuse.md). Same-directory rename is one changelog op. */
int aios_posix_rename(aios_posix_fs* fs, uint64_t old_parent, const char* old_name,
                      uint64_t new_parent, const char* new_name);
int aios_posix_rename2(aios_posix_fs* fs, uint64_t old_parent, const char* old_name,
                       uint64_t new_parent, const char* new_name, unsigned flags);

int aios_posix_symlink(aios_posix_fs* fs, uint64_t parent, const char* name, const char* target,
                       aios_posix_stat* st_out);
/* Writes a NUL-terminated target. size==0 returns needed bytes including NUL.
 * Otherwise returns bytes excluding NUL, or -ERANGE / -errno. */
int aios_posix_readlink(aios_posix_fs* fs, uint64_t ino, char* buf, size_t size);

int aios_posix_read(aios_posix_fs* fs, uint64_t ino, uint64_t offset, void* buf,
                    size_t len, size_t* out_len);
/* Data is durable on the cluster when write returns. The inode's size/mtime
 * update is deferred and batched: it is visible to this mount immediately, and
 * to other clients (other mounts, S3, XRootD, the kernel client) within ~100 ms
 * via the mount's background flusher, or as soon as aios_posix_fsync returns.
 * Gateways that must publish a complete file on close call aios_posix_fsync. */
int aios_posix_write(aios_posix_fs* fs, uint64_t ino, uint64_t offset, const void* buf,
                     size_t len, size_t* out_len);
int aios_posix_truncate(aios_posix_fs* fs, uint64_t ino, uint64_t size);
int aios_posix_setattr(aios_posix_fs* fs, uint64_t ino, const aios_posix_stat* st,
                       uint32_t to_set);
/* Flush the deferred size/mtime for ino (see aios_posix_write). */
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

/* Crash-consistent volume snapshot under posix/{vol}/.snap/{id}/.
 * Writes snap id (hex) into snap_id_out (NUL-terminated). Returns 0 or -errno.
 * Mutating ops return -EBUSY while the snapshot freeze is held. */
int aios_posix_snapshot(aios_posix_fs* fs, char* snap_id_out, size_t snap_id_len);

/* Like aios_posix_snapshot but limited to a volume-relative subtree path
 * (e.g. "/home/alice"). NULL, "", or "/" snapshots the whole volume. */
int aios_posix_snapshot_at(aios_posix_fs* fs, const char* path, char* snap_id_out,
                           size_t snap_id_len);

#ifdef __cplusplus
}
#endif
