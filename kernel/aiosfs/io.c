// SPDX-License-Identifier: GPL-2.0
/*
 * Backend I/O dispatch used by page-cache readpage/writepage.
 */
#include "aiosfs.h"

#include <linux/slab.h>

static int upcall_io_read(struct inode *inode, loff_t pos, void *buf, size_t len,
			  size_t *out_len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_kabi_rw_in in;
	struct aios_kabi_rw_out *hdr;
	void *out = NULL;
	u32 rep_len = 0;
	int err;

	if (len > AIOS_KABI_MAX_PAYLOAD - sizeof(*hdr))
		len = AIOS_KABI_MAX_PAYLOAD - sizeof(*hdr);

	in.ino = inode->i_ino;
	in.offset = (u64)pos;
	in.size = len;
	in._pad = 0;

	err = aios_upcall(info->conn, AIOS_OP_READ, info->mount_id, &in, sizeof(in), &out,
			  &rep_len);
	if (err)
		return err;
	if (rep_len < sizeof(*hdr)) {
		kfree(out);
		return -EIO;
	}
	hdr = out;
	if (hdr->size > rep_len - sizeof(*hdr)) {
		kfree(out);
		return -EIO;
	}
	memcpy(buf, hdr + 1, hdr->size);
	if (out_len)
		*out_len = hdr->size;
	kfree(out);
	return 0;
}

static int upcall_io_write(struct inode *inode, loff_t pos, const void *buf, size_t len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	size_t in_len;
	struct aios_kabi_rw_in *in;
	void *payload;
	void *out = NULL;
	u32 out_len = 0;
	int err;

	if (!len)
		return 0;
	if (len > AIOS_KABI_MAX_PAYLOAD - sizeof(*in))
		len = AIOS_KABI_MAX_PAYLOAD - sizeof(*in);

	in_len = sizeof(*in) + len;
	payload = kmalloc(in_len, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	in = payload;
	in->ino = inode->i_ino;
	in->offset = (u64)pos;
	in->size = len;
	in->_pad = 0;
	memcpy(in + 1, buf, len);

	err = aios_upcall(info->conn, AIOS_OP_WRITE, info->mount_id, payload, in_len, &out,
			  &out_len);
	kfree(payload);
	if (err)
		return err;
	kfree(out);
	return 0;
}

static int upcall_io_set_size(struct inode *inode, loff_t size)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_kabi_setattr_in in = {
		.ino = inode->i_ino,
		.to_set = AIOS_KABI_SET_SIZE,
	};

	in.st.size = (u64)size;
	return aios_upcall(info->conn, AIOS_OP_SETATTR, info->mount_id, &in, sizeof(in), NULL,
			   NULL);
}

static int upcall_io_fsync(struct inode *inode)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_kabi_ino_in in = { .ino = inode->i_ino };

	return aios_upcall(info->conn, AIOS_OP_FSYNC, info->mount_id, &in, sizeof(in), NULL,
			   NULL);
}

int aios_io_read(struct inode *inode, loff_t pos, void *buf, size_t len, size_t *out_len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);

	if (out_len)
		*out_len = 0;
	if (!len)
		return 0;
	if (info->backend == AIOS_BACKEND_HTTP)
		return aios_http_io_read(inode, pos, buf, len, out_len);
	return upcall_io_read(inode, pos, buf, len, out_len);
}

int aios_io_write(struct inode *inode, loff_t pos, const void *buf, size_t len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);

	if (!len)
		return 0;
	if (info->backend == AIOS_BACKEND_HTTP)
		return aios_http_io_write(inode, pos, buf, len);
	return upcall_io_write(inode, pos, buf, len);
}

int aios_io_set_size(struct inode *inode, loff_t size)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);

	if (info->backend == AIOS_BACKEND_HTTP)
		return aios_http_io_set_size(inode, size);
	return upcall_io_set_size(inode, size);
}

int aios_io_fsync(struct inode *inode)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);

	if (info->backend == AIOS_BACKEND_HTTP)
		return 0; /* durability is per writepage PUT */
	return upcall_io_fsync(inode);
}
