// SPDX-License-Identifier: GPL-2.0
/*
 * Extended attributes for aiosfs (AlmaLinux 9 / 5.14).
 * VFS uses s_xattr handlers; listxattr remains on inode_operations.
 * Upcall backend → aios-kbridge → aios_posix_*xattr; HTTP → inode meta JSON.
 */
#include "aiosfs.h"

#include <linux/slab.h>
#include <linux/string.h>

#define AIOS_XATTR_VALUE_MAX AIOS_KABI_XATTR_VALUE_MAX

static int upcall_getxattr(struct inode *inode, const char *name, void *buf, size_t size)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_kabi_getxattr_in in = { .ino = inode->i_ino, .size = size };
	struct aios_kabi_xattr_out *hdr;
	void *out = NULL;
	u32 out_len = 0;
	int err;

	if (!name || !*name || strlen(name) > AIOS_KABI_NAME_MAX)
		return -EINVAL;
	strscpy(in.name, name, sizeof(in.name));

	err = aios_upcall(info->conn, AIOS_OP_GETXATTR, info->mount_id, &in, sizeof(in), &out,
			  &out_len);
	if (err)
		return err;
	if (out_len < sizeof(*hdr)) {
		kfree(out);
		return -EIO;
	}
	hdr = out;
	if (size == 0) {
		err = (int)hdr->size;
		kfree(out);
		return err;
	}
	if (hdr->size > size) {
		kfree(out);
		return -ERANGE;
	}
	if (out_len < sizeof(*hdr) + hdr->size) {
		kfree(out);
		return -EIO;
	}
	if (buf && hdr->size)
		memcpy(buf, hdr + 1, hdr->size);
	err = (int)hdr->size;
	kfree(out);
	return err;
}

static int upcall_setxattr(struct inode *inode, const char *name, const void *buf, size_t size,
			   int flags)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_kabi_setxattr_in *in;
	size_t in_len;
	void *payload;
	int err;

	if (!name || !*name || strlen(name) > AIOS_KABI_NAME_MAX)
		return -EINVAL;
	if (size > AIOS_XATTR_VALUE_MAX)
		return -E2BIG;
	if (size && !buf)
		return -EINVAL;

	in_len = sizeof(*in) + size;
	payload = kmalloc(in_len, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	in = payload;
	memset(in, 0, sizeof(*in));
	in->ino = inode->i_ino;
	in->flags = flags;
	in->value_len = size;
	strscpy(in->name, name, sizeof(in->name));
	if (size)
		memcpy(in + 1, buf, size);

	err = aios_upcall(info->conn, AIOS_OP_SETXATTR, info->mount_id, payload, in_len, NULL,
			  NULL);
	kfree(payload);
	return err;
}

static int upcall_listxattr(struct inode *inode, char *list, size_t size)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_kabi_listxattr_in in = { .ino = inode->i_ino, .size = size };
	struct aios_kabi_xattr_out *hdr;
	void *out = NULL;
	u32 out_len = 0;
	int err;

	err = aios_upcall(info->conn, AIOS_OP_LISTXATTR, info->mount_id, &in, sizeof(in), &out,
			  &out_len);
	if (err)
		return err;
	if (out_len < sizeof(*hdr)) {
		kfree(out);
		return -EIO;
	}
	hdr = out;
	if (size == 0) {
		err = (int)hdr->size;
		kfree(out);
		return err;
	}
	if (hdr->size > size) {
		kfree(out);
		return -ERANGE;
	}
	if (out_len < sizeof(*hdr) + hdr->size) {
		kfree(out);
		return -EIO;
	}
	if (list && hdr->size)
		memcpy(list, hdr + 1, hdr->size);
	err = (int)hdr->size;
	kfree(out);
	return err;
}

static int upcall_removexattr(struct inode *inode, const char *name)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_kabi_removexattr_in in = { .ino = inode->i_ino };

	if (!name || !*name || strlen(name) > AIOS_KABI_NAME_MAX)
		return -EINVAL;
	strscpy(in.name, name, sizeof(in.name));
	return aios_upcall(info->conn, AIOS_OP_REMOVEXATTR, info->mount_id, &in, sizeof(in), NULL,
			   NULL);
}

int aios_getxattr(struct inode *inode, const char *name, void *buf, size_t size)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);

	if (info->backend == AIOS_BACKEND_HTTP)
		return aios_http_getxattr(inode, name, buf, size);
	return upcall_getxattr(inode, name, buf, size);
}

int aios_setxattr(struct inode *inode, const char *name, const void *buf, size_t size, int flags)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	int kflags = 0;

	if (flags & XATTR_CREATE)
		kflags |= AIOS_KABI_XATTR_CREATE;
	if (flags & XATTR_REPLACE)
		kflags |= AIOS_KABI_XATTR_REPLACE;

	if (info->backend == AIOS_BACKEND_HTTP)
		return aios_http_setxattr(inode, name, buf, size, kflags);
	return upcall_setxattr(inode, name, buf, size, kflags);
}

int aios_listxattr(struct dentry *dentry, char *list, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);

	if (info->backend == AIOS_BACKEND_HTTP)
		return aios_http_listxattr(inode, list, size);
	return upcall_listxattr(inode, list, size);
}

int aios_removexattr(struct inode *inode, const char *name)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);

	if (info->backend == AIOS_BACKEND_HTTP)
		return aios_http_removexattr(inode, name);
	return upcall_removexattr(inode, name);
}

static int aios_xattr_get(const struct xattr_handler *handler, struct dentry *dentry,
			  struct inode *inode, const char *name, void *buffer, size_t size)
{
	const char *full = xattr_full_name(handler, name);

	return aios_getxattr(inode, full, buffer, size);
}

static int aios_xattr_set(const struct xattr_handler *handler, struct user_namespace *mnt_userns,
			  struct dentry *dentry, struct inode *inode, const char *name,
			  const void *buffer, size_t size, int flags)
{
	const char *full = xattr_full_name(handler, name);

	if (!buffer && size == 0)
		return aios_removexattr(inode, full);
	return aios_setxattr(inode, full, buffer, size, flags);
}

static const struct xattr_handler aios_user_xattr_handler = {
	.prefix = XATTR_USER_PREFIX,
	.get = aios_xattr_get,
	.set = aios_xattr_set,
};

static const struct xattr_handler aios_trusted_xattr_handler = {
	.prefix = XATTR_TRUSTED_PREFIX,
	.get = aios_xattr_get,
	.set = aios_xattr_set,
};

const struct xattr_handler *aios_xattr_handlers[] = {
	&aios_user_xattr_handler,
	&aios_trusted_xattr_handler,
	NULL,
};
