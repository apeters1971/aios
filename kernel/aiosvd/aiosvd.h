/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AIOSVD_H
#define AIOSVD_H

#include <linux/atomic.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

#include "../aios_http/aios_http_api.h"
#include "../aiosvd_uapi.h"

#define AIOSVD_MIN_OBJ_ORDER 16 /* 64 KiB */
#define AIOSVD_MAX_OBJ_ORDER 24 /* 16 MiB (aios_http body cap) */
#define AIOSVD_MAX_CLIENTS 8
#define AIOSVD_DEFAULT_QUEUE_DEPTH 128
#define AIOSVD_OCACHE_SIZE 16

struct aiosvd_ocache_ent {
	bool valid;
	bool dirty;
	u64 objno;
	void *data; /* obj_size when valid */
	u32 datalen;
};

struct aiosvd_device {
	int id;
	bool mapped;
	bool readonly;
	char pool[AIOSVD_POOL_MAX + 1];
	char name[AIOSVD_NAME_MAX + 1];
	char parent_pool[AIOSVD_POOL_MAX + 1];
	char parent_name[AIOSVD_NAME_MAX + 1];
	char key_id[AIOSVD_KEY_ID_MAX + 1];
	char endpoint[256];
	char cluster_key[256];
	char app_label[64];
	u64 size;
	u32 obj_order;
	u32 obj_size;
	u32 flags;
	u32 queue_depth;
	struct aios_http_pool *http_pool;
	struct gendisk *disk;
	struct blk_mq_tag_set tag_set;
	struct workqueue_struct *req_wq; /* request dispatcher */
	struct workqueue_struct *stripe_wq; /* parallel stripe I/O */
	struct mutex open_mu;
	int open_count;
	/* Object cache (RMW / reuse) */
	struct mutex cache_mu;
	struct aiosvd_ocache_ent cache[AIOSVD_OCACHE_SIZE];
	/* Header CAS for resize / rename */
	u64 header_cas;
	/* Stats */
	atomic64_t bytes_read;
	atomic64_t bytes_written;
	atomic64_t ops_read;
	atomic64_t ops_write;
	atomic64_t ops_discard;
	atomic64_t errors;
	atomic64_t cache_hits;
};

struct aiosvd_req {
	struct work_struct work;
	struct request *rq;
	struct aiosvd_device *dev;
};

int aiosvd_ctl_init(void);
void aiosvd_ctl_exit(void);

int aiosvd_map(struct aiosvd_map_arg *arg);
int aiosvd_unmap(int dev_id);
int aiosvd_info(struct aiosvd_info_arg *arg);
int aiosvd_list(struct aiosvd_list_arg *arg);
int aiosvd_resize(struct aiosvd_resize_arg *arg);
int aiosvd_clone(struct aiosvd_clone_arg *arg);
int aiosvd_rename(struct aiosvd_rename_arg *arg);
int aiosvd_create_disk(struct aiosvd_device *dev);
void aiosvd_teardown_disk(struct aiosvd_device *dev);

struct aios_http_client *aiosvd_client_get(struct aiosvd_device *dev);
void aiosvd_client_put(struct aiosvd_device *dev, struct aios_http_client *c);
int aiosvd_clients_create(struct aiosvd_device *dev, const char *endpoint, const char *key,
			  const char *app_label, u32 n);
void aiosvd_clients_destroy(struct aiosvd_device *dev);

int aiosvd_dev_io(struct aiosvd_device *dev, struct request *rq);
int aiosvd_cache_flush(struct aiosvd_device *dev);
void aiosvd_cache_destroy(struct aiosvd_device *dev);
void aiosvd_cache_invalidate(struct aiosvd_device *dev);

void aiosvd_header_oid(const char *pool, const char *name, char *out, size_t n);
void aiosvd_data_oid(const char *pool, const char *name, u64 objno, char *out, size_t n);
int aiosvd_header_parse(const char *js, size_t len, u64 *size, u32 *obj_order,
			char *parent_pool, size_t pp_len, char *parent_name, size_t pn_len,
			char *key_id, size_t kid_len);
int aiosvd_header_format(char *buf, size_t n, const char *pool, const char *name, u64 size,
			 u32 obj_order, const char *parent_pool, const char *parent_name,
			 const char *key_id);
void aiosvd_fill_info(struct aiosvd_device *dev, struct aiosvd_info_arg *arg);

extern int aiosvd_major;
extern struct aiosvd_device aiosvd_devs[AIOSVD_MAX_DEVS];
extern struct mutex aiosvd_devs_mu;

#endif /* AIOSVD_H */
