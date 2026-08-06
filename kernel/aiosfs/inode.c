// SPDX-License-Identifier: GPL-2.0
#include "aiosfs.h"

#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uio.h>

void aios_stat_to_inode(struct inode *inode, const struct aios_kabi_stat *st)
{
	inode->i_ino = st->ino;
	inode->i_mode = st->mode;
	set_nlink(inode, st->nlink ? st->nlink : 1);
	i_uid_write(inode, st->uid);
	i_gid_write(inode, st->gid);
	if (S_ISREG(st->mode)) {
		struct address_space *mapping = inode->i_mapping;

		if (mapping && (mapping_tagged(mapping, PAGECACHE_TAG_DIRTY) ||
				mapping_tagged(mapping, PAGECACHE_TAG_WRITEBACK))) {
			/* Keep in-core size while dirty/writeback pages are outstanding. */
		} else if (st->size != (u64)i_size_read(inode)) {
			i_size_write(inode, st->size);
		}
	} else {
		inode->i_size = st->size;
	}
	inode->i_atime.tv_sec = st->atime_ns / 1000000000ull;
	inode->i_atime.tv_nsec = st->atime_ns % 1000000000ull;
	inode->i_mtime.tv_sec = st->mtime_ns / 1000000000ull;
	inode->i_mtime.tv_nsec = st->mtime_ns % 1000000000ull;
	inode->i_ctime.tv_sec = st->ctime_ns / 1000000000ull;
	inode->i_ctime.tv_nsec = st->ctime_ns % 1000000000ull;
}

struct inode *aios_iget(struct super_block *sb, const struct aios_kabi_stat *st)
{
	struct inode *inode;

	inode = iget_locked(sb, st->ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	aios_stat_to_inode(inode, st);
	if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &aios_dir_inode_ops;
		inode->i_fop = &aios_dir_ops;
	} else {
		inode->i_op = &aios_file_inode_ops;
		aios_setup_file_inode(inode);
	}
	unlock_new_inode(inode);
	return inode;
}

static int aios_refresh_inode(struct inode *inode)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_kabi_ino_in in = { .ino = inode->i_ino };
	struct aios_kabi_stat *st;
	void *out = NULL;
	u32 out_len = 0;
	int err;

	err = aios_upcall(info->conn, AIOS_OP_GETATTR, info->mount_id, &in, sizeof(in),
			  &out, &out_len);
	if (err)
		return err;
	if (out_len < sizeof(*st)) {
		kfree(out);
		return -EIO;
	}
	st = out;
	aios_stat_to_inode(inode, st);
	kfree(out);
	return 0;
}

static struct dentry *aios_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_kabi_lookup_in in = { .parent = dir->i_ino };
	struct aios_kabi_stat *st;
	struct inode *inode;
	void *out = NULL;
	u32 out_len = 0;
	int err;

	if (dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	memcpy(in.name, dentry->d_name.name, dentry->d_name.len);
	in.name[dentry->d_name.len] = '\0';

	err = aios_upcall(info->conn, AIOS_OP_LOOKUP, info->mount_id, &in, sizeof(in),
			  &out, &out_len);
	if (err == -ENOENT) {
		d_add(dentry, NULL);
		return NULL;
	}
	if (err)
		return ERR_PTR(err);
	if (out_len < sizeof(*st)) {
		kfree(out);
		return ERR_PTR(-EIO);
	}
	st = out;
	inode = aios_iget(dir->i_sb, st);
	kfree(out);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	return d_splice_alias(inode, dentry);
}

static int aios_create(struct user_namespace *mnt_userns, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_kabi_create_in in = { .parent = dir->i_ino, .mode = mode };
	struct aios_kabi_stat *st;
	struct inode *inode;
	void *out = NULL;
	u32 out_len = 0;
	int err;

	if (dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(in.name, dentry->d_name.name, dentry->d_name.len);
	in.name[dentry->d_name.len] = '\0';

	err = aios_upcall(info->conn, AIOS_OP_CREATE, info->mount_id, &in, sizeof(in),
			  &out, &out_len);
	if (err)
		return err;
	if (out_len < sizeof(*st)) {
		kfree(out);
		return -EIO;
	}
	st = out;
	inode = aios_iget(dir->i_sb, st);
	kfree(out);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
	return 0;
}

static int aios_mkdir(struct user_namespace *mnt_userns, struct inode *dir,
		      struct dentry *dentry, umode_t mode)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_kabi_create_in in = { .parent = dir->i_ino, .mode = mode };
	struct aios_kabi_stat *st;
	struct inode *inode;
	void *out = NULL;
	u32 out_len = 0;
	int err;

	if (dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(in.name, dentry->d_name.name, dentry->d_name.len);
	in.name[dentry->d_name.len] = '\0';

	err = aios_upcall(info->conn, AIOS_OP_MKDIR, info->mount_id, &in, sizeof(in),
			  &out, &out_len);
	if (err)
		return err;
	if (out_len < sizeof(*st)) {
		kfree(out);
		return -EIO;
	}
	st = out;
	inode = aios_iget(dir->i_sb, st);
	kfree(out);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
	inc_nlink(dir);
	return 0;
}

static int aios_unlink(struct inode *dir, struct dentry *dentry)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_kabi_unlink_in in = { .parent = dir->i_ino };
	int err;

	if (dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(in.name, dentry->d_name.name, dentry->d_name.len);
	in.name[dentry->d_name.len] = '\0';

	err = aios_upcall(info->conn, AIOS_OP_UNLINK, info->mount_id, &in, sizeof(in),
			  NULL, NULL);
	if (!err) {
		drop_nlink(d_inode(dentry));
		d_drop(dentry);
	}
	return err;
}

static int aios_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_kabi_unlink_in in = { .parent = dir->i_ino };
	int err;

	if (dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(in.name, dentry->d_name.name, dentry->d_name.len);
	in.name[dentry->d_name.len] = '\0';

	err = aios_upcall(info->conn, AIOS_OP_RMDIR, info->mount_id, &in, sizeof(in),
			  NULL, NULL);
	if (!err) {
		clear_nlink(d_inode(dentry));
		drop_nlink(dir);
		d_drop(dentry);
	}
	return err;
}

static int aios_rename(struct user_namespace *mnt_userns, struct inode *old_dir,
		       struct dentry *old_dentry, struct inode *new_dir,
		       struct dentry *new_dentry, unsigned int flags)
{
	struct aios_sb_info *info = AIOS_SB(old_dir->i_sb);
	struct aios_kabi_rename_in in = {
		.old_parent = old_dir->i_ino,
		.new_parent = new_dir->i_ino,
	};
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	bool is_dir = old_inode && S_ISDIR(old_inode->i_mode);
	int err;

	if (flags)
		return -EINVAL;
	if (old_dentry->d_name.len > AIOS_KABI_NAME_MAX ||
	    new_dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(in.old_name, old_dentry->d_name.name, old_dentry->d_name.len);
	in.old_name[old_dentry->d_name.len] = '\0';
	memcpy(in.new_name, new_dentry->d_name.name, new_dentry->d_name.len);
	in.new_name[new_dentry->d_name.len] = '\0';

	err = aios_upcall(info->conn, AIOS_OP_RENAME, info->mount_id, &in, sizeof(in),
			   NULL, NULL);
	if (err)
		return err;

	if (new_inode) {
		if (S_ISDIR(new_inode->i_mode))
			clear_nlink(new_inode);
		else
			drop_nlink(new_inode);
	}
	if (is_dir) {
		drop_nlink(old_dir);
		inc_nlink(new_dir);
	}
	return 0;
}

static int aios_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_kabi_link_in in = {
		.old_parent = d_inode(old_dentry->d_parent)->i_ino,
		.new_parent = dir->i_ino,
	};
	int err;

	if (old_dentry->d_name.len > AIOS_KABI_NAME_MAX ||
	    dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(in.old_name, old_dentry->d_name.name, old_dentry->d_name.len);
	in.old_name[old_dentry->d_name.len] = '\0';
	memcpy(in.new_name, dentry->d_name.name, dentry->d_name.len);
	in.new_name[dentry->d_name.len] = '\0';

	err = aios_upcall(info->conn, AIOS_OP_LINK, info->mount_id, &in, sizeof(in),
			  NULL, NULL);
	if (!err) {
		ihold(d_inode(old_dentry));
		d_instantiate(dentry, d_inode(old_dentry));
	}
	return err;
}

static int aios_getattr(struct user_namespace *mnt_userns, const struct path *path,
			struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	int err = aios_refresh_inode(inode);

	if (err)
		return err;
	generic_fillattr(inode, stat);
	return 0;
}

static int aios_setattr(struct user_namespace *mnt_userns, struct dentry *dentry,
			struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_kabi_setattr_in in = { .ino = inode->i_ino };
	int err;

	err = setattr_prepare(mnt_userns, dentry, attr);
	if (err)
		return err;

	if (attr->ia_valid & ATTR_MODE) {
		in.to_set |= AIOS_KABI_SET_MODE;
		in.st.mode = attr->ia_mode;
	}
	if (attr->ia_valid & ATTR_UID) {
		in.to_set |= AIOS_KABI_SET_UID;
		in.st.uid = from_kuid(mnt_userns, attr->ia_uid);
	}
	if (attr->ia_valid & ATTR_GID) {
		in.to_set |= AIOS_KABI_SET_GID;
		in.st.gid = from_kgid(mnt_userns, attr->ia_gid);
	}
	if (attr->ia_valid & ATTR_SIZE) {
		in.to_set |= AIOS_KABI_SET_SIZE;
		in.st.size = attr->ia_size;
	}
	if (attr->ia_valid & ATTR_MTIME) {
		in.to_set |= AIOS_KABI_SET_MTIME;
		in.st.mtime_ns = (u64)attr->ia_mtime.tv_sec * 1000000000ull +
				 attr->ia_mtime.tv_nsec;
	}
	if (attr->ia_valid & ATTR_ATIME) {
		in.to_set |= AIOS_KABI_SET_ATIME;
		in.st.atime_ns = (u64)attr->ia_atime.tv_sec * 1000000000ull +
				 attr->ia_atime.tv_nsec;
	}

	err = aios_upcall(info->conn, AIOS_OP_SETATTR, info->mount_id, &in, sizeof(in),
			  NULL, NULL);
	if (err)
		return err;
	if (attr->ia_valid & ATTR_SIZE)
		truncate_setsize(inode, attr->ia_size);
	setattr_copy(mnt_userns, inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

const struct inode_operations aios_dir_inode_ops = {
	.lookup = aios_lookup,
	.create = aios_create,
	.mkdir = aios_mkdir,
	.unlink = aios_unlink,
	.rmdir = aios_rmdir,
	.rename = aios_rename,
	.link = aios_link,
	.getattr = aios_getattr,
	.setattr = aios_setattr,
	.listxattr = aios_listxattr,
};

const struct inode_operations aios_file_inode_ops = {
	.getattr = aios_getattr,
	.setattr = aios_setattr,
	.listxattr = aios_listxattr,
};

static int aios_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_kabi_readdir_in in = {
		.ino = inode->i_ino,
		.offset = (u64)ctx->pos,
		.max_entries = 64,
	};
	struct aios_kabi_readdir_out *hdr;
	struct aios_kabi_dirent *ents;
	void *out = NULL;
	u32 out_len = 0;
	u32 i;
	int err;

	/* Userspace readdir already includes "." / "..". */
	err = aios_upcall(info->conn, AIOS_OP_READDIR, info->mount_id, &in, sizeof(in),
			  &out, &out_len);
	if (err)
		return err;
	if (out_len < sizeof(*hdr)) {
		kfree(out);
		return -EIO;
	}
	hdr = out;
	if (out_len < sizeof(*hdr) + hdr->count * sizeof(*ents)) {
		kfree(out);
		return -EIO;
	}
	ents = (struct aios_kabi_dirent *)(hdr + 1);
	{
		loff_t start = in.offset;

		for (i = 0; i < hdr->count; ++i) {
			unsigned char type = DT_UNKNOWN;

			if (S_ISDIR(ents[i].mode))
				type = DT_DIR;
			else if (S_ISREG(ents[i].mode))
				type = DT_REG;
			if (!dir_emit(ctx, ents[i].name, strnlen(ents[i].name, AIOS_KABI_NAME_MAX),
				      ents[i].ino, type))
				break;
			ctx->pos = start + (loff_t)i + 1;
		}
		if (i == hdr->count) {
			if (hdr->count > 0 && hdr->next_offset <= (u64)start) {
				kfree(out);
				return -EIO;
			}
			ctx->pos = hdr->next_offset;
		}
	}
	kfree(out);
	return 0;
}

const struct file_operations aios_dir_ops = {
	.owner = THIS_MODULE,
	.iterate_shared = aios_readdir,
	.llseek = generic_file_llseek,
};

int aios_file_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file_inode(file);
	int err;

	err = file_write_and_wait_range(file, start, end);
	if (err)
		return err;
	inode_lock(inode);
	err = sync_inode_metadata(inode, 1);
	if (!err)
		err = aios_io_fsync(inode);
	inode_unlock(inode);
	return err;
}

const struct file_operations aios_file_ops = {
	.owner = THIS_MODULE,
	.llseek = generic_file_llseek,
	.read_iter = aios_file_read_iter,
	.write_iter = aios_file_write_iter,
	.mmap = generic_file_mmap,
	.fsync = aios_file_fsync,
	.fallocate = aios_fallocate,
	.lock = locks_lock_file_wait,
	.flock = locks_lock_file_wait,
	.splice_read = generic_file_splice_read,
	.open = generic_file_open,
};

static int aios_d_revalidate(struct dentry *dentry, unsigned int flags)
{
	return 0; /* always revalidate via lookup upcall */
}

const struct dentry_operations aios_dentry_ops = {
	.d_revalidate = aios_d_revalidate,
};
