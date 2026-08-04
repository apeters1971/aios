// SPDX-License-Identifier: GPL-2.0
#include "aiosvd.h"

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

static bool valid_name(const char *s)
{
	size_t i, n;

	if (!s || !*s)
		return false;
	n = strnlen(s, AIOSVD_NAME_MAX + 1);
	if (n == 0 || n > AIOSVD_NAME_MAX)
		return false;
	for (i = 0; i < n; i++) {
		char c = s[i];

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
			continue;
		return false;
	}
	return true;
}

static u32 default_nclients(u32 requested)
{
	u32 n = requested ? requested : num_online_cpus();

	if (n < 2 && !requested)
		n = 2;
	if (n < 1)
		n = 1;
	if (n > AIOSVD_MAX_CLIENTS)
		n = AIOSVD_MAX_CLIENTS;
	return n;
}

static void init_dev_stats(struct aiosvd_device *dev)
{
	atomic64_set(&dev->bytes_read, 0);
	atomic64_set(&dev->bytes_written, 0);
	atomic64_set(&dev->ops_read, 0);
	atomic64_set(&dev->ops_write, 0);
	atomic64_set(&dev->ops_discard, 0);
	atomic64_set(&dev->errors, 0);
	atomic64_set(&dev->cache_hits, 0);
	mutex_init(&dev->cache_mu);
	mutex_init(&dev->open_mu);
}

static int slot_claim(const char *pool, const char *name, struct aiosvd_device **dev_out)
{
	struct aiosvd_device *dev = NULL;
	int id = -1;
	int i;

	for (i = 0; i < AIOSVD_MAX_DEVS; i++) {
		if (aiosvd_devs[i].mapped && !strcmp(aiosvd_devs[i].pool, pool) &&
		    !strcmp(aiosvd_devs[i].name, name))
			return -EEXIST;
	}
	for (i = 0; i < AIOSVD_MAX_DEVS; i++) {
		if (!aiosvd_devs[i].mapped) {
			id = i;
			dev = &aiosvd_devs[i];
			break;
		}
	}
	if (!dev)
		return -ENOSPC;
	memset(dev, 0, sizeof(*dev));
	dev->id = id;
	dev->mapped = true;
	*dev_out = dev;
	return 0;
}

int aiosvd_map(struct aiosvd_map_arg *arg)
{
	struct aiosvd_device *dev = NULL;
	char header_oid[192];
	char hdr_js[768];
	char parent_pool[AIOSVD_POOL_MAX + 1] = "";
	char parent_name[AIOSVD_NAME_MAX + 1] = "";
	char key_id[AIOSVD_KEY_ID_MAX + 1] = "";
	struct aios_http_buf body = { 0 };
	struct aios_http_client *probe;
	u64 size;
	u64 header_cas = 0;
	u32 order;
	u32 nclients;
	int err;

	if (!arg->endpoint[0] || !arg->cluster_key[0])
		return -EINVAL;
	if (!valid_name(arg->pool) || !valid_name(arg->name))
		return -EINVAL;

	order = arg->obj_order ? arg->obj_order : AIOSVD_DEFAULT_OBJ_ORDER;
	if (order < AIOSVD_MIN_OBJ_ORDER || order > AIOSVD_MAX_OBJ_ORDER)
		return -EINVAL;
	if (arg->size && (arg->size & 4095))
		return -EINVAL;

	if (arg->key_id[0])
		strscpy(key_id, arg->key_id, sizeof(key_id));

	probe = aios_http_client_create(arg->endpoint, arg->cluster_key, GFP_KERNEL);
	if (IS_ERR(probe))
		return PTR_ERR(probe);
	aios_http_client_set_timeout_ms(probe, 30000);
	/* Default app_label "vbd" so object OPS are separated from S3/FS in admin monitoring. */
	if (!arg->app_label[0])
		strscpy(arg->app_label, "vbd", sizeof(arg->app_label));
	aios_http_client_set_app_label(probe, arg->app_label);

	aiosvd_header_oid(arg->pool, arg->name, header_oid, sizeof(header_oid));

	if (arg->flags & AIOSVD_MAP_CREATE) {
		if (!arg->size) {
			aios_http_client_destroy(probe);
			return -EINVAL;
		}
		err = aiosvd_header_format(hdr_js, sizeof(hdr_js), arg->pool, arg->name, arg->size,
					   order, NULL, NULL, key_id);
		if (err) {
			aios_http_client_destroy(probe);
			return err;
		}
		header_cas = 0;
		err = aios_http_put(probe, header_oid, hdr_js, strlen(hdr_js), NULL, &header_cas);
		if (err == -EAGAIN) {
			if (arg->flags & AIOSVD_MAP_EXCL) {
				aios_http_client_destroy(probe);
				return -EEXIST;
			}
			err = aios_http_get(probe, header_oid, &body, &header_cas);
			if (err) {
				aios_http_client_destroy(probe);
				return err;
			}
			err = aiosvd_header_parse(body.data, body.len, &size, &order, parent_pool,
						  sizeof(parent_pool), parent_name,
						  sizeof(parent_name), key_id, sizeof(key_id));
			aios_http_buf_free(&body);
			if (err) {
				aios_http_client_destroy(probe);
				return err;
			}
		} else if (err) {
			aios_http_client_destroy(probe);
			return err;
		} else {
			size = arg->size;
		}
	} else {
		err = aios_http_get(probe, header_oid, &body, &header_cas);
		if (err) {
			aios_http_client_destroy(probe);
			return err;
		}
		err = aiosvd_header_parse(body.data, body.len, &size, &order, parent_pool,
					  sizeof(parent_pool), parent_name, sizeof(parent_name),
					  key_id, sizeof(key_id));
		aios_http_buf_free(&body);
		if (err) {
			aios_http_client_destroy(probe);
			return err;
		}
		/* Map-arg key_id overrides header if provided. */
		if (arg->key_id[0])
			strscpy(key_id, arg->key_id, sizeof(key_id));
	}
	aios_http_client_destroy(probe);

	if (key_id[0])
		pr_info("aiosvd: map %s/%s key_id=%s (hook only; no in-kernel crypto)\n",
			arg->pool, arg->name, key_id);

	mutex_lock(&aiosvd_devs_mu);
	err = slot_claim(arg->pool, arg->name, &dev);
	if (err) {
		mutex_unlock(&aiosvd_devs_mu);
		return err;
	}

	dev->size = size;
	dev->obj_order = order;
	dev->obj_size = 1u << order;
	dev->header_cas = header_cas;
	dev->flags = arg->flags;
	dev->readonly = !!(arg->flags & AIOSVD_MAP_READONLY);
	dev->queue_depth = arg->queue_depth;
	strscpy(dev->pool, arg->pool, sizeof(dev->pool));
	strscpy(dev->name, arg->name, sizeof(dev->name));
	strscpy(dev->parent_pool, parent_pool, sizeof(dev->parent_pool));
	strscpy(dev->parent_name, parent_name, sizeof(dev->parent_name));
	strscpy(dev->key_id, key_id, sizeof(dev->key_id));
	strscpy(dev->endpoint, arg->endpoint, sizeof(dev->endpoint));
	strscpy(dev->cluster_key, arg->cluster_key, sizeof(dev->cluster_key));
	strscpy(dev->app_label, arg->app_label, sizeof(dev->app_label));
	init_dev_stats(dev);
	mutex_unlock(&aiosvd_devs_mu);

	nclients = default_nclients(arg->max_clients);
	err = aiosvd_clients_create(dev, arg->endpoint, arg->cluster_key, arg->app_label,
				    nclients);
	if (err)
		goto fail_slot;

	err = aiosvd_create_disk(dev);
	if (err) {
		aiosvd_clients_destroy(dev);
		goto fail_slot;
	}

	arg->dev_id = dev->id;
	arg->size = size;
	arg->obj_order = order;
	pr_info("aiosvd: mapped %s/%s as /dev/%s%d (%llu bytes, obj_order=%u, clients=%u, qd=%u)%s\n",
		arg->pool, arg->name, AIOSVD_DISK_PREFIX, dev->id, (unsigned long long)size, order,
		aios_http_pool_size(dev->http_pool), dev->queue_depth,
		dev->readonly ? " [ro]" : "");
	return 0;

fail_slot:
	mutex_lock(&aiosvd_devs_mu);
	dev->mapped = false;
	mutex_unlock(&aiosvd_devs_mu);
	return err;
}

int aiosvd_unmap(int dev_id)
{
	struct aiosvd_device *dev;

	if (dev_id < 0 || dev_id >= AIOSVD_MAX_DEVS)
		return -EINVAL;

	mutex_lock(&aiosvd_devs_mu);
	dev = &aiosvd_devs[dev_id];
	if (!dev->mapped) {
		mutex_unlock(&aiosvd_devs_mu);
		return -ENOENT;
	}
	mutex_lock(&dev->open_mu);
	if (dev->open_count) {
		mutex_unlock(&dev->open_mu);
		mutex_unlock(&aiosvd_devs_mu);
		return -EBUSY;
	}
	mutex_unlock(&dev->open_mu);
	dev->mapped = false;
	mutex_unlock(&aiosvd_devs_mu);

	aiosvd_cache_flush(dev);
	aiosvd_teardown_disk(dev);
	aiosvd_cache_destroy(dev);
	aiosvd_clients_destroy(dev);
	memzero_explicit(dev->cluster_key, sizeof(dev->cluster_key));
	pr_info("aiosvd: unmapped /dev/%s%d\n", AIOSVD_DISK_PREFIX, dev_id);
	return 0;
}

int aiosvd_info(struct aiosvd_info_arg *arg)
{
	struct aiosvd_device *dev;

	if (arg->dev_id < 0 || arg->dev_id >= AIOSVD_MAX_DEVS)
		return -EINVAL;

	mutex_lock(&aiosvd_devs_mu);
	dev = &aiosvd_devs[arg->dev_id];
	if (!dev->mapped) {
		mutex_unlock(&aiosvd_devs_mu);
		return -ENOENT;
	}
	aiosvd_fill_info(dev, arg);
	mutex_unlock(&aiosvd_devs_mu);
	return 0;
}

int aiosvd_list(struct aiosvd_list_arg *arg)
{
	unsigned int i, n = 0;

	mutex_lock(&aiosvd_devs_mu);
	for (i = 0; i < AIOSVD_MAX_DEVS; i++) {
		if (!aiosvd_devs[i].mapped)
			continue;
		aiosvd_fill_info(&aiosvd_devs[i], &arg->entries[n]);
		n++;
	}
	arg->count = n;
	mutex_unlock(&aiosvd_devs_mu);
	return 0;
}

int aiosvd_resize(struct aiosvd_resize_arg *arg)
{
	struct aiosvd_device *dev;
	struct aios_http_client *http;
	char header_oid[192];
	char hdr_js[768];
	u64 cas;
	u64 old_size, new_size = arg->new_size;
	u32 order;
	int err;

	if (arg->dev_id < 0 || arg->dev_id >= AIOSVD_MAX_DEVS)
		return -EINVAL;
	if (!new_size || (new_size & 4095))
		return -EINVAL;

	mutex_lock(&aiosvd_devs_mu);
	dev = &aiosvd_devs[arg->dev_id];
	if (!dev->mapped) {
		mutex_unlock(&aiosvd_devs_mu);
		return -ENOENT;
	}
	if (dev->readonly) {
		mutex_unlock(&aiosvd_devs_mu);
		return -EROFS;
	}
	old_size = dev->size;
	order = dev->obj_order;
	cas = dev->header_cas;
	aiosvd_header_oid(dev->pool, dev->name, header_oid, sizeof(header_oid));
	err = aiosvd_header_format(hdr_js, sizeof(hdr_js), dev->pool, dev->name, new_size, order,
				   dev->parent_pool, dev->parent_name, dev->key_id);
	if (err) {
		mutex_unlock(&aiosvd_devs_mu);
		return err;
	}
	mutex_unlock(&aiosvd_devs_mu);

	err = aiosvd_cache_flush(dev);
	if (err)
		return err;

	http = aiosvd_client_get(dev);
	if (!http)
		return -ENODEV;
	err = aios_http_put(http, header_oid, hdr_js, strlen(hdr_js), NULL, &cas);
	aiosvd_client_put(dev, http);
	if (err)
		return err;

	mutex_lock(&aiosvd_devs_mu);
	if (!dev->mapped) {
		mutex_unlock(&aiosvd_devs_mu);
		return -ENOENT;
	}
	dev->header_cas = cas;
	dev->size = new_size;
	if (dev->disk)
		set_capacity_revalidate_and_notify(dev->disk, new_size >> SECTOR_SHIFT, true);
	mutex_unlock(&aiosvd_devs_mu);

	if (new_size < old_size) {
		u64 first = (new_size + dev->obj_size - 1) >> dev->obj_order;
		u64 last = (old_size + dev->obj_size - 1) >> dev->obj_order;
		u64 objno;
		char oid[192];

		aiosvd_cache_invalidate(dev);
		http = aiosvd_client_get(dev);
		if (http) {
			for (objno = first; objno < last; objno++) {
				aiosvd_data_oid(dev->pool, dev->name, objno, oid, sizeof(oid));
				aios_http_delete(http, oid);
			}
			aiosvd_client_put(dev, http);
		}
	}

	arg->new_size = new_size;
	pr_info("aiosvd: resized /dev/%s%d to %llu bytes\n", AIOSVD_DISK_PREFIX, arg->dev_id,
		(unsigned long long)new_size);
	return 0;
}

int aiosvd_clone(struct aiosvd_clone_arg *arg)
{
	struct aiosvd_device *src, *dev = NULL;
	struct aiosvd_map_arg map;
	char header_oid[192];
	char hdr_js[768];
	struct aios_http_client *http;
	u64 header_cas = 0;
	u64 size;
	u32 order;
	int err;

	if (arg->src_dev_id < 0 || arg->src_dev_id >= AIOSVD_MAX_DEVS)
		return -EINVAL;
	if (!valid_name(arg->pool) || !valid_name(arg->name))
		return -EINVAL;

	{
		char src_pool[AIOSVD_POOL_MAX + 1];
		char src_name[AIOSVD_NAME_MAX + 1];

		mutex_lock(&aiosvd_devs_mu);
		src = &aiosvd_devs[arg->src_dev_id];
		if (!src->mapped) {
			mutex_unlock(&aiosvd_devs_mu);
			return -ENOENT;
		}
		size = src->size;
		order = src->obj_order;
		memset(&map, 0, sizeof(map));
		strscpy(map.endpoint, src->endpoint, sizeof(map.endpoint));
		strscpy(map.cluster_key, src->cluster_key, sizeof(map.cluster_key));
		strscpy(map.app_label, src->app_label, sizeof(map.app_label));
		strscpy(map.key_id, src->key_id, sizeof(map.key_id));
		map.queue_depth = src->queue_depth;
		map.max_clients = src->http_pool ? aios_http_pool_size(src->http_pool) : 0;
		strscpy(map.pool, arg->pool, sizeof(map.pool));
		strscpy(map.name, arg->name, sizeof(map.name));
		strscpy(src_pool, src->pool, sizeof(src_pool));
		strscpy(src_name, src->name, sizeof(src_name));
		mutex_unlock(&aiosvd_devs_mu);

		/* Flush parent so child sees consistent base. */
		err = aiosvd_cache_flush(src);
		if (err)
			return err;

		http = aios_http_client_create(map.endpoint, map.cluster_key, GFP_KERNEL);
		if (IS_ERR(http))
			return PTR_ERR(http);
		aios_http_client_set_timeout_ms(http, 30000);
		if (!map.app_label[0])
			strscpy(map.app_label, "vbd", sizeof(map.app_label));
		aios_http_client_set_app_label(http, map.app_label);

		aiosvd_header_oid(arg->pool, arg->name, header_oid, sizeof(header_oid));
		err = aiosvd_header_format(hdr_js, sizeof(hdr_js), arg->pool, arg->name, size,
					   order, src_pool, src_name, map.key_id);
		if (err) {
			aios_http_client_destroy(http);
			return err;
		}
		header_cas = 0;
		err = aios_http_put(http, header_oid, hdr_js, strlen(hdr_js), NULL, &header_cas);
		aios_http_client_destroy(http);
		if (err)
			return err == -EAGAIN ? -EEXIST : err;

		mutex_lock(&aiosvd_devs_mu);
		err = slot_claim(arg->pool, arg->name, &dev);
		if (err) {
			mutex_unlock(&aiosvd_devs_mu);
			return err;
		}
		dev->size = size;
		dev->obj_order = order;
		dev->obj_size = 1u << order;
		dev->header_cas = header_cas;
		dev->flags = 0;
		dev->readonly = false;
		dev->queue_depth = map.queue_depth;
		strscpy(dev->pool, arg->pool, sizeof(dev->pool));
		strscpy(dev->name, arg->name, sizeof(dev->name));
		strscpy(dev->parent_pool, src_pool, sizeof(dev->parent_pool));
		strscpy(dev->parent_name, src_name, sizeof(dev->parent_name));
		strscpy(dev->key_id, map.key_id, sizeof(dev->key_id));
		strscpy(dev->endpoint, map.endpoint, sizeof(dev->endpoint));
		strscpy(dev->cluster_key, map.cluster_key, sizeof(dev->cluster_key));
		strscpy(dev->app_label, map.app_label, sizeof(dev->app_label));
		init_dev_stats(dev);
		mutex_unlock(&aiosvd_devs_mu);
	}

	err = aiosvd_clients_create(dev, map.endpoint, map.cluster_key, map.app_label,
				    default_nclients(map.max_clients));
	if (err)
		goto fail_slot;

	err = aiosvd_create_disk(dev);
	if (err) {
		aiosvd_clients_destroy(dev);
		goto fail_slot;
	}

	arg->dest_dev_id = dev->id;
	pr_info("aiosvd: cloned → %s/%s as /dev/%s%d (COW parent=%s/%s)\n", arg->pool, arg->name,
		AIOSVD_DISK_PREFIX, dev->id, dev->parent_pool, dev->parent_name);
	return 0;

fail_slot:
	mutex_lock(&aiosvd_devs_mu);
	dev->mapped = false;
	mutex_unlock(&aiosvd_devs_mu);
	return err;
}

int aiosvd_rename(struct aiosvd_rename_arg *arg)
{
	struct aiosvd_device *dev;
	struct aios_http_client *http;
	char old_hdr[192], new_hdr[192];
	char old_oid[192], new_oid[192];
	char hdr_js[768];
	char old_pool[AIOSVD_POOL_MAX + 1];
	char old_name[AIOSVD_NAME_MAX + 1];
	u64 cas, size, nobj, objno;
	u32 order;
	int err, i;

	if (arg->dev_id < 0 || arg->dev_id >= AIOSVD_MAX_DEVS)
		return -EINVAL;
	if (!valid_name(arg->pool) || !valid_name(arg->name))
		return -EINVAL;

	mutex_lock(&aiosvd_devs_mu);
	dev = &aiosvd_devs[arg->dev_id];
	if (!dev->mapped) {
		mutex_unlock(&aiosvd_devs_mu);
		return -ENOENT;
	}
	if (dev->readonly) {
		mutex_unlock(&aiosvd_devs_mu);
		return -EROFS;
	}
	if (!strcmp(dev->pool, arg->pool) && !strcmp(dev->name, arg->name)) {
		mutex_unlock(&aiosvd_devs_mu);
		return 0;
	}
	for (i = 0; i < AIOSVD_MAX_DEVS; i++) {
		if (i == arg->dev_id || !aiosvd_devs[i].mapped)
			continue;
		if (!strcmp(aiosvd_devs[i].pool, arg->pool) &&
		    !strcmp(aiosvd_devs[i].name, arg->name)) {
			mutex_unlock(&aiosvd_devs_mu);
			return -EEXIST;
		}
	}
	strscpy(old_pool, dev->pool, sizeof(old_pool));
	strscpy(old_name, dev->name, sizeof(old_name));
	size = dev->size;
	order = dev->obj_order;
	cas = dev->header_cas;
	mutex_unlock(&aiosvd_devs_mu);

	err = aiosvd_cache_flush(dev);
	if (err)
		return err;

	aiosvd_header_oid(old_pool, old_name, old_hdr, sizeof(old_hdr));
	aiosvd_header_oid(arg->pool, arg->name, new_hdr, sizeof(new_hdr));
	err = aiosvd_header_format(hdr_js, sizeof(hdr_js), arg->pool, arg->name, size, order,
				   dev->parent_pool, dev->parent_name, dev->key_id);
	if (err)
		return err;

	http = aiosvd_client_get(dev);
	if (!http)
		return -ENODEV;

	/* Create new header (exclusive). */
	{
		u64 new_cas = 0;

		err = aios_http_put(http, new_hdr, hdr_js, strlen(hdr_js), NULL, &new_cas);
		if (err) {
			aiosvd_client_put(dev, http);
			return err == -EAGAIN ? -EEXIST : err;
		}
		cas = new_cas;
	}

	/* Migrate present data objects. */
	nobj = (size + dev->obj_size - 1) >> dev->obj_order;
	for (objno = 0; objno < nobj; objno++) {
		struct aios_http_buf body = { 0 };

		aiosvd_data_oid(old_pool, old_name, objno, old_oid, sizeof(old_oid));
		aiosvd_data_oid(arg->pool, arg->name, objno, new_oid, sizeof(new_oid));
		err = aios_http_get(http, old_oid, &body, NULL);
		if (err == -ENOENT) {
			err = 0;
			continue;
		}
		if (err)
			break;
		err = aios_http_put(http, new_oid, body.data, body.len, NULL, NULL);
		aios_http_buf_free(&body);
		if (err)
			break;
		aios_http_delete(http, old_oid);
	}
	if (!err)
		aios_http_delete(http, old_hdr);
	aiosvd_client_put(dev, http);
	if (err)
		return err;

	aiosvd_cache_invalidate(dev);

	mutex_lock(&aiosvd_devs_mu);
	if (!dev->mapped) {
		mutex_unlock(&aiosvd_devs_mu);
		return -ENOENT;
	}
	strscpy(dev->pool, arg->pool, sizeof(dev->pool));
	strscpy(dev->name, arg->name, sizeof(dev->name));
	dev->header_cas = cas;
	mutex_unlock(&aiosvd_devs_mu);

	pr_info("aiosvd: renamed /dev/%s%d %s/%s → %s/%s\n", AIOSVD_DISK_PREFIX, arg->dev_id,
		old_pool, old_name, arg->pool, arg->name);
	return 0;
}

static long aiosvd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int err;

	switch (cmd) {
	case AIOSVD_IOCTL_MAP: {
		struct aiosvd_map_arg m;

		if (copy_from_user(&m, (void __user *)arg, sizeof(m)))
			return -EFAULT;
		err = aiosvd_map(&m);
		if (err)
			return err;
		if (copy_to_user((void __user *)arg, &m, sizeof(m)))
			return -EFAULT;
		return 0;
	}
	case AIOSVD_IOCTL_UNMAP: {
		struct aiosvd_unmap_arg u;

		if (copy_from_user(&u, (void __user *)arg, sizeof(u)))
			return -EFAULT;
		return aiosvd_unmap(u.dev_id);
	}
	case AIOSVD_IOCTL_INFO: {
		struct aiosvd_info_arg info;

		if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
			return -EFAULT;
		err = aiosvd_info(&info);
		if (err)
			return err;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case AIOSVD_IOCTL_LIST: {
		struct aiosvd_list_arg *list;

		list = kzalloc(sizeof(*list), GFP_KERNEL);
		if (!list)
			return -ENOMEM;
		err = aiosvd_list(list);
		if (!err && copy_to_user((void __user *)arg, list, sizeof(*list)))
			err = -EFAULT;
		kfree(list);
		return err;
	}
	case AIOSVD_IOCTL_RESIZE: {
		struct aiosvd_resize_arg r;

		if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
			return -EFAULT;
		err = aiosvd_resize(&r);
		if (err)
			return err;
		if (copy_to_user((void __user *)arg, &r, sizeof(r)))
			return -EFAULT;
		return 0;
	}
	case AIOSVD_IOCTL_CLONE: {
		struct aiosvd_clone_arg c;

		if (copy_from_user(&c, (void __user *)arg, sizeof(c)))
			return -EFAULT;
		err = aiosvd_clone(&c);
		if (err)
			return err;
		if (copy_to_user((void __user *)arg, &c, sizeof(c)))
			return -EFAULT;
		return 0;
	}
	case AIOSVD_IOCTL_RENAME: {
		struct aiosvd_rename_arg r;

		if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
			return -EFAULT;
		err = aiosvd_rename(&r);
		if (err)
			return err;
		if (copy_to_user((void __user *)arg, &r, sizeof(r)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations aiosvd_ctl_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = aiosvd_ioctl,
	.compat_ioctl = aiosvd_ioctl,
	.llseek = no_llseek,
};

static struct miscdevice aiosvd_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = AIOSVD_CTL_NAME,
	.fops = &aiosvd_ctl_fops,
	.mode = 0600,
};

int aiosvd_ctl_init(void)
{
	return misc_register(&aiosvd_misc);
}

void aiosvd_ctl_exit(void)
{
	int i;

	for (i = 0; i < AIOSVD_MAX_DEVS; i++) {
		if (aiosvd_devs[i].mapped)
			aiosvd_unmap(i);
	}
	misc_deregister(&aiosvd_misc);
}
