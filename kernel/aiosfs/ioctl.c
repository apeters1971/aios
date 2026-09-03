// SPDX-License-Identifier: GPL-2.0
/*
 * Filesystem ioctls for aiosfs. PREFETCHV uses the same inline range vector
 * as FUSE (kernel/aiosfs_uapi.h): one copy_from_user, no nested pointers.
 */
#include "aiosfs.h"
#include "../aiosfs_uapi.h"

#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/uaccess.h>

static int aios_prefetch_cmp_u64(const void *a, const void *b)
{
	u64 va = *(const u64 *)a;
	u64 vb = *(const u64 *)b;

	if (va < vb)
		return -1;
	if (va > vb)
		return 1;
	return 0;
}

static int aios_prefetch_cmp_range(const void *a, const void *b)
{
	const struct aios_range *ra = a;
	const struct aios_range *rb = b;

	if (ra->offset < rb->offset)
		return -1;
	if (ra->offset > rb->offset)
		return 1;
	if (ra->length < rb->length)
		return -1;
	if (ra->length > rb->length)
		return 1;
	return 0;
}

static void aios_prefetch_normalize(struct aios_range *ranges, u32 *nranges)
{
	u32 n, i, out;
	u64 prev_end, cur_end;

	if (!ranges || !nranges || *nranges < 2)
		return;
	n = *nranges;
	sort(ranges, n, sizeof(*ranges), aios_prefetch_cmp_range, NULL);
	out = 0;
	for (i = 0; i < n; i++) {
		if (out == 0) {
			ranges[out++] = ranges[i];
			continue;
		}
		prev_end = ranges[out - 1].offset + ranges[out - 1].length;
		if (ranges[i].offset <= prev_end) {
			cur_end = ranges[i].offset + ranges[i].length;
			if (cur_end > prev_end)
				ranges[out - 1].length = cur_end - ranges[out - 1].offset;
		} else {
			ranges[out++] = ranges[i];
		}
	}
	*nranges = out;
}

static int aios_prefetch_validate(const struct aios_range *ranges, u32 nranges, u32 flags,
				  u64 file_size)
{
	u32 i;
	u64 total = 0, end;

	if (nranges == 0)
		return -EINVAL;
	if (nranges > AIOS_PREFETCH_MAX_RANGES)
		return -E2BIG;
	if (flags & ~AIOS_PREFETCH_SUPPORTED_FLAGS)
		return -EOPNOTSUPP;

	for (i = 0; i < nranges; i++) {
		if (ranges[i].length == 0)
			return -EINVAL;
		if (check_add_overflow(ranges[i].offset, ranges[i].length, &end))
			return -EINVAL;
		if (ranges[i].offset >= file_size || end > file_size)
			return -EINVAL;
		if (check_add_overflow(total, ranges[i].length, &total))
			return -EOVERFLOW;
		if (total > AIOS_PREFETCH_MAX_BYTES)
			return -E2BIG;
	}
	return 0;
}

static int aios_prefetch_collect_chunks(const struct aios_range *ranges, u32 nranges, u64 unit,
					u64 **chunks_out, u32 *nchunks_out)
{
	u32 i, n = 0, w, r;
	u64 *chunks;
	u64 first, last, c;

	if (!unit)
		return -EINVAL;
	for (i = 0; i < nranges; i++) {
		first = ranges[i].offset / unit;
		last = (ranges[i].offset + ranges[i].length - 1) / unit;
		if (last < first || n > UINT_MAX - (u32)(last - first + 1))
			return -E2BIG;
		n += (u32)(last - first + 1);
	}
	if (!n) {
		*chunks_out = NULL;
		*nchunks_out = 0;
		return 0;
	}
	chunks = kvmalloc_array(n, sizeof(*chunks), GFP_KERNEL);
	if (!chunks)
		return -ENOMEM;
	n = 0;
	for (i = 0; i < nranges; i++) {
		first = ranges[i].offset / unit;
		last = ranges[i].offset + ranges[i].length - 1;
		last /= unit;
		for (c = first; c <= last; c++)
			chunks[n++] = c;
	}
	sort(chunks, n, sizeof(*chunks), aios_prefetch_cmp_u64, NULL);
	w = 0;
	for (r = 0; r < n; r++) {
		if (w == 0 || chunks[r] != chunks[w - 1])
			chunks[w++] = chunks[r];
	}
	*chunks_out = chunks;
	*nchunks_out = w;
	return 0;
}

static int aios_prefetch_upcall(struct inode *inode, const struct aios_prefetchv *req)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_kabi_prefetchv_in *in;
	int err;

	in = kmalloc(sizeof(*in), GFP_KERNEL);
	if (!in)
		return -ENOMEM;
	in->ino = inode->i_ino;
	in->req = *req;
	err = aios_upcall(info->conn, AIOS_OP_PREFETCHV, info->mount_id, in, sizeof(*in), NULL,
			  NULL);
	kfree(in);
	return err;
}

static u64 aios_prefetch_unit(struct inode *inode)
{
	struct aios_inode_aux *aux = inode->i_private;
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);

	if (aux && aux->stripe_unit)
		return aux->stripe_unit;
	if (info && info->stripe_unit)
		return info->stripe_unit;
	return 1024ull * 1024ull;
}

static int aios_prefetch_populate(struct inode *inode, const struct aios_range *ranges,
				  u32 nranges, u64 unit)
{
	u64 *chunks = NULL;
	u32 nchunks = 0, i;
	u64 file_size = (u64)i_size_read(inode);
	void *buf;
	int err;

	err = aios_prefetch_collect_chunks(ranges, nranges, unit, &chunks, &nchunks);
	if (err)
		return err;
	if (!nchunks)
		return 0;

	buf = kvmalloc(unit, GFP_KERNEL);
	if (!buf) {
		kvfree(chunks);
		return -ENOMEM;
	}

	for (i = 0; i < nchunks; i++) {
		u64 start = chunks[i] * unit;
		u64 end = start + unit;
		size_t got = 0;

		if (start >= file_size)
			continue;
		if (end > file_size)
			end = file_size;
		memset(buf, 0, unit);
		err = aios_io_read(inode, (loff_t)start, buf, (size_t)(end - start), &got);
		if (err)
			break;
		err = aios_prefetch_copy_pages(inode, (loff_t)start, buf, (size_t)(end - start));
		if (err)
			break;
	}
	kvfree(buf);
	kvfree(chunks);
	return err;
}

static long aios_ioctl_prefetchv(struct file *file, void __user *argp)
{
	struct inode *inode = file_inode(file);
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_prefetchv *req;
	u64 file_size;
	int err;

	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!S_ISREG(inode->i_mode))
		return -ENOTTY;

	req = memdup_user(argp, sizeof(*req));
	if (IS_ERR(req))
		return PTR_ERR(req);

	file_size = (u64)i_size_read(inode);
	err = aios_prefetch_validate(req->ranges, req->nranges, req->flags, file_size);
	if (err)
		goto out;
	aios_prefetch_normalize(req->ranges, &req->nranges);

	if (info->backend == AIOS_BACKEND_UPCALL) {
		err = aios_prefetch_upcall(inode, req);
		if (err)
			goto out;
	}

	err = aios_prefetch_populate(inode, req->ranges, req->nranges, aios_prefetch_unit(inode));
out:
	kfree(req);
	return err;
}

long aios_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case AIOS_IOC_PREFETCHV:
		return aios_ioctl_prefetchv(file, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}
