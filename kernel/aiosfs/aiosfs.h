/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AIOSFS_H
#define AIOSFS_H

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/uio.h>
#include <linux/uidgid.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/writeback.h>
#include <linux/xattr.h>
#if __has_include(<linux/filelock.h>)
#include <linux/filelock.h>
#endif
#if __has_include(<linux/mnt_idmap.h>)
#include <linux/mnt_idmap.h>
#endif
/*
 * Alma 9.8 / current RHEL 9 kernels expose struct mnt_idmap from fs.h
 * without shipping linux/mnt_idmap.h, so __has_include is the wrong
 * probe. Default to the 6.3-style idmap VFS. Define AIOS_VFS_USERNS
 * when building against a 5.14-era tree that still uses user_namespace.
 */
#ifndef AIOS_VFS_USERNS
#define AIOS_HAS_MNT_IDMAP 1
#define AIOS_IDMAP struct mnt_idmap
#else
#define AIOS_IDMAP struct user_namespace
#endif
/*
 * Alma 9.8 backported folio aops onto 5.14: write_cache_pages takes a
 * folio callback, readpage/set_page_dirty are gone, write_begin dropped
 * flags. Define AIOS_AOPS_PAGE for a tree that still uses page aops.
 */
#ifndef AIOS_AOPS_PAGE
#define AIOS_HAS_FOLIO_AOPS 1
#endif

static inline void aios_fillattr(AIOS_IDMAP *idmap, struct inode *inode, struct kstat *stat)
{
#ifdef AIOS_HAS_MNT_IDMAP
	generic_fillattr(idmap, inode, stat);
#else
	(void)idmap;
	generic_fillattr(inode, stat);
#endif
}

static inline uid_t aios_iattr_uid(AIOS_IDMAP *idmap, const struct iattr *attr)
{
#ifdef AIOS_HAS_MNT_IDMAP
	(void)idmap;
	return from_kuid(&init_user_ns, attr->ia_uid);
#else
	return from_kuid(idmap, attr->ia_uid);
#endif
}

static inline gid_t aios_iattr_gid(AIOS_IDMAP *idmap, const struct iattr *attr)
{
#ifdef AIOS_HAS_MNT_IDMAP
	(void)idmap;
	return from_kgid(&init_user_ns, attr->ia_gid);
#else
	return from_kgid(idmap, attr->ia_gid);
#endif
}

#include "../aios_kabi.h"

struct aios_http_client;
struct aios_http_pool;

#define AIOSFS_NAME "aios"
#define AIOSFS_MAGIC 0x41494F53 /* AIOS */
#define AIOSFS_HTTP_POOL_SIZE 4

enum aios_backend {
	AIOS_BACKEND_UPCALL = 0,
	AIOS_BACKEND_HTTP = 1,
};

struct aios_conn;

/*
 * Chunk read-modify-write on the HTTP backend is GET → patch → PUT of a whole
 * stripe unit. Every path that does it (buffered writeback workers, writepage,
 * O_DIRECT, punch, truncate) must hold the stripe's mutex across the three
 * steps or concurrent 4 KiB updates to the same chunk lose each other.
 */
#define AIOS_CHUNK_LOCK_STRIPES 16

struct aios_inode_aux {
	u64 last_synced_size;
	u64 cas;
	u64 stripe_unit;
	u32 stripe_width;
	u64 dirty_bytes;
	unsigned long dirty_since; /* jiffies; 0 if size/mtime is clean */
	/* extras_lock guards xattrs_obj / symlink / extras_valid. */
	spinlock_t extras_lock;
	char *xattrs_obj; /* heap `"xattrs"` object `{...}`, or NULL */
	char *symlink;
	bool extras_valid;
	struct mutex chunk_mu[AIOS_CHUNK_LOCK_STRIPES];
};

/* Get-or-create inode->i_private. Safe against concurrent callers. */
struct aios_inode_aux *aios_inode_aux_get(struct inode *inode, bool *created);

static inline struct mutex *aios_chunk_lock(struct aios_inode_aux *aux, u64 chunk)
{
	return &aux->chunk_mu[chunk % AIOS_CHUNK_LOCK_STRIPES];
}

struct aios_dir_cache;

#define AIOS_DENTRY_TTL_MS 250

struct aios_sb_info {
	enum aios_backend backend;
	struct aios_conn *conn;
	struct aios_http_client *http;
	struct aios_http_pool *http_pool;
	/* Writeback fan-out. Must not be system_wq: these items do socket I/O and
	 * allocate, so on the reclaim path they need a rescuer to make progress. */
	struct workqueue_struct *wb_wq;
	struct mutex http_mu;
	struct aios_dir_cache *dir_cache;
	int mount_id;
	char endpoint[256];
	/* Shared cluster key, or the principal key when principal[0] is set. */
	char cluster_key[256];
	char principal[65];
	char volume[64];
	char app_label[64];
	u64 stripe_unit;
	u32 stripe_width;
	u32 uid;
	u32 gid;
};

static inline struct aios_sb_info *AIOS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

int aios_upcall_init(void);
void aios_upcall_exit(void);

/* Synchronous upcall. payload_in may be NULL if in_len==0.
 * On success, *out owns kmalloc'd reply payload (caller kfree), *out_len set.
 * Returns 0 or -errno. */
int aios_upcall(struct aios_conn *conn, u32 opcode, int mount_id, const void *payload_in,
		u32 in_len, void **payload_out, u32 *out_len);

struct aios_conn *aios_conn_get(void);
void aios_conn_put(struct aios_conn *conn);
bool aios_conn_daemon_present(struct aios_conn *conn);

int aios_fill_super(struct super_block *sb, struct aios_sb_info *info);
int aios_fill_super_http(struct super_block *sb, struct aios_sb_info *info);
int aios_init_fs_context(struct fs_context *fc);
int aios_show_options(struct seq_file *m, struct dentry *root);
struct inode *aios_iget(struct super_block *sb, const struct aios_kabi_stat *st);

/* Backend I/O for page cache */
int aios_io_read(struct inode *inode, loff_t pos, void *buf, size_t len, size_t *out_len);
int aios_io_write(struct inode *inode, loff_t pos, const void *buf, size_t len);
int aios_io_set_size(struct inode *inode, loff_t size);
int aios_io_grow_size(struct inode *inode, loff_t size);
int aios_io_fsync(struct inode *inode);

int aios_http_io_read(struct inode *inode, loff_t pos, void *buf, size_t len, size_t *out_len);
int aios_http_io_write(struct inode *inode, loff_t pos, const void *buf, size_t len);
int aios_http_io_set_size(struct inode *inode, loff_t size);
int aios_http_io_punch(struct inode *inode, loff_t offset, loff_t len);
/* Parallel dirty-page flush using aios_http_pool (chunk-grouped). */
int aios_http_writepages(struct address_space *mapping, struct writeback_control *wbc);
/* Called from evict_inode when i_nlink == 0: remove chunks + inode object. */
void aios_http_evict_unlinked(struct inode *inode);

/* xattrs (HTTP backend) */
int aios_http_getxattr(struct inode *inode, const char *name, void *buf, size_t size);
int aios_http_setxattr(struct inode *inode, const char *name, const void *buf, size_t size,
		       int flags);
int aios_http_listxattr(struct inode *inode, char *list, size_t size);
int aios_http_removexattr(struct inode *inode, const char *name);

/* xattrs (dispatch + VFS handlers for 5.14) */
int aios_getxattr(struct inode *inode, const char *name, void *buf, size_t size);
int aios_setxattr(struct inode *inode, const char *name, const void *buf, size_t size, int flags);
ssize_t aios_listxattr(struct dentry *dentry, char *list, size_t size);
int aios_removexattr(struct inode *inode, const char *name);
extern const struct xattr_handler *aios_xattr_handlers[];

/* Page cache / file ops helpers */
extern const struct address_space_operations aios_aops;
void aios_setup_file_inode(struct inode *inode);
int aios_write_inode(struct inode *inode, struct writeback_control *wbc);
void aios_evict_inode(struct inode *inode);
int aios_file_fsync(struct file *file, loff_t start, loff_t end, int datasync);
ssize_t aios_file_read_iter(struct kiocb *iocb, struct iov_iter *to);
ssize_t aios_file_write_iter(struct kiocb *iocb, struct iov_iter *from);
long aios_fallocate(struct file *file, int mode, loff_t offset, loff_t len);

extern const struct inode_operations aios_dir_inode_ops;
extern const struct inode_operations aios_file_inode_ops;
extern const struct inode_operations aios_symlink_inode_ops;
extern const struct file_operations aios_dir_ops;
extern const struct file_operations aios_file_ops;
extern const struct super_operations aios_super_ops;
extern const struct dentry_operations aios_dentry_ops;

extern const struct inode_operations aios_http_dir_inode_ops;
extern const struct inode_operations aios_http_file_inode_ops;
extern const struct inode_operations aios_http_symlink_inode_ops;
extern const struct file_operations aios_http_dir_ops;

void aios_stat_to_inode(struct inode *inode, const struct aios_kabi_stat *st);
void aios_set_inode_blocks(struct inode *inode);

static inline void aios_d_mark_fresh(struct dentry *dentry)
{
	if (dentry)
		dentry->d_time = jiffies;
}

static inline bool aios_d_is_fresh(const struct dentry *dentry)
{
	return dentry && time_before(jiffies, dentry->d_time +
						     msecs_to_jiffies(AIOS_DENTRY_TTL_MS));
}

#endif /* AIOSFS_H */
