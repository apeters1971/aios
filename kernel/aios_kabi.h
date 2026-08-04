/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Shared kernel ↔ userspace ABI for the aiosfs prototype.
 * Mirrors src/posix/aios_posix.h ops; userspace aios-kbridge maps these
 * onto libaios_posix. Intended for AlmaLinux 9 / RHEL 9 (kernel 5.14).
 */
#ifndef AIOS_KABI_H
#define AIOS_KABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AIOS_KABI_MAGIC 0x41494F53u /* 'AIOS' */
#define AIOS_KABI_VERSION 1
#define AIOS_KABI_NAME_MAX 255
#define AIOS_KABI_DEV_NAME "aios_bridge"
#define AIOS_KABI_MAX_PAYLOAD (1024u * 1024u)

enum aios_kabi_opcode {
  AIOS_OP_MOUNT = 1,
  AIOS_OP_UMOUNT = 2,
  AIOS_OP_LOOKUP = 3,
  AIOS_OP_GETATTR = 4,
  AIOS_OP_READDIR = 5,
  AIOS_OP_CREATE = 6,
  AIOS_OP_MKDIR = 7,
  AIOS_OP_UNLINK = 8,
  AIOS_OP_RMDIR = 9,
  AIOS_OP_RENAME = 10,
  AIOS_OP_READ = 11,
  AIOS_OP_WRITE = 12,
  AIOS_OP_TRUNCATE = 13,
  AIOS_OP_SETATTR = 14,
  AIOS_OP_FSYNC = 15,
  AIOS_OP_STATFS = 16,
  AIOS_OP_LINK = 17,
};

/* Wire-format inode attributes (packed, LE on the wire as host for Alma9 x86_64). */
struct aios_kabi_stat {
  uint64_t ino;
  uint32_t mode;
  uint32_t nlink;
  uint32_t uid;
  uint32_t gid;
  uint64_t size;
  uint64_t atime_ns;
  uint64_t mtime_ns;
  uint64_t ctime_ns;
};

struct aios_kabi_dirent {
  uint64_t ino;
  uint32_t mode;
  uint32_t _pad;
  char name[AIOS_KABI_NAME_MAX + 1];
};

struct aios_kabi_statvfs {
  uint64_t blocks;
  uint64_t bfree;
  uint64_t bavail;
  uint64_t files;
  uint64_t ffree;
  uint32_t bsize;
  uint32_t namemax;
};

/* Request header; payload follows immediately (length = hdr.payload_len). */
struct aios_kabi_req_hdr {
  uint32_t magic;
  uint32_t version;
  uint64_t unique;
  uint32_t opcode;
  uint32_t payload_len;
  int32_t mount_id; /* set by kernel for session-scoped ops */
  int32_t _pad;
};

/* Reply header; payload follows. result is 0 or -errno. */
struct aios_kabi_rep_hdr {
  uint32_t magic;
  uint32_t version;
  uint64_t unique;
  int32_t result;
  uint32_t payload_len;
};

/* AIOS_OP_MOUNT payload (request). */
struct aios_kabi_mount_in {
  char endpoint[256];
  char cluster_key[256];
  char volume[64];
  char app_label[64];
  uint64_t stripe_unit;
  uint32_t stripe_width;
  uint32_t uid;
  uint32_t gid;
};

/* AIOS_OP_MOUNT payload (reply). */
struct aios_kabi_mount_out {
  int32_t mount_id;
  uint32_t _pad;
  struct aios_kabi_stat root;
};

struct aios_kabi_lookup_in {
  uint64_t parent;
  char name[AIOS_KABI_NAME_MAX + 1];
};

struct aios_kabi_ino_in {
  uint64_t ino;
};

struct aios_kabi_readdir_in {
  uint64_t ino;
  uint64_t offset;
  uint32_t max_entries;
  uint32_t _pad;
};

struct aios_kabi_readdir_out {
  uint64_t next_offset;
  uint32_t count;
  uint32_t _pad;
  /* followed by count × aios_kabi_dirent */
};

struct aios_kabi_create_in {
  uint64_t parent;
  uint32_t mode;
  uint32_t _pad;
  char name[AIOS_KABI_NAME_MAX + 1];
};

struct aios_kabi_unlink_in {
  uint64_t parent;
  char name[AIOS_KABI_NAME_MAX + 1];
};

struct aios_kabi_rename_in {
  uint64_t old_parent;
  uint64_t new_parent;
  char old_name[AIOS_KABI_NAME_MAX + 1];
  char new_name[AIOS_KABI_NAME_MAX + 1];
};

struct aios_kabi_link_in {
  uint64_t old_parent;
  uint64_t new_parent;
  char old_name[AIOS_KABI_NAME_MAX + 1];
  char new_name[AIOS_KABI_NAME_MAX + 1];
};

struct aios_kabi_rw_in {
  uint64_t ino;
  uint64_t offset;
  uint32_t size;
  uint32_t _pad;
  /* WRITE: followed by size bytes */
};

struct aios_kabi_rw_out {
  uint32_t size;
  uint32_t _pad;
  /* READ: followed by size bytes */
};

struct aios_kabi_truncate_in {
  uint64_t ino;
  uint64_t size;
};

struct aios_kabi_setattr_in {
  uint64_t ino;
  uint32_t to_set;
  uint32_t _pad;
  struct aios_kabi_stat st;
};

enum {
  AIOS_KABI_SET_MODE = 1u << 0,
  AIOS_KABI_SET_UID = 1u << 1,
  AIOS_KABI_SET_GID = 1u << 2,
  AIOS_KABI_SET_SIZE = 1u << 3,
  AIOS_KABI_SET_MTIME = 1u << 4,
  AIOS_KABI_SET_ATIME = 1u << 5,
};

#ifdef __cplusplus
}
#endif

#endif /* AIOS_KABI_H */
