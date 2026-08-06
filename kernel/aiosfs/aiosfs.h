/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AIOSFS_H
#define AIOSFS_H

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/uio.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/writeback.h>
#include <linux/xattr.h>

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

struct aios_sb_info {
	enum aios_backend backend;
	struct aios_conn *conn;
	struct aios_http_client *http;
	struct aios_http_pool *http_pool;
	/* Writeback fan-out. Must not be system_wq: these items do socket I/O and
	 * allocate, so on the reclaim path they need a rescuer to make progress. */
	struct workqueue_struct *wb_wq;
	struct mutex http_mu;
	int mount_id;
	char endpoint[256];
	char cluster_key[256];
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
int aios_io_fsync(struct inode *inode);

int aios_http_io_read(struct inode *inode, loff_t pos, void *buf, size_t len, size_t *out_len);
int aios_http_io_write(struct inode *inode, loff_t pos, const void *buf, size_t len);
int aios_http_io_set_size(struct inode *inode, loff_t size);
int aios_http_io_punch(struct inode *inode, loff_t offset, loff_t len);
/* Parallel dirty-page flush using aios_http_pool (chunk-grouped). */
int aios_http_writepages(struct address_space *mapping, struct writeback_control *wbc);

/* xattrs (HTTP backend) */
int aios_http_getxattr(struct inode *inode, const char *name, void *buf, size_t size);
int aios_http_setxattr(struct inode *inode, const char *name, const void *buf, size_t size,
		       int flags);
int aios_http_listxattr(struct inode *inode, char *list, size_t size);
int aios_http_removexattr(struct inode *inode, const char *name);

/* xattrs (dispatch + VFS handlers for 5.14) */
int aios_getxattr(struct inode *inode, const char *name, void *buf, size_t size);
int aios_setxattr(struct inode *inode, const char *name, const void *buf, size_t size, int flags);
int aios_listxattr(struct dentry *dentry, char *list, size_t size);
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
extern const struct file_operations aios_dir_ops;
extern const struct file_operations aios_file_ops;
extern const struct super_operations aios_super_ops;
extern const struct dentry_operations aios_dentry_ops;

extern const struct inode_operations aios_http_dir_inode_ops;
extern const struct inode_operations aios_http_file_inode_ops;
extern const struct file_operations aios_http_dir_ops;

void aios_stat_to_inode(struct inode *inode, const struct aios_kabi_stat *st);

#endif /* AIOSFS_H */
