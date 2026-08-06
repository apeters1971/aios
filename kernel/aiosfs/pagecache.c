// SPDX-License-Identifier: GPL-2.0
/*
 * Page-cache buffered I/O + writeback + O_DIRECT for aiosfs (AlmaLinux 9 / 5.14).
 *
 * Reads fill pages via aios_io_read; dirty pages are flushed by writepage /
 * writepages through aios_io_write. Userspace sees aios_file_read_iter /
 * aios_file_write_iter (buffered default; IOCB_DIRECT bypasses page cache).
 */
#include "aiosfs.h"

#include <linux/falloc.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/writeback.h>

#define AIOS_DIO_CHUNK (256u * 1024u)

static int aios_readpage(struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	loff_t pos = page_offset(page);
	loff_t i_size = i_size_read(inode);
	void *kaddr;
	size_t got = 0;
	int err = 0;

	kaddr = kmap(page);
	if (pos < i_size) {
		size_t len = min_t(loff_t, PAGE_SIZE, i_size - pos);

		err = aios_io_read(inode, pos, kaddr, len, &got);
		if (!err) {
			if (got < PAGE_SIZE)
				memset((char *)kaddr + got, 0, PAGE_SIZE - got);
		}
	} else {
		memset(kaddr, 0, PAGE_SIZE);
	}
	kunmap(page);
	flush_dcache_page(page);

	if (err) {
		ClearPageUptodate(page);
		SetPageError(page);
	} else {
		SetPageUptodate(page);
		ClearPageError(page);
	}
	unlock_page(page);
	return err;
}

static int aios_write_page_data(struct page *page)
{
	struct inode *inode = page->mapping->host;
	loff_t pos = page_offset(page);
	loff_t i_size = i_size_read(inode);
	size_t len;
	void *kaddr;
	int err;

	if (pos >= i_size)
		return 0;
	len = min_t(loff_t, PAGE_SIZE, i_size - pos);

	kaddr = kmap(page);
	err = aios_io_write(inode, pos, kaddr, len);
	kunmap(page);
	return err;
}

static int aios_writepage(struct page *page, struct writeback_control *wbc)
{
	struct address_space *mapping = page->mapping;
	int err;

	set_page_writeback(page);
	err = aios_write_page_data(page);
	if (err) {
		SetPageError(page);
		mapping_set_error(mapping, err);
		end_page_writeback(page);
		unlock_page(page);
		return err;
	}
	ClearPageError(page);
	end_page_writeback(page);
	unlock_page(page);
	return 0;
}

static int aios_writepages_cb(struct page *page, struct writeback_control *wbc, void *data)
{
	return aios_writepage(page, wbc);
}

static int aios_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);

	if (info->backend == AIOS_BACKEND_HTTP && info->http_pool)
		return aios_http_writepages(mapping, wbc);
	return write_cache_pages(mapping, wbc, aios_writepages_cb, NULL);
}

static int aios_write_begin(struct file *file, struct address_space *mapping, loff_t pos,
			    unsigned len, unsigned flags, struct page **pagep, void **fsdata)
{
	pgoff_t index = pos >> PAGE_SHIFT;
	struct page *page;
	int err;

	page = grab_cache_page_write_begin(mapping, index, flags);
	if (!page)
		return -ENOMEM;

	if (!PageUptodate(page) && (len != PAGE_SIZE)) {
		/* Partial page: pull existing data (or zeros past EOF). */
		loff_t i_size = i_size_read(mapping->host);
		void *kaddr = kmap(page);
		size_t got = 0;

		if (page_offset(page) < i_size) {
			size_t rlen = min_t(loff_t, PAGE_SIZE, i_size - page_offset(page));

			err = aios_io_read(mapping->host, page_offset(page), kaddr, rlen, &got);
			if (err) {
				kunmap(page);
				unlock_page(page);
				put_page(page);
				return err;
			}
			if (got < PAGE_SIZE)
				memset((char *)kaddr + got, 0, PAGE_SIZE - got);
		} else {
			memset(kaddr, 0, PAGE_SIZE);
		}
		kunmap(page);
		flush_dcache_page(page);
		SetPageUptodate(page);
	} else if (!PageUptodate(page) && len == PAGE_SIZE) {
		/* Full-page overwrite — mark uptodate after write_end. */
	}

	*pagep = page;
	return 0;
}

static int aios_write_end(struct file *file, struct address_space *mapping, loff_t pos,
			  unsigned len, unsigned copied, struct page *page, void *fsdata)
{
	struct inode *inode = mapping->host;
	loff_t last;

	if (unlikely(copied < len) && !PageUptodate(page)) {
		zero_user(page, 0, PAGE_SIZE);
		copied = 0;
	}

	if (copied) {
		if (!PageUptodate(page))
			SetPageUptodate(page);
		set_page_dirty(page);
	}

	last = pos + copied;
	if (last > i_size_read(inode)) {
		i_size_write(inode, last);
		mark_inode_dirty(inode);
	}

	unlock_page(page);
	put_page(page);
	return copied;
}

const struct address_space_operations aios_aops = {
	.readpage = aios_readpage,
	.writepage = aios_writepage,
	.writepages = aios_writepages,
	.set_page_dirty = __set_page_dirty_nobuffers,
	.write_begin = aios_write_begin,
	.write_end = aios_write_end,
};

static ssize_t aios_direct_IO(struct kiocb *iocb, struct iov_iter *iter, bool write)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t pos = iocb->ki_pos;
	size_t total = iov_iter_count(iter);
	size_t chunk = min_t(size_t, total, AIOS_DIO_CHUNK);
	void *buf;
	ssize_t done = 0;
	int err = 0;

	if (!total)
		return 0;

	buf = kvmalloc(chunk, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	while (iov_iter_count(iter)) {
		size_t n = min_t(size_t, iov_iter_count(iter), chunk);
		size_t got = 0;

		if (write) {
			if (copy_from_iter(buf, n, iter) != n) {
				err = -EFAULT;
				break;
			}
			err = aios_io_write(inode, pos, buf, n);
			if (err)
				break;
			got = n;
		} else {
			err = aios_io_read(inode, pos, buf, n, &got);
			if (err)
				break;
			if (!got)
				break;
			if (copy_to_iter(buf, got, iter) != got) {
				err = -EFAULT;
				break;
			}
		}
		pos += got;
		done += got;
		if (!write && got < n)
			break;
	}

	kvfree(buf);
	if (done) {
		iocb->ki_pos = pos;
		if (write && pos > i_size_read(inode)) {
			i_size_write(inode, pos);
			mark_inode_dirty(inode);
		}
		return done;
	}
	return err ? err : 0;
}

ssize_t aios_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	if (iocb->ki_flags & IOCB_DIRECT) {
		struct inode *inode = file_inode(iocb->ki_filp);
		struct file *file = iocb->ki_filp;
		loff_t size = i_size_read(inode);
		size_t count = iov_iter_count(to);
		loff_t end;
		int err;

		if (!count || iocb->ki_pos >= size)
			return 0;
		if (iocb->ki_pos + (loff_t)count > size)
			iov_iter_truncate(to, size - iocb->ki_pos);
		count = iov_iter_count(to);
		if (!count)
			return 0;

		end = iocb->ki_pos + count - 1;
		err = filemap_write_and_wait_range(inode->i_mapping, iocb->ki_pos, end);
		if (err)
			return err;
		err = invalidate_inode_pages2_range(inode->i_mapping,
						   iocb->ki_pos >> PAGE_SHIFT,
						   end >> PAGE_SHIFT);
		if (err)
			return err;
		return aios_direct_IO(iocb, to, false);
	}
	return generic_file_read_iter(iocb, to);
}

ssize_t aios_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	if (iocb->ki_flags & IOCB_DIRECT) {
		struct file *file = iocb->ki_filp;
		struct inode *inode = file_inode(file);
		ssize_t ret;
		int err;

		inode_lock(inode);
		ret = generic_write_checks(iocb, from);
		if (ret > 0) {
			size_t count = iov_iter_count(from);

			err = file_remove_privs(file);
			if (!err)
				err = file_update_time(file);
			if (err) {
				ret = err;
			} else if (!count) {
				ret = 0;
			} else {
				loff_t endbyte = iocb->ki_pos + count - 1;

				err = filemap_write_and_wait_range(inode->i_mapping,
								   iocb->ki_pos, endbyte);
				if (!err)
					err = invalidate_inode_pages2_range(inode->i_mapping,
									    iocb->ki_pos >> PAGE_SHIFT,
									    endbyte >> PAGE_SHIFT);
				if (err)
					ret = err;
				else
					ret = aios_direct_IO(iocb, from, true);
				if (ret > 0)
					invalidate_inode_pages2_range(inode->i_mapping,
								      iocb->ki_pos >> PAGE_SHIFT,
								      (iocb->ki_pos + ret - 1) >>
									      PAGE_SHIFT);
			}
		}
		inode_unlock(inode);
		return ret;
	}
	return generic_file_write_iter(iocb, from);
}

long aios_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	int err;

	if (offset < 0 || len <= 0)
		return -EINVAL;
	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
		return -EOPNOTSUPP;
	if ((mode & FALLOC_FL_PUNCH_HOLE) && !(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;
	if (!(mode & FALLOC_FL_PUNCH_HOLE))
		return -EOPNOTSUPP; /* prealloc not implemented */

	if (!S_ISREG(inode->i_mode))
		return -ENODEV;

	inode_lock(inode);
	if (info->backend == AIOS_BACKEND_HTTP) {
		err = aios_http_io_punch(inode, offset, len);
		if (!err)
			truncate_pagecache_range(inode, offset, offset + len - 1);
	} else {
		err = -EOPNOTSUPP;
	}
	inode_unlock(inode);
	return err;
}

int aios_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct aios_inode_aux *aux = inode->i_private;
	loff_t size;
	int err;

	if (!S_ISREG(inode->i_mode))
		return 0;
	size = i_size_read(inode);
	if (!aux) {
		aux = kzalloc(sizeof(*aux), GFP_KERNEL);
		if (!aux)
			return -ENOMEM;
		aux->last_synced_size = (u64)size;
		inode->i_private = aux;
	}
	if (aux->last_synced_size == (u64)size)
		return 0;
	if ((u64)size < aux->last_synced_size)
		return 0;
	err = aios_io_set_size(inode, size);
	if (!err)
		aux->last_synced_size = (u64)size;
	return err;
}

void aios_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	kfree(inode->i_private);
	inode->i_private = NULL;
}

void aios_setup_file_inode(struct inode *inode)
{
	inode->i_mapping->a_ops = &aios_aops;
	inode->i_fop = &aios_file_ops;
}
