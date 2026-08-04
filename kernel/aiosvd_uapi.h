/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Userspace ↔ kernel ABI for aiosvd (AIOS Volume Device).
 * AlmaLinux 9 / RHEL 9 (kernel 5.14).
 */
#ifndef AIOSVD_UAPI_H
#define AIOSVD_UAPI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

#define AIOSVD_CTL_NAME "aiosvd_ctl"
#define AIOSVD_DISK_PREFIX "aiosvd"
#define AIOSVD_IOCTL_MAGIC 0xA1

#define AIOSVD_NAME_MAX 63
#define AIOSVD_POOL_MAX 63
#define AIOSVD_KEY_ID_MAX 63
#define AIOSVD_MAX_DEVS 32
#define AIOSVD_DEFAULT_OBJ_ORDER 22 /* 4 MiB objects */

/* Map flags */
#define AIOSVD_MAP_CREATE (1u << 0) /* create header if missing */
#define AIOSVD_MAP_READONLY (1u << 1) /* reject writes / discard */
#define AIOSVD_MAP_EXCL (1u << 2) /* with CREATE: fail if header exists */

struct aiosvd_map_arg {
	char endpoint[256];
	char cluster_key[256];
	char pool[AIOSVD_POOL_MAX + 1];
	char name[AIOSVD_NAME_MAX + 1];
	char app_label[64];
	char key_id[AIOSVD_KEY_ID_MAX + 1]; /* encryption key hook (stored/logged only) */
	uint64_t size; /* bytes; required with AIOSVD_MAP_CREATE, else read from header */
	uint32_t obj_order; /* 0 → default; object size = 1 << obj_order */
	uint32_t flags;
	uint32_t queue_depth; /* 0 → default; caps blk-mq tag depth */
	uint32_t max_clients; /* 0 → default; caps HTTP client pool */
	int32_t dev_id; /* OUT: mapped device index → /dev/aiosvdN */
	int32_t _pad;
};

struct aiosvd_unmap_arg {
	int32_t dev_id;
	int32_t _pad;
};

struct aiosvd_info_arg {
	int32_t dev_id;
	int32_t _pad;
	char pool[AIOSVD_POOL_MAX + 1];
	char name[AIOSVD_NAME_MAX + 1];
	char parent_pool[AIOSVD_POOL_MAX + 1];
	char parent_name[AIOSVD_NAME_MAX + 1];
	char key_id[AIOSVD_KEY_ID_MAX + 1];
	uint64_t size;
	uint32_t obj_order;
	uint32_t flags;
	uint64_t bytes_read;
	uint64_t bytes_written;
	uint64_t ops_read;
	uint64_t ops_write;
	uint64_t ops_discard;
	uint64_t errors;
	uint64_t timeouts;
	uint64_t reconnects;
	uint64_t cache_hits;
};

struct aiosvd_list_arg {
	uint32_t count; /* OUT */
	uint32_t _pad;
	struct aiosvd_info_arg entries[AIOSVD_MAX_DEVS];
};

struct aiosvd_resize_arg {
	int32_t dev_id;
	int32_t _pad;
	uint64_t new_size; /* bytes; must be multiple of 4096 */
};

/* Lightweight clone: child header references parent; COW on first write. */
struct aiosvd_clone_arg {
	int32_t src_dev_id; /* mapped parent device */
	int32_t _pad;
	char pool[AIOSVD_POOL_MAX + 1]; /* child pool */
	char name[AIOSVD_NAME_MAX + 1]; /* child name */
	int32_t dest_dev_id; /* OUT: mapped child device */
	int32_t _pad2;
};

struct aiosvd_rename_arg {
	int32_t dev_id;
	int32_t _pad;
	char pool[AIOSVD_POOL_MAX + 1];
	char name[AIOSVD_NAME_MAX + 1];
};

#define AIOSVD_IOCTL_MAP _IOWR(AIOSVD_IOCTL_MAGIC, 1, struct aiosvd_map_arg)
#define AIOSVD_IOCTL_UNMAP _IOW(AIOSVD_IOCTL_MAGIC, 2, struct aiosvd_unmap_arg)
#define AIOSVD_IOCTL_INFO _IOWR(AIOSVD_IOCTL_MAGIC, 3, struct aiosvd_info_arg)
#define AIOSVD_IOCTL_LIST _IOR(AIOSVD_IOCTL_MAGIC, 4, struct aiosvd_list_arg)
#define AIOSVD_IOCTL_RESIZE _IOWR(AIOSVD_IOCTL_MAGIC, 5, struct aiosvd_resize_arg)
#define AIOSVD_IOCTL_CLONE _IOWR(AIOSVD_IOCTL_MAGIC, 6, struct aiosvd_clone_arg)
#define AIOSVD_IOCTL_RENAME _IOWR(AIOSVD_IOCTL_MAGIC, 7, struct aiosvd_rename_arg)

#endif /* AIOSVD_UAPI_H */
