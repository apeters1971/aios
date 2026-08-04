// SPDX-License-Identifier: GPL-2.0
/*
 * Page-cache buffered I/O + writeback for aiosfs (AlmaLinux 9 / 5.14).
 *
 * Reads fill pages via aios_io_read; dirty pages are flushed by writepage /
 * writepages through aios_io_write. Userspace sees generic_file_read_iter /
 * generic_file_write_iter.
 */
#include "aiosfs.h"

#include <linux/pagemap.h>
#include <linux/writeback.h>

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

int aios_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	loff_t size;

	if (!S_ISREG(inode->i_mode))
		return 0;
	size = i_size_read(inode);
	return aios_io_set_size(inode, size);
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
