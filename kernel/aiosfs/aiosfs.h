/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AIOSFS_H
#define AIOSFS_H

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/wait.h>

#include "../aios_kabi.h"

#define AIOSFS_NAME "aios"
#define AIOSFS_MAGIC 0x41494F53 /* AIOS */

struct aios_conn;

struct aios_sb_info {
	struct aios_conn *conn;
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
int aios_upcall(struct aios_conn *conn, u32 opcode, int mount_id,
		const void *payload_in, u32 in_len,
		void **payload_out, u32 *out_len);

struct aios_conn *aios_conn_get(void);
void aios_conn_put(struct aios_conn *conn);
bool aios_conn_daemon_present(struct aios_conn *conn);

int aios_fill_super(struct super_block *sb, struct aios_sb_info *info);
int aios_init_fs_context(struct fs_context *fc);
struct inode *aios_iget(struct super_block *sb, const struct aios_kabi_stat *st);

extern const struct inode_operations aios_dir_inode_ops;
extern const struct inode_operations aios_file_inode_ops;
extern const struct file_operations aios_dir_ops;
extern const struct file_operations aios_file_ops;
extern const struct super_operations aios_super_ops;
extern const struct dentry_operations aios_dentry_ops;

void aios_stat_to_inode(struct inode *inode, const struct aios_kabi_stat *st);

#endif /* AIOSFS_H */
