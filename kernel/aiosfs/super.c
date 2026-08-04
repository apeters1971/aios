// SPDX-License-Identifier: GPL-2.0
#include "aiosfs.h"

#include <linux/fs_context.h>
#include <linux/parser.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/string.h>

enum {
	Opt_endpoint,
	Opt_cluster_key,
	Opt_volume,
	Opt_app_label,
	Opt_stripe_unit,
	Opt_stripe_width,
	Opt_uid,
	Opt_gid,
	Opt_backend,
	Opt_err,
};

static const match_table_t aios_tokens = {
	{ Opt_endpoint, "endpoint=%s" },
	{ Opt_cluster_key, "cluster_key=%s" },
	{ Opt_volume, "volume=%s" },
	{ Opt_app_label, "app_label=%s" },
	{ Opt_stripe_unit, "stripe_unit=%s" },
	{ Opt_stripe_width, "stripe_width=%u" },
	{ Opt_uid, "uid=%u" },
	{ Opt_gid, "gid=%u" },
	{ Opt_backend, "backend=%s" },
	{ Opt_err, NULL },
};

static int aios_parse_options(char *options, struct aios_sb_info *info)
{
	substring_t args[MAX_OPT_ARGS];
	char *p;

	if (!options)
		return 0;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;

		if (!*p)
			continue;
		token = match_token(p, aios_tokens, args);
		switch (token) {
		case Opt_endpoint:
			match_strlcpy(info->endpoint, &args[0], sizeof(info->endpoint));
			break;
		case Opt_cluster_key:
			match_strlcpy(info->cluster_key, &args[0], sizeof(info->cluster_key));
			break;
		case Opt_volume:
			match_strlcpy(info->volume, &args[0], sizeof(info->volume));
			break;
		case Opt_app_label:
			match_strlcpy(info->app_label, &args[0], sizeof(info->app_label));
			break;
		case Opt_stripe_unit: {
			char buf[32];
			match_strlcpy(buf, &args[0], sizeof(buf));
			if (kstrtou64(buf, 0, &info->stripe_unit))
				return -EINVAL;
			break;
		}
		case Opt_stripe_width: {
			unsigned int v;

			if (match_uint(&args[0], &v))
				return -EINVAL;
			info->stripe_width = v;
			break;
		}
		case Opt_uid: {
			unsigned int v;

			if (match_uint(&args[0], &v))
				return -EINVAL;
			info->uid = v;
			break;
		}
		case Opt_gid: {
			unsigned int v;

			if (match_uint(&args[0], &v))
				return -EINVAL;
			info->gid = v;
			break;
		}
		case Opt_backend: {
			char buf[16];

			match_strlcpy(buf, &args[0], sizeof(buf));
			if (!strcmp(buf, "upcall"))
				info->backend = AIOS_BACKEND_UPCALL;
			else if (!strcmp(buf, "http"))
				info->backend = AIOS_BACKEND_HTTP;
			else {
				pr_err("aiosfs: backend= must be upcall or http\n");
				return -EINVAL;
			}
			break;
		}
		default:
			pr_err("aiosfs: unknown option '%s'\n", p);
			return -EINVAL;
		}
	}
	return 0;
}

static void aios_put_super(struct super_block *sb)
{
	struct aios_sb_info *info = AIOS_SB(sb);

	if (!info)
		return;
	if (info->backend == AIOS_BACKEND_UPCALL && info->conn && info->mount_id >= 0) {
		aios_upcall(info->conn, AIOS_OP_UMOUNT, info->mount_id, NULL, 0, NULL, NULL);
		aios_conn_put(info->conn);
		info->conn = NULL;
	}
	kfree(info);
	sb->s_fs_info = NULL;
}

static int aios_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct aios_sb_info *info = AIOS_SB(dentry->d_sb);
	struct aios_kabi_statvfs *st;
	void *out = NULL;
	u32 out_len = 0;
	int err;

	err = aios_upcall(info->conn, AIOS_OP_STATFS, info->mount_id, NULL, 0, &out, &out_len);
	if (err)
		return err;
	if (out_len < sizeof(*st)) {
		kfree(out);
		return -EIO;
	}
	st = out;
	buf->f_type = AIOSFS_MAGIC;
	buf->f_bsize = st->bsize ? st->bsize : 4096;
	buf->f_blocks = st->blocks;
	buf->f_bfree = st->bfree;
	buf->f_bavail = st->bavail;
	buf->f_files = st->files;
	buf->f_ffree = st->ffree;
	buf->f_namelen = st->namemax ? st->namemax : AIOS_KABI_NAME_MAX;
	kfree(out);
	return 0;
}

int aios_show_options(struct seq_file *m, struct dentry *root)
{
	struct aios_sb_info *info = AIOS_SB(root->d_sb);

	if (info->backend == AIOS_BACKEND_HTTP)
		seq_puts(m, ",backend=http");
	else
		seq_puts(m, ",backend=upcall");
	if (info->endpoint[0])
		seq_printf(m, ",endpoint=%s", info->endpoint);
	if (info->volume[0])
		seq_printf(m, ",volume=%s", info->volume);
	if (info->app_label[0])
		seq_printf(m, ",app_label=%s", info->app_label);
	if (info->stripe_unit)
		seq_printf(m, ",stripe_unit=%llu",
			   (unsigned long long)info->stripe_unit);
	if (info->stripe_width)
		seq_printf(m, ",stripe_width=%u", info->stripe_width);
	return 0;
}

const struct super_operations aios_super_ops = {
	.statfs = aios_statfs,
	.drop_inode = generic_delete_inode,
	.evict_inode = aios_evict_inode,
	.write_inode = aios_write_inode,
	.put_super = aios_put_super,
	.show_options = aios_show_options,
};

int aios_fill_super(struct super_block *sb, struct aios_sb_info *info)
{
	struct aios_kabi_mount_in min = { 0 };
	struct aios_kabi_mount_out *mout;
	struct inode *root;
	void *out = NULL;
	u32 out_len = 0;
	int err;

	sb->s_magic = AIOSFS_MAGIC;
	sb->s_op = &aios_super_ops;
	sb->s_d_op = &aios_dentry_ops;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_blocksize = 4096;
	sb->s_blocksize_bits = 12;
	sb->s_time_gran = 1;
	sb->s_fs_info = info;

	info->conn = aios_conn_get();
	if (!info->conn)
		return -ENOMEM;
	if (!aios_conn_daemon_present(info->conn)) {
		pr_err("aiosfs: start aios-kbridge before mounting (/dev/%s)\n",
		       AIOS_KABI_DEV_NAME);
		aios_conn_put(info->conn);
		info->conn = NULL;
		return -ENOTCONN;
	}

	strscpy(min.endpoint, info->endpoint, sizeof(min.endpoint));
	strscpy(min.cluster_key, info->cluster_key, sizeof(min.cluster_key));
	strscpy(min.volume, info->volume[0] ? info->volume : "default", sizeof(min.volume));
	strscpy(min.app_label, info->app_label, sizeof(min.app_label));
	min.stripe_unit = info->stripe_unit;
	min.stripe_width = info->stripe_width;
	min.uid = info->uid;
	min.gid = info->gid;

	err = aios_upcall(info->conn, AIOS_OP_MOUNT, -1, &min, sizeof(min), &out, &out_len);
	if (err) {
		aios_conn_put(info->conn);
		info->conn = NULL;
		return err;
	}
	if (out_len < sizeof(*mout)) {
		kfree(out);
		aios_conn_put(info->conn);
		info->conn = NULL;
		return -EIO;
	}
	mout = out;
	info->mount_id = mout->mount_id;

	root = aios_iget(sb, &mout->root);
	kfree(out);
	if (IS_ERR(root)) {
		aios_upcall(info->conn, AIOS_OP_UMOUNT, info->mount_id, NULL, 0, NULL, NULL);
		aios_conn_put(info->conn);
		info->conn = NULL;
		return PTR_ERR(root);
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		aios_upcall(info->conn, AIOS_OP_UMOUNT, info->mount_id, NULL, 0, NULL, NULL);
		aios_conn_put(info->conn);
		info->conn = NULL;
		return -ENOMEM;
	}
	return 0;
}

static int aios_get_tree_fill(struct super_block *sb, struct fs_context *fc)
{
	struct aios_sb_info *template = fc->s_fs_info;
	struct aios_sb_info *info;
	int err;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	*info = *template;
	info->mount_id = -1;
	info->conn = NULL;

	if (info->backend == AIOS_BACKEND_HTTP)
		err = aios_fill_super_http(sb, info);
	else
		err = aios_fill_super(sb, info);
	if (err) {
		kfree(info);
		sb->s_fs_info = NULL;
	}
	return err;
}

static int aios_get_tree(struct fs_context *fc)
{
	struct aios_sb_info *info = fc->s_fs_info;

	if (!info->endpoint[0] || !info->cluster_key[0]) {
		pr_err("aiosfs: endpoint= and cluster_key= are required\n");
		return -EINVAL;
	}
	return get_tree_nodev(fc, aios_get_tree_fill);
}

static void aios_free_fc(struct fs_context *fc)
{
	kfree(fc->s_fs_info);
	fc->s_fs_info = NULL;
}

static int aios_parse_monolithic(struct fs_context *fc, void *data)
{
	return aios_parse_options(data, fc->s_fs_info);
}

static const struct fs_context_operations aios_context_ops = {
	.free = aios_free_fc,
	.parse_monolithic = aios_parse_monolithic,
	.get_tree = aios_get_tree,
};

int aios_init_fs_context(struct fs_context *fc)
{
	struct aios_sb_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->mount_id = -1;
	info->backend = AIOS_BACKEND_UPCALL;
	strscpy(info->volume, "default", sizeof(info->volume));
	fc->s_fs_info = info;
	fc->ops = &aios_context_ops;
	return 0;
}
