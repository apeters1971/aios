// SPDX-License-Identifier: GPL-2.0
#include "aiosvd.h"

#include <linux/blk-mq.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

static void aiosvd_req_workfn(struct work_struct *work)
{
	struct aiosvd_req *areq = container_of(work, struct aiosvd_req, work);
	struct request *rq = areq->rq;
	int err;

	err = aiosvd_dev_io(areq->dev, rq);
	blk_mq_end_request(rq, errno_to_blk_status(err));
	kfree(areq);
}

static blk_status_t aiosvd_queue_rq(struct blk_mq_hw_ctx *hctx,
				    const struct blk_mq_queue_data *bd)
{
	struct aiosvd_device *dev = hctx->queue->queuedata;
	struct request *rq = bd->rq;
	struct aiosvd_req *areq;

	areq = kmalloc(sizeof(*areq), GFP_NOIO);
	if (!areq)
		return BLK_STS_RESOURCE;

	blk_mq_start_request(rq);
	areq->rq = rq;
	areq->dev = dev;
	INIT_WORK(&areq->work, aiosvd_req_workfn);
	if (!queue_work(dev->req_wq, &areq->work)) {
		kfree(areq);
		return BLK_STS_RESOURCE;
	}
	return BLK_STS_OK;
}

static const struct blk_mq_ops aiosvd_mq_ops = {
	.queue_rq = aiosvd_queue_rq,
};

static int aiosvd_open(struct block_device *bdev, fmode_t mode)
{
	struct aiosvd_device *dev = bdev->bd_disk->private_data;

	mutex_lock(&dev->open_mu);
	dev->open_count++;
	mutex_unlock(&dev->open_mu);
	return 0;
}

static void aiosvd_release(struct gendisk *disk, fmode_t mode)
{
	struct aiosvd_device *dev = disk->private_data;

	mutex_lock(&dev->open_mu);
	if (dev->open_count > 0)
		dev->open_count--;
	mutex_unlock(&dev->open_mu);
}

static ssize_t stats_show(struct device *d, struct device_attribute *attr, char *buf)
{
	struct aiosvd_device *dev = dev_get_drvdata(d);
	u64 timeouts = 0, reconnects = 0;
	unsigned int nclients = 0;

	if (!dev)
		return -ENODEV;
	if (dev->http_pool) {
		aios_http_pool_get_stats(dev->http_pool, &timeouts, &reconnects);
		nclients = aios_http_pool_size(dev->http_pool);
	}
	return sysfs_emit(buf,
			  "pool=%s\nname=%s\nparent_pool=%s\nparent_name=%s\n"
			  "key_id=%s\nsize=%llu\nobj_order=%u\nflags=%u\nnclients=%u\n"
			  "queue_depth=%u\nbytes_read=%llu\nbytes_written=%llu\n"
			  "ops_read=%llu\nops_write=%llu\nops_discard=%llu\nerrors=%llu\n"
			  "timeouts=%llu\nreconnects=%llu\ncache_hits=%llu\n",
			  dev->pool, dev->name, dev->parent_pool, dev->parent_name, dev->key_id,
			  (unsigned long long)dev->size, dev->obj_order, dev->flags, nclients,
			  dev->queue_depth, (unsigned long long)atomic64_read(&dev->bytes_read),
			  (unsigned long long)atomic64_read(&dev->bytes_written),
			  (unsigned long long)atomic64_read(&dev->ops_read),
			  (unsigned long long)atomic64_read(&dev->ops_write),
			  (unsigned long long)atomic64_read(&dev->ops_discard),
			  (unsigned long long)atomic64_read(&dev->errors),
			  (unsigned long long)timeouts, (unsigned long long)reconnects,
			  (unsigned long long)atomic64_read(&dev->cache_hits));
}
static DEVICE_ATTR_RO(stats);

static struct attribute *aiosvd_attrs[] = {
	&dev_attr_stats.attr,
	NULL,
};

static const struct attribute_group aiosvd_attr_group = {
	.name = "aiosvd",
	.attrs = aiosvd_attrs,
};

static const struct attribute_group *aiosvd_attr_groups[] = {
	&aiosvd_attr_group,
	NULL,
};

static const struct block_device_operations aiosvd_ops = {
	.owner = THIS_MODULE,
	.open = aiosvd_open,
	.release = aiosvd_release,
};

static void aiosvd_destroy_disk(struct aiosvd_device *dev)
{
	if (!dev->disk)
		return;
	del_gendisk(dev->disk);
	blk_cleanup_disk(dev->disk);
	dev->disk = NULL;
	blk_mq_free_tag_set(&dev->tag_set);
	if (dev->stripe_wq) {
		destroy_workqueue(dev->stripe_wq);
		dev->stripe_wq = NULL;
	}
	if (dev->req_wq) {
		destroy_workqueue(dev->req_wq);
		dev->req_wq = NULL;
	}
}

int aiosvd_create_disk(struct aiosvd_device *dev)
{
	struct gendisk *disk;
	unsigned int hwqs;
	unsigned int max_sectors;
	unsigned int qd;
	int err;

	dev->req_wq = alloc_workqueue("aiosvd%d-rq", WQ_MEM_RECLAIM | WQ_UNBOUND, 0, dev->id);
	if (!dev->req_wq)
		return -ENOMEM;
	dev->stripe_wq =
		alloc_workqueue("aiosvd%d-io", WQ_MEM_RECLAIM | WQ_UNBOUND, 0, dev->id);
	if (!dev->stripe_wq) {
		err = -ENOMEM;
		goto err_req_wq;
	}

	hwqs = num_online_cpus();
	if (hwqs < 1)
		hwqs = 1;
	if (hwqs > 4)
		hwqs = 4;

	qd = dev->queue_depth ? dev->queue_depth : AIOSVD_DEFAULT_QUEUE_DEPTH;
	if (qd < 1)
		qd = 1;
	if (qd > 1024)
		qd = 1024;
	dev->queue_depth = qd;

	memset(&dev->tag_set, 0, sizeof(dev->tag_set));
	dev->tag_set.ops = &aiosvd_mq_ops;
	dev->tag_set.nr_hw_queues = hwqs;
	dev->tag_set.queue_depth = qd;
	dev->tag_set.numa_node = NUMA_NO_NODE;
	dev->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
	dev->tag_set.cmd_size = 0;
	dev->tag_set.driver_data = dev;

	err = blk_mq_alloc_tag_set(&dev->tag_set);
	if (err)
		goto err_stripe_wq;

	disk = blk_mq_alloc_disk(&dev->tag_set, dev);
	if (IS_ERR(disk)) {
		err = PTR_ERR(disk);
		goto err_tag;
	}

	disk->major = aiosvd_major;
	disk->first_minor = dev->id;
	disk->minors = 1;
	disk->fops = &aiosvd_ops;
	disk->private_data = dev;
	snprintf(disk->disk_name, DISK_NAME_LEN, "%s%d", AIOSVD_DISK_PREFIX, dev->id);
	set_capacity(disk, dev->size >> SECTOR_SHIFT);
	blk_queue_logical_block_size(disk->queue, 4096);
	blk_queue_physical_block_size(disk->queue, 4096);
	blk_queue_io_min(disk->queue, 4096);
	blk_queue_io_opt(disk->queue, dev->obj_size);

	/* Multi-object I/O: up to 16 objects per request where safe. */
	max_sectors = (dev->obj_size >> SECTOR_SHIFT) * 16;
	if (max_sectors < (dev->obj_size >> SECTOR_SHIFT))
		max_sectors = dev->obj_size >> SECTOR_SHIFT;
	blk_queue_max_hw_sectors(disk->queue, max_sectors);
	blk_queue_max_segments(disk->queue, 256);
	blk_queue_max_discard_sectors(disk->queue, max_sectors);
	blk_queue_max_write_zeroes_sectors(disk->queue, max_sectors);
	blk_queue_discard_granularity(disk->queue, 4096);
	blk_queue_flag_set(QUEUE_FLAG_NONROT, disk->queue);
	blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, disk->queue);
	blk_queue_write_cache(disk->queue, true, true); /* WC + FUA */

	disk->queue->limits.discard_granularity = 4096;
	disk->queue->limits.max_discard_sectors = max_sectors;
	disk->queue->limits.max_write_zeroes_sectors = max_sectors;

	/* Readahead toward ~2 objects. */
	if (disk->queue->backing_dev_info) {
		unsigned long ra_pages = (2UL * dev->obj_size) / PAGE_SIZE;

		if (ra_pages < 128)
			ra_pages = 128;
		disk->queue->backing_dev_info->ra_pages = ra_pages;
	}

	dev->disk = disk;
	add_disk(disk);
	dev_set_drvdata(disk_to_dev(disk), dev);

	if (sysfs_create_groups(&disk_to_dev(disk)->kobj, aiosvd_attr_groups))
		pr_warn("aiosvd: sysfs stats unavailable for %s\n", disk->disk_name);

	return 0;

err_tag:
	blk_mq_free_tag_set(&dev->tag_set);
err_stripe_wq:
	destroy_workqueue(dev->stripe_wq);
	dev->stripe_wq = NULL;
err_req_wq:
	destroy_workqueue(dev->req_wq);
	dev->req_wq = NULL;
	return err;
}

void aiosvd_teardown_disk(struct aiosvd_device *dev)
{
	if (dev->disk)
		sysfs_remove_groups(&disk_to_dev(dev->disk)->kobj, aiosvd_attr_groups);
	aiosvd_destroy_disk(dev);
}
