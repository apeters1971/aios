// SPDX-License-Identifier: GPL-2.0
/*
 * Char-device upcall bridge: kernel VFS threads enqueue requests; userspace
 * aios-kbridge reads them from /dev/aios_bridge and writes replies.
 */
#include "aiosfs.h"

#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

struct aios_request {
	struct list_head list;
	struct aios_kabi_req_hdr hdr;
	void *payload;
	wait_queue_head_t wait;
	bool done;
	int result;
	void *rep_payload;
	u32 rep_len;
};

struct aios_conn {
	struct mutex lock;
	struct list_head pending; /* awaiting daemon read */
	struct list_head waiting; /* awaiting daemon reply */
	wait_queue_head_t read_wait;
	atomic_t unique;
	refcount_t refcnt;
	bool daemon_open;
};

static struct aios_conn *g_conn;
static DEFINE_MUTEX(g_conn_lock);

struct aios_conn *aios_conn_get(void)
{
	mutex_lock(&g_conn_lock);
	if (!g_conn) {
		g_conn = kzalloc(sizeof(*g_conn), GFP_KERNEL);
		if (!g_conn) {
			mutex_unlock(&g_conn_lock);
			return NULL;
		}
		mutex_init(&g_conn->lock);
		INIT_LIST_HEAD(&g_conn->pending);
		INIT_LIST_HEAD(&g_conn->waiting);
		init_waitqueue_head(&g_conn->read_wait);
		atomic_set(&g_conn->unique, 1);
		refcount_set(&g_conn->refcnt, 1);
	} else {
		refcount_inc(&g_conn->refcnt);
	}
	mutex_unlock(&g_conn_lock);
	return g_conn;
}

void aios_conn_put(struct aios_conn *conn)
{
	if (!conn)
		return;
	if (!refcount_dec_and_test(&conn->refcnt))
		return;
	mutex_lock(&g_conn_lock);
	if (g_conn == conn)
		g_conn = NULL;
	mutex_unlock(&g_conn_lock);
	kfree(conn);
}

bool aios_conn_daemon_present(struct aios_conn *conn)
{
	bool present;

	if (!conn)
		return false;
	mutex_lock(&conn->lock);
	present = conn->daemon_open;
	mutex_unlock(&conn->lock);
	return present;
}

int aios_upcall(struct aios_conn *conn, u32 opcode, int mount_id,
		const void *payload_in, u32 in_len,
		void **payload_out, u32 *out_len)
{
	struct aios_request *req;
	int err;

	if (!conn)
		return -ENOTCONN;
	if (in_len > AIOS_KABI_MAX_PAYLOAD)
		return -E2BIG;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;
	init_waitqueue_head(&req->wait);
	req->hdr.magic = AIOS_KABI_MAGIC;
	req->hdr.version = AIOS_KABI_VERSION;
	req->hdr.unique = (u64)atomic_inc_return(&conn->unique);
	req->hdr.opcode = opcode;
	req->hdr.payload_len = in_len;
	req->hdr.mount_id = mount_id;

	if (in_len) {
		req->payload = kmemdup(payload_in, in_len, GFP_KERNEL);
		if (!req->payload) {
			kfree(req);
			return -ENOMEM;
		}
	}

	mutex_lock(&conn->lock);
	if (!conn->daemon_open) {
		mutex_unlock(&conn->lock);
		kfree(req->payload);
		kfree(req);
		return -ENOTCONN;
	}
	list_add_tail(&req->list, &conn->pending);
	mutex_unlock(&conn->lock);
	wake_up_interruptible(&conn->read_wait);

	err = wait_event_interruptible(req->wait, req->done);
	if (err) {
		mutex_lock(&conn->lock);
		if (!req->done) {
			list_del_init(&req->list);
			mutex_unlock(&conn->lock);
			kfree(req->payload);
			kfree(req->rep_payload);
			kfree(req);
			return -EINTR;
		}
		mutex_unlock(&conn->lock);
	}

	err = req->result;
	if (!err && payload_out && out_len) {
		*payload_out = req->rep_payload;
		*out_len = req->rep_len;
		req->rep_payload = NULL;
	} else {
		kfree(req->rep_payload);
		if (payload_out)
			*payload_out = NULL;
		if (out_len)
			*out_len = 0;
	}

	kfree(req->payload);
	kfree(req);
	return err;
}

static ssize_t aios_dev_read(struct file *file, char __user *buf, size_t count,
			     loff_t *ppos)
{
	struct aios_conn *conn = file->private_data;
	struct aios_request *req = NULL;
	size_t need;
	int err;

	if (!conn)
		return -ENOTCONN;

	err = wait_event_interruptible(conn->read_wait, ({
		bool ready = false;
		mutex_lock(&conn->lock);
		if (!list_empty(&conn->pending)) {
			req = list_first_entry(&conn->pending, struct aios_request, list);
			list_del_init(&req->list);
			list_add_tail(&req->list, &conn->waiting);
			ready = true;
		}
		mutex_unlock(&conn->lock);
		ready || !conn->daemon_open;
	}));
	if (err)
		return err;
	if (!req)
		return -ENODEV;

	need = sizeof(req->hdr) + req->hdr.payload_len;
	if (count < need) {
		/* Put back at head for retry with larger buffer. */
		mutex_lock(&conn->lock);
		list_del_init(&req->list);
		list_add(&req->list, &conn->pending);
		mutex_unlock(&conn->lock);
		wake_up_interruptible(&conn->read_wait);
		return -EMSGSIZE;
	}
	if (copy_to_user(buf, &req->hdr, sizeof(req->hdr)))
		goto fault;
	if (req->hdr.payload_len &&
	    copy_to_user(buf + sizeof(req->hdr), req->payload, req->hdr.payload_len))
		goto fault;
	return (ssize_t)need;

fault:
	mutex_lock(&conn->lock);
	list_del_init(&req->list);
	list_add(&req->list, &conn->pending);
	mutex_unlock(&conn->lock);
	return -EFAULT;
}

static struct aios_request *find_waiting(struct aios_conn *conn, u64 unique)
{
	struct aios_request *req;

	list_for_each_entry(req, &conn->waiting, list) {
		if (req->hdr.unique == unique)
			return req;
	}
	return NULL;
}

static ssize_t aios_dev_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct aios_conn *conn = file->private_data;
	struct aios_kabi_rep_hdr hdr;
	struct aios_request *req;
	void *payload = NULL;

	if (!conn || count < sizeof(hdr))
		return -EINVAL;
	if (copy_from_user(&hdr, buf, sizeof(hdr)))
		return -EFAULT;
	if (hdr.magic != AIOS_KABI_MAGIC || hdr.version != AIOS_KABI_VERSION)
		return -EINVAL;
	if (hdr.payload_len > AIOS_KABI_MAX_PAYLOAD ||
	    count < sizeof(hdr) + hdr.payload_len)
		return -EINVAL;

	if (hdr.payload_len) {
		payload = kmalloc(hdr.payload_len, GFP_KERNEL);
		if (!payload)
			return -ENOMEM;
		if (copy_from_user(payload, buf + sizeof(hdr), hdr.payload_len)) {
			kfree(payload);
			return -EFAULT;
		}
	}

	mutex_lock(&conn->lock);
	req = find_waiting(conn, hdr.unique);
	if (!req) {
		mutex_unlock(&conn->lock);
		kfree(payload);
		return -ENOENT;
	}
	list_del_init(&req->list);
	req->result = hdr.result;
	req->rep_payload = payload;
	req->rep_len = hdr.payload_len;
	req->done = true;
	mutex_unlock(&conn->lock);
	wake_up_interruptible(&req->wait);
	return (ssize_t)count;
}

static __poll_t aios_dev_poll(struct file *file, poll_table *wait)
{
	struct aios_conn *conn = file->private_data;
	__poll_t mask = 0;

	if (!conn)
		return EPOLLERR;
	poll_wait(file, &conn->read_wait, wait);
	mutex_lock(&conn->lock);
	if (!list_empty(&conn->pending))
		mask |= EPOLLIN | EPOLLRDNORM;
	mask |= EPOLLOUT | EPOLLWRNORM;
	mutex_unlock(&conn->lock);
	return mask;
}

static int aios_dev_open(struct inode *inode, struct file *file)
{
	struct aios_conn *conn = aios_conn_get();

	if (!conn)
		return -ENOMEM;
	mutex_lock(&conn->lock);
	if (conn->daemon_open) {
		mutex_unlock(&conn->lock);
		aios_conn_put(conn);
		return -EBUSY;
	}
	conn->daemon_open = true;
	mutex_unlock(&conn->lock);
	file->private_data = conn;
	return 0;
}

static int aios_dev_release(struct inode *inode, struct file *file)
{
	struct aios_conn *conn = file->private_data;
	struct aios_request *req, *tmp;

	if (!conn)
		return 0;

	mutex_lock(&conn->lock);
	conn->daemon_open = false;
	list_for_each_entry_safe(req, tmp, &conn->pending, list) {
		list_del_init(&req->list);
		req->result = -ENOTCONN;
		req->done = true;
		wake_up_interruptible(&req->wait);
	}
	list_for_each_entry_safe(req, tmp, &conn->waiting, list) {
		list_del_init(&req->list);
		req->result = -ENOTCONN;
		req->done = true;
		wake_up_interruptible(&req->wait);
	}
	mutex_unlock(&conn->lock);
	wake_up_interruptible(&conn->read_wait);
	aios_conn_put(conn);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations aios_dev_fops = {
	.owner = THIS_MODULE,
	.open = aios_dev_open,
	.release = aios_dev_release,
	.read = aios_dev_read,
	.write = aios_dev_write,
	.poll = aios_dev_poll,
	.llseek = no_llseek,
};

static struct miscdevice aios_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = AIOS_KABI_DEV_NAME,
	.fops = &aios_dev_fops,
	.mode = 0600,
};

int aios_upcall_init(void)
{
	return misc_register(&aios_misc);
}

void aios_upcall_exit(void)
{
	misc_deregister(&aios_misc);
	mutex_lock(&g_conn_lock);
	if (g_conn && refcount_read(&g_conn->refcnt) == 1) {
		kfree(g_conn);
		g_conn = NULL;
	}
	mutex_unlock(&g_conn_lock);
}
