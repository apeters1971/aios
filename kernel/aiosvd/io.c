// SPDX-License-Identifier: GPL-2.0
/*
 * Stripe I/O: logical offsets → vd/{pool}/{name}/data.{objno} objects.
 * Multi-object requests fan out across a shared aios_http client pool.
 * Small per-device object cache; partial writes prefer Content-Range PUT.
 */
#include "aiosvd.h"

#include <linux/bio.h>
#include <linux/blk-mq.h>
#include <linux/completion.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/string.h>

static bool json_get_u64(const char *json, const char *key, u64 *out)
{
	char pat[64];
	const char *p;
	int n;

	n = snprintf(pat, sizeof(pat), "\"%s\":", key);
	if (n < 0 || n >= (int)sizeof(pat))
		return false;
	p = strstr(json, pat);
	if (!p)
		return false;
	p += n;
	while (*p == ' ' || *p == '\t')
		p++;
	return kstrtou64(p, 10, out) == 0;
}

static bool json_get_u32(const char *json, const char *key, u32 *out)
{
	u64 v;

	if (!json_get_u64(json, key, &v))
		return false;
	*out = (u32)v;
	return true;
}

static bool json_get_str(const char *json, const char *key, char *out, size_t out_len)
{
	char pat[80];
	const char *p;
	size_t i;
	int n;

	if (!out || !out_len)
		return false;
	out[0] = '\0';
	n = snprintf(pat, sizeof(pat), "\"%s\":\"", key);
	if (n < 0 || n >= (int)sizeof(pat))
		return false;
	p = strstr(json, pat);
	if (!p)
		return false;
	p += n;
	for (i = 0; *p && *p != '"' && i + 1 < out_len; i++, p++)
		out[i] = *p;
	out[i] = '\0';
	return *p == '"';
}

int aiosvd_header_parse(const char *js, size_t len, u64 *size, u32 *obj_order,
			char *parent_pool, size_t pp_len, char *parent_name, size_t pn_len,
			char *key_id, size_t kid_len)
{
	char *buf;
	u64 sz = 0;
	u32 order = AIOSVD_DEFAULT_OBJ_ORDER;

	buf = kmalloc(len + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, js, len);
	buf[len] = '\0';
	if (!json_get_u64(buf, "size", &sz) || !sz) {
		kfree(buf);
		return -EINVAL;
	}
	json_get_u32(buf, "obj_order", &order);
	if (parent_pool && pp_len)
		json_get_str(buf, "parent_pool", parent_pool, pp_len);
	if (parent_name && pn_len)
		json_get_str(buf, "parent_name", parent_name, pn_len);
	if (key_id && kid_len)
		json_get_str(buf, "key_id", key_id, kid_len);
	kfree(buf);
	if (order < AIOSVD_MIN_OBJ_ORDER || order > AIOSVD_MAX_OBJ_ORDER)
		return -EINVAL;
	*size = sz;
	*obj_order = order;
	return 0;
}

int aiosvd_header_format(char *buf, size_t n, const char *pool, const char *name, u64 size,
			 u32 obj_order, const char *parent_pool, const char *parent_name,
			 const char *key_id)
{
	int w;

	if (parent_pool && parent_pool[0] && parent_name && parent_name[0]) {
		w = snprintf(buf, n,
			     "{\"aiosvd\":1,\"pool\":\"%s\",\"name\":\"%s\",\"size\":%llu,"
			     "\"obj_order\":%u,\"parent_pool\":\"%s\",\"parent_name\":\"%s\","
			     "\"key_id\":\"%s\"}",
			     pool, name, (unsigned long long)size, obj_order, parent_pool,
			     parent_name, key_id ? key_id : "");
	} else {
		w = snprintf(buf, n,
			     "{\"aiosvd\":1,\"pool\":\"%s\",\"name\":\"%s\",\"size\":%llu,"
			     "\"obj_order\":%u,\"key_id\":\"%s\"}",
			     pool, name, (unsigned long long)size, obj_order,
			     key_id ? key_id : "");
	}
	return (w < 0 || (size_t)w >= n) ? -EOVERFLOW : 0;
}

void aiosvd_fill_info(struct aiosvd_device *dev, struct aiosvd_info_arg *arg)
{
	u64 timeouts = 0, reconnects = 0;

	arg->dev_id = dev->id;
	strscpy(arg->pool, dev->pool, sizeof(arg->pool));
	strscpy(arg->name, dev->name, sizeof(arg->name));
	strscpy(arg->parent_pool, dev->parent_pool, sizeof(arg->parent_pool));
	strscpy(arg->parent_name, dev->parent_name, sizeof(arg->parent_name));
	strscpy(arg->key_id, dev->key_id, sizeof(arg->key_id));
	arg->size = dev->size;
	arg->obj_order = dev->obj_order;
	arg->flags = dev->flags;
	arg->bytes_read = atomic64_read(&dev->bytes_read);
	arg->bytes_written = atomic64_read(&dev->bytes_written);
	arg->ops_read = atomic64_read(&dev->ops_read);
	arg->ops_write = atomic64_read(&dev->ops_write);
	arg->ops_discard = atomic64_read(&dev->ops_discard);
	arg->errors = atomic64_read(&dev->errors);
	arg->cache_hits = atomic64_read(&dev->cache_hits);
	if (dev->http_pool)
		aios_http_pool_get_stats(dev->http_pool, &timeouts, &reconnects);
	arg->timeouts = timeouts;
	arg->reconnects = reconnects;
}

static bool buffer_is_zero(const void *buf, size_t len)
{
	const unsigned long *p = buf;
	size_t n = len / sizeof(unsigned long);
	size_t i;
	const u8 *b;

	for (i = 0; i < n; i++) {
		if (p[i])
			return false;
	}
	b = buf;
	for (i = n * sizeof(unsigned long); i < len; i++) {
		if (b[i])
			return false;
	}
	return true;
}

static int rq_copy_linear(struct request *rq, size_t rq_off, void *buf, size_t len, bool to_rq)
{
	struct req_iterator iter;
	struct bio_vec bvec;
	size_t pos = 0;
	size_t done = 0;

	rq_for_each_segment(bvec, rq, iter) {
		size_t seg_start = pos;
		size_t seg_end = pos + bvec.bv_len;
		size_t copy_off, copy_len;
		void *kaddr;

		pos = seg_end;
		if (seg_end <= rq_off)
			continue;
		if (seg_start >= rq_off + len)
			break;
		copy_off = rq_off > seg_start ? rq_off - seg_start : 0;
		copy_len = min(seg_end, rq_off + len) - (seg_start + copy_off);
		kaddr = kmap_local_page(bvec.bv_page);
		if (to_rq)
			memcpy(kaddr + bvec.bv_offset + copy_off, (char *)buf + done, copy_len);
		else
			memcpy((char *)buf + done, kaddr + bvec.bv_offset + copy_off, copy_len);
		kunmap_local(kaddr);
		done += copy_len;
		if (done >= len)
			break;
	}
	return done == len ? 0 : -EIO;
}

static bool has_parent(struct aiosvd_device *dev)
{
	return dev->parent_pool[0] && dev->parent_name[0];
}

/* ---- object cache ------------------------------------------------------ */

static struct aiosvd_ocache_ent *cache_lookup(struct aiosvd_device *dev, u64 objno)
{
	unsigned int i;

	for (i = 0; i < AIOSVD_OCACHE_SIZE; i++) {
		if (dev->cache[i].valid && dev->cache[i].objno == objno)
			return &dev->cache[i];
	}
	return NULL;
}

static struct aiosvd_ocache_ent *cache_victim(struct aiosvd_device *dev)
{
	unsigned int i;

	for (i = 0; i < AIOSVD_OCACHE_SIZE; i++) {
		if (!dev->cache[i].valid)
			return &dev->cache[i];
	}
	for (i = 0; i < AIOSVD_OCACHE_SIZE; i++) {
		if (!dev->cache[i].dirty)
			return &dev->cache[i];
	}
	return &dev->cache[0];
}

static int cache_writeback_ent(struct aiosvd_device *dev, struct aios_http_client *http,
			       struct aiosvd_ocache_ent *ent)
{
	char oid[192];
	int err;

	if (!ent->valid || !ent->dirty)
		return 0;
	aiosvd_data_oid(dev->pool, dev->name, ent->objno, oid, sizeof(oid));
	if (!ent->datalen || buffer_is_zero(ent->data, ent->datalen)) {
		err = aios_http_delete(http, oid);
		if (err == -ENOENT)
			err = 0;
	} else {
		err = aios_http_put(http, oid, ent->data, ent->datalen, NULL, NULL);
	}
	if (!err)
		ent->dirty = false;
	return err;
}

int aiosvd_cache_flush(struct aiosvd_device *dev)
{
	struct aios_http_client *http;
	unsigned int i;
	int err = 0;

	/* Drain stripe work first; take a client before cache_mu (same order as I/O). */
	flush_workqueue(dev->stripe_wq);
	http = aiosvd_client_get(dev);
	if (!http)
		return -ENODEV;
	mutex_lock(&dev->cache_mu);
	for (i = 0; i < AIOSVD_OCACHE_SIZE; i++) {
		int e = cache_writeback_ent(dev, http, &dev->cache[i]);

		if (e && !err)
			err = e;
	}
	mutex_unlock(&dev->cache_mu);
	aiosvd_client_put(dev, http);
	return err;
}

void aiosvd_cache_invalidate(struct aiosvd_device *dev)
{
	unsigned int i;

	mutex_lock(&dev->cache_mu);
	for (i = 0; i < AIOSVD_OCACHE_SIZE; i++) {
		dev->cache[i].valid = false;
		dev->cache[i].dirty = false;
		dev->cache[i].datalen = 0;
	}
	mutex_unlock(&dev->cache_mu);
}

void aiosvd_cache_destroy(struct aiosvd_device *dev)
{
	unsigned int i;

	mutex_lock(&dev->cache_mu);
	for (i = 0; i < AIOSVD_OCACHE_SIZE; i++) {
		kvfree(dev->cache[i].data);
		dev->cache[i].data = NULL;
		dev->cache[i].valid = false;
		dev->cache[i].dirty = false;
		dev->cache[i].datalen = 0;
	}
	mutex_unlock(&dev->cache_mu);
}

static int cache_ensure_buf(struct aiosvd_device *dev, struct aiosvd_ocache_ent *ent)
{
	if (ent->data)
		return 0;
	ent->data = kvmalloc(dev->obj_size, GFP_KERNEL);
	if (!ent->data)
		return -ENOMEM;
	return 0;
}

static int load_object_for_cow(struct aios_http_client *http, struct aiosvd_device *dev, u64 objno,
			       void *dst, u32 *datalen_out)
{
	char oid[192];
	struct aios_http_buf body = { 0 };
	int err;

	aiosvd_data_oid(dev->pool, dev->name, objno, oid, sizeof(oid));
	err = aios_http_get(http, oid, &body, NULL);
	if (!err) {
		memcpy(dst, body.data, min_t(size_t, body.len, dev->obj_size));
		if (body.len < dev->obj_size)
			memset((char *)dst + body.len, 0, dev->obj_size - body.len);
		*datalen_out = min_t(u32, (u32)body.len, dev->obj_size);
		aios_http_buf_free(&body);
		return 0;
	}
	if (err != -ENOENT)
		return err;

	/* Child missing: COW from parent if present. */
	if (has_parent(dev)) {
		aiosvd_data_oid(dev->parent_pool, dev->parent_name, objno, oid, sizeof(oid));
		err = aios_http_get(http, oid, &body, NULL);
		if (!err) {
			memcpy(dst, body.data, min_t(size_t, body.len, dev->obj_size));
			if (body.len < dev->obj_size)
				memset((char *)dst + body.len, 0, dev->obj_size - body.len);
			*datalen_out = min_t(u32, (u32)body.len, dev->obj_size);
			aios_http_buf_free(&body);
			return 0;
		}
		if (err != -ENOENT)
			return err;
	}
	memset(dst, 0, dev->obj_size);
	*datalen_out = 0;
	return 0;
}

static int read_object_slice(struct aios_http_client *http, struct aiosvd_device *dev, u64 objno,
			     u32 obj_off, void *dst, u32 len)
{
	char oid[192];
	struct aios_http_buf body = { 0 };
	struct aiosvd_ocache_ent *ent;
	int err;

	if (!len)
		return 0;

	mutex_lock(&dev->cache_mu);
	ent = cache_lookup(dev, objno);
	if (ent) {
		atomic64_inc(&dev->cache_hits);
		memcpy(dst, (char *)ent->data + obj_off, len);
		mutex_unlock(&dev->cache_mu);
		return 0;
	}
	mutex_unlock(&dev->cache_mu);

	aiosvd_data_oid(dev->pool, dev->name, objno, oid, sizeof(oid));
	err = aios_http_get_range(http, oid, obj_off, obj_off + len - 1, &body);
	if (err == -ENOENT && has_parent(dev)) {
		aiosvd_data_oid(dev->parent_pool, dev->parent_name, objno, oid, sizeof(oid));
		err = aios_http_get_range(http, oid, obj_off, obj_off + len - 1, &body);
	}
	if (err == -ENOENT) {
		memset(dst, 0, len);
		return 0;
	}
	if (err)
		return err;
	if (body.len < len)
		memset((char *)dst + body.len, 0, len - body.len);
	memcpy(dst, body.data, min_t(size_t, body.len, len));
	aios_http_buf_free(&body);
	return 0;
}

static int write_object_slice(struct aios_http_client *http, struct aiosvd_device *dev, u64 objno,
			      u32 obj_off, const void *src, u32 len)
{
	char oid[192];
	struct aiosvd_ocache_ent *ent;
	int err;

	if (!len)
		return 0;
	aiosvd_data_oid(dev->pool, dev->name, objno, oid, sizeof(oid));

	/* Full-object write: bypass cache PUT, invalidate cache entry. */
	if (obj_off == 0 && len == dev->obj_size) {
		mutex_lock(&dev->cache_mu);
		ent = cache_lookup(dev, objno);
		if (ent) {
			ent->valid = false;
			ent->dirty = false;
		}
		mutex_unlock(&dev->cache_mu);
		if (buffer_is_zero(src, len)) {
			err = aios_http_delete(http, oid);
			return (err == -ENOENT) ? 0 : err;
		}
		return aios_http_put(http, oid, src, len, NULL, NULL);
	}

	mutex_lock(&dev->cache_mu);
	ent = cache_lookup(dev, objno);
	if (ent) {
		atomic64_inc(&dev->cache_hits);
		memcpy((char *)ent->data + obj_off, src, len);
		if (obj_off + len > ent->datalen)
			ent->datalen = obj_off + len;
		ent->dirty = true;
		mutex_unlock(&dev->cache_mu);
		return 0;
	}

	/* Cache miss: prefer range PUT unless parent COW requires a full object. */
	if (!has_parent(dev)) {
		mutex_unlock(&dev->cache_mu);
		return aios_http_put_range(http, oid, obj_off, src, len, NULL);
	}

	/* COW path: load child-or-parent into a cache slot, then dirty. */
	ent = cache_victim(dev);
	err = cache_writeback_ent(dev, http, ent);
	if (err) {
		mutex_unlock(&dev->cache_mu);
		return err;
	}
	err = cache_ensure_buf(dev, ent);
	if (err) {
		mutex_unlock(&dev->cache_mu);
		return err;
	}
	err = load_object_for_cow(http, dev, objno, ent->data, &ent->datalen);
	if (err) {
		mutex_unlock(&dev->cache_mu);
		return err;
	}
	ent->objno = objno;
	ent->valid = true;
	memcpy((char *)ent->data + obj_off, src, len);
	if (obj_off + len > ent->datalen)
		ent->datalen = obj_off + len;
	ent->dirty = true;
	mutex_unlock(&dev->cache_mu);
	return 0;
}

static int discard_object_slice(struct aios_http_client *http, struct aiosvd_device *dev, u64 objno,
				u32 obj_off, u32 len)
{
	char oid[192];
	struct aiosvd_ocache_ent *ent;
	char *obj;
	u32 put_len;
	int err;

	aiosvd_data_oid(dev->pool, dev->name, objno, oid, sizeof(oid));

	mutex_lock(&dev->cache_mu);
	ent = cache_lookup(dev, objno);
	if (ent) {
		if (obj_off == 0 && len == dev->obj_size) {
			ent->valid = false;
			ent->dirty = false;
			ent->datalen = 0;
			mutex_unlock(&dev->cache_mu);
			err = aios_http_delete(http, oid);
			return (err == -ENOENT) ? 0 : err;
		}
		memset((char *)ent->data + obj_off, 0, len);
		ent->dirty = true;
		mutex_unlock(&dev->cache_mu);
		return 0;
	}
	mutex_unlock(&dev->cache_mu);

	if (obj_off == 0 && len == dev->obj_size) {
		err = aios_http_delete(http, oid);
		return (err == -ENOENT) ? 0 : err;
	}

	/* Partial discard without cache: zero via range PUT when no parent. */
	if (!has_parent(dev)) {
		void *z = kzalloc(len, GFP_KERNEL);

		if (!z)
			return -ENOMEM;
		err = aios_http_put_range(http, oid, obj_off, z, len, NULL);
		kfree(z);
		if (err == -ENOENT)
			return 0;
		return err;
	}

	/* Parent COW: materialize then zero. */
	obj = kvmalloc(dev->obj_size, GFP_KERNEL);
	if (!obj)
		return -ENOMEM;
	err = load_object_for_cow(http, dev, objno, obj, &put_len);
	if (err) {
		kvfree(obj);
		return err;
	}
	memset(obj + obj_off, 0, len);
	if (put_len < obj_off + len)
		put_len = obj_off + len;
	if (buffer_is_zero(obj, put_len)) {
		err = aios_http_delete(http, oid);
		if (err == -ENOENT)
			err = 0;
	} else {
		err = aios_http_put(http, oid, obj, put_len, NULL, NULL);
	}
	kvfree(obj);
	return err;
}

enum aiosvd_stripe_op {
	AIOSVD_OP_READ = 1,
	AIOSVD_OP_WRITE,
	AIOSVD_OP_DISCARD,
};

struct aiosvd_stripe_job {
	struct work_struct work;
	struct aiosvd_device *dev;
	struct request *rq;
	u64 objno;
	u32 obj_off;
	u32 len;
	size_t rq_off;
	enum aiosvd_stripe_op op;
	atomic_t *err_atom;
	atomic_t *pending;
	struct completion *done;
};

static void aiosvd_stripe_workfn(struct work_struct *work)
{
	struct aiosvd_stripe_job *job = container_of(work, struct aiosvd_stripe_job, work);
	struct aios_http_client *http;
	void *buf = NULL;
	unsigned int noio;
	int err = 0;

	/* Serving block I/O: the kvmalloc below and every allocation inside
	 * aios_http must not recurse into reclaim, which could queue writeback to
	 * this device behind the job currently blocked in the allocator. */
	noio = memalloc_noio_save();
	http = aiosvd_client_get(job->dev);
	if (!http) {
		err = -ENODEV;
		goto out;
	}

	if (job->op == AIOSVD_OP_DISCARD) {
		err = discard_object_slice(http, job->dev, job->objno, job->obj_off, job->len);
	} else {
		buf = kvmalloc(job->len, GFP_KERNEL);
		if (!buf) {
			err = -ENOMEM;
			goto put;
		}
		if (job->op == AIOSVD_OP_WRITE) {
			err = rq_copy_linear(job->rq, job->rq_off, buf, job->len, false);
			if (!err)
				err = write_object_slice(http, job->dev, job->objno, job->obj_off,
							 buf, job->len);
		} else {
			err = read_object_slice(http, job->dev, job->objno, job->obj_off, buf,
						job->len);
			if (!err)
				err = rq_copy_linear(job->rq, job->rq_off, buf, job->len, true);
		}
		kvfree(buf);
	}
put:
	aiosvd_client_put(job->dev, http);
out:
	memalloc_noio_restore(noio);
	if (err)
		atomic_cmpxchg(job->err_atom, 0, err);
	if (atomic_dec_and_test(job->pending))
		complete(job->done);
}

static int aiosvd_run_stripes(struct aiosvd_device *dev, struct request *rq, u64 offset,
			      unsigned int nbytes, enum aiosvd_stripe_op op)
{
	struct aiosvd_stripe_job *jobs;
	struct completion done;
	atomic_t pending;
	atomic_t err_atom;
	u64 pos, end;
	unsigned int njobs = 0, i;
	size_t rq_off = 0;
	int err;

	if (!nbytes)
		return 0;

	pos = offset;
	end = offset + nbytes;
	while (pos < end) {
		u32 obj_off = (u32)(pos & (dev->obj_size - 1));
		u32 chunk = min_t(u64, end - pos, (u64)(dev->obj_size - obj_off));

		njobs++;
		pos += chunk;
	}
	if (!njobs)
		return 0;

	jobs = kcalloc(njobs, sizeof(*jobs), GFP_KERNEL);
	if (!jobs)
		return -ENOMEM;

	init_completion(&done);
	atomic_set(&pending, njobs);
	atomic_set(&err_atom, 0);

	pos = offset;
	rq_off = 0;
	i = 0;
	while (pos < end) {
		u64 objno = pos >> dev->obj_order;
		u32 obj_off = (u32)(pos & (dev->obj_size - 1));
		u32 chunk = min_t(u64, end - pos, (u64)(dev->obj_size - obj_off));

		jobs[i].dev = dev;
		jobs[i].rq = rq;
		jobs[i].objno = objno;
		jobs[i].obj_off = obj_off;
		jobs[i].len = chunk;
		jobs[i].rq_off = rq_off;
		jobs[i].op = op;
		jobs[i].err_atom = &err_atom;
		jobs[i].pending = &pending;
		jobs[i].done = &done;
		INIT_WORK(&jobs[i].work, aiosvd_stripe_workfn);
		queue_work(dev->stripe_wq, &jobs[i].work);

		pos += chunk;
		rq_off += chunk;
		i++;
	}

	wait_for_completion(&done);
	err = atomic_read(&err_atom);
	kfree(jobs);
	return err;
}

static int aiosvd_do_flush(struct aiosvd_device *dev)
{
	return aiosvd_cache_flush(dev);
}

int aiosvd_dev_io(struct aiosvd_device *dev, struct request *rq)
{
	unsigned int op = req_op(rq);
	sector_t sector = blk_rq_pos(rq);
	u64 offset = (u64)sector << SECTOR_SHIFT;
	unsigned int nbytes = blk_rq_bytes(rq);
	int err = 0;

	switch (op) {
	case REQ_OP_FLUSH:
		err = aiosvd_do_flush(dev);
		break;
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		if (dev->readonly) {
			err = -EROFS;
			break;
		}
		if (offset + nbytes > dev->size) {
			err = -EIO;
			break;
		}
		err = aiosvd_run_stripes(dev, rq, offset, nbytes, AIOSVD_OP_DISCARD);
		if (!err) {
			atomic64_inc(&dev->ops_discard);
			atomic64_add(nbytes, &dev->bytes_written);
		}
		break;
	case REQ_OP_READ:
		if (offset + nbytes > dev->size) {
			err = -EIO;
			break;
		}
		err = aiosvd_run_stripes(dev, rq, offset, nbytes, AIOSVD_OP_READ);
		if (!err) {
			atomic64_inc(&dev->ops_read);
			atomic64_add(nbytes, &dev->bytes_read);
		}
		break;
	case REQ_OP_WRITE:
		if (dev->readonly) {
			err = -EROFS;
			break;
		}
		if (offset + nbytes > dev->size) {
			err = -EIO;
			break;
		}
		err = aiosvd_run_stripes(dev, rq, offset, nbytes, AIOSVD_OP_WRITE);
		if (!err) {
			atomic64_inc(&dev->ops_write);
			atomic64_add(nbytes, &dev->bytes_written);
			if (rq->cmd_flags & REQ_FUA)
				err = aiosvd_do_flush(dev);
		}
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	if (err)
		atomic64_inc(&dev->errors);
	return err;
}
