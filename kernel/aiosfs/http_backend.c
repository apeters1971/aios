// SPDX-License-Identifier: GPL-2.0
/*
 * In-kernel POSIX path: aiosfs VFS → aios_http.ko → AIOS cluster.
 * Compatible object layout with userspace libaios_posix (compact dir rewrites).
 */
#include "aiosfs.h"

#include "../aios_http/aios_http_api.h"

#include <linux/completion.h>
#include <linux/cred.h>
#include <linux/delayed_call.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/backing-dev.h>
#include <linux/pagemap.h>
#include <linux/random.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/statfs.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <linux/workqueue.h>
#include <linux/writeback.h>

#define AIOS_HTTP_ROOT_INO 1
#define AIOS_HTTP_DEFAULT_STRIPE_UNIT (1024ull * 1024ull)
#define AIOS_HTTP_DEFAULT_STRIPE_WIDTH 4
#define AIOS_HTTP_MAX_DIR_ENTS 4096
#define AIOS_HTTP_AOPK_MAGIC 0x6b504f41u /* 'AOPk' LE */
#define AIOS_HTTP_OP_LINK 1
#define AIOS_HTTP_OP_UNLINK 2
#define AIOS_HTTP_OP_RENAME 3
#define AIOS_HTTP_MAX_XATTRS 128
#define AIOS_HTTP_MAX_XATTR_VALUE AIOS_KABI_XATTR_VALUE_MAX
#define AIOS_DIR_CACHE_SLOTS 16
#define AIOS_DIRTY_FLUSH_BYTES (4ull * 1024 * 1024)
#define AIOS_DIRTY_FLUSH_MS 100
/* Directory changelog is compacted into a snapshot past this size (matches
 * changelog::kAutoCompactBytes in libaios_posix). */
#define AIOS_HTTP_LOG_COMPACT_BYTES (1024ull * 1024ull)

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif

/* Contended directory ops back off exponentially up to this many attempts
 * (~5 s typical, ~7 s worst case: longer than a lease break grace period, so
 * a peer's lease is broken and reacquired within the budget) and then fail
 * with -EBUSY, never -EAGAIN. */
#define AIOS_HTTP_DIR_RETRIES 20
#define AIOS_HTTP_TXN_ID_LEN 128
#define AIOS_HTTP_META_RETRIES 8

static char *extract_xattrs_object(const char *js);
static void http_clear_size_dirty(struct inode *inode, u64 size);

static struct aios_http_client *http_client_get(struct aios_sb_info *info)
{
	return aios_http_pool_get(info->http_pool);
}

static void http_client_put(struct aios_sb_info *info, struct aios_http_client *c)
{
	aios_http_pool_put(info->http_pool, c);
}

/* Sleep before retry @attempt of a contended directory / CAS operation. */
static void http_retry_backoff(int attempt)
{
	unsigned int ms = 2u << min(attempt, 7); /* 2, 4, ... 256 */

	ms += get_random_u32() % (ms + 1);
	msleep(min(ms, 500u));
}

/* -EAGAIN is what the HTTP layer returns for CAS / lock conflicts; it is not
 * a valid result for open/unlink/rename, so once retries are exhausted callers
 * report -EBUSY instead. */
static int http_no_eagain(int err)
{
	return err == -EAGAIN ? -EBUSY : err;
}

struct aios_dir_ent {
	char name[AIOS_KABI_NAME_MAX + 1];
	u64 ino;
};

struct aios_dir_table {
	struct aios_dir_ent *ents;
	unsigned int count;
	u64 ino;
	u64 next_op;
	u64 log_bytes;
	u64 snapshot_op;
	u64 meta_cas;
	char meta_oid[160];
	char log_oid[160];
	char snap_oid[160];
};

struct aios_dir_cache_slot {
	u64 ino;
	struct aios_dir_ent *ents;
	unsigned int count;
	u64 next_op;
	u64 log_bytes;
	u64 snapshot_op;
	u64 meta_cas;
	unsigned long loaded;
};

struct aios_dir_cache {
	struct aios_dir_cache_slot slots[AIOS_DIR_CACHE_SLOTS];
};

struct aios_inode_meta {
	u64 ino;
	u32 mode;
	u32 nlink;
	u32 uid;
	u32 gid;
	u64 size;
	u64 atime_ns;
	u64 mtime_ns;
	u64 ctime_ns;
	u64 stripe_unit;
	u32 stripe_width;
	u64 cas;
	bool exists;
	bool extras_loaded;
	char *xattrs_obj;
	char *symlink;
};

/*
 * Directory lease (delegation).
 *
 * A directory that this mount is actively modifying is leased: we hold the
 * server lock on its meta object and keep renewing it. While the lease lasts
 * nobody else can commit to the directory, so namespace operations only
 * update the dcache / directory cache and queue a changelog record here; a
 * worker appends the queued records in batches, advances meta once per batch
 * and updates the parent inode's mtime/nlink once per batch. create/unlink/
 * rename thus cost no synchronous directory round trip.
 *
 * Losing the lease (renew fails: expired, fenced or broken past the grace
 * period) drops back to the synchronous per-operation protocol; queued
 * records are re-committed one by one under fresh locks, and anything that
 * still fails is reported by the next fsync/syncfs of the directory (POSIX
 * makes no durability promise for metadata before fsync). A peer asking for
 * the lease (break) is honoured by flushing and releasing at the next renew.
 *
 * Locking: the list and every lease's ino are stable under http_mu (leases
 * are created / recycled by namespace operations). lease->mu covers the
 * queue and state; flush_mu serialises a flush against a cache-miss reload
 * so a load never sees a record both on the server and still in the queue.
 */
struct aios_lease_op {
	struct list_head node;
	u32 op;
	char a0[AIOS_KABI_NAME_MAX + 1];
	char a1[AIOS_KABI_NAME_MAX + 1];
};

struct aios_dir_lease {
	struct list_head node;
	struct aios_sb_info *info;
	struct mutex mu;
	struct mutex flush_mu;
	u64 ino;
	bool held;             /* server lease believed valid */
	bool break_requested;  /* a peer wants it: flush, release, stop */
	bool release_wanted;   /* a local sync path wants it dropped */
	char token[128];
	char meta_oid[160];
	char log_oid[160];
	char snap_oid[160];
	unsigned long expires;    /* jiffies; conservative local copy */
	unsigned long last_use;   /* jiffies of the last queued op */
	unsigned long last_renew; /* jiffies */
	unsigned long next_try;   /* jiffies; do not re-acquire before */
	/* Directory meta as of the last commit we made (or the load at acquire). */
	u64 next_op;
	u64 log_bytes;
	u64 snapshot_op;
	u64 meta_cas;
	struct list_head pending; /* aios_lease_op, oldest first */
	unsigned int npending;
	/* Deferred parent inode update. */
	bool parent_dirty;
	u64 parent_mtime_ns;
	int nlink_delta;
	int err; /* sticky error from an async commit, returned by fsync */
	struct delayed_work work;
	wait_queue_head_t wq;
};

/* Lease tuning. Renew well inside the TTL; the break grace must exceed the
 * renew interval so a holder always sees the break before the deadline. */
#define AIOS_LEASE_TTL_MS 30000
#define AIOS_LEASE_RENEW_MS 3000
#define AIOS_LEASE_BREAK_GRACE_MS 5000
#define AIOS_LEASE_FLUSH_MS 20
#define AIOS_LEASE_IDLE_MS 10000
#define AIOS_LEASE_RETRY_MS 1000
#define AIOS_LEASE_MAX 32
#define AIOS_LEASE_MAX_PENDING 1024
#define AIOS_LEASE_BATCH_BYTES (64u * 1024u)

static u64 now_ns(void)
{
	return (u64)ktime_to_ns(ktime_get_real());
}

static void oid_super(const char *vol, char *out, size_t n)
{
	snprintf(out, n, "posix/%s/super", vol);
}

static void oid_ino(const char *vol, u64 ino, char *out, size_t n)
{
	snprintf(out, n, "posix/%s/ino/%llu", vol, (unsigned long long)ino);
}

static void oid_dir_meta(const char *vol, u64 ino, char *out, size_t n)
{
	snprintf(out, n, "posix/%s/dir/%llu/meta", vol, (unsigned long long)ino);
}

static void oid_dir_log(const char *vol, u64 ino, char *out, size_t n)
{
	snprintf(out, n, "posix/%s/dir/%llu/log", vol, (unsigned long long)ino);
}

static void oid_dir_snap(const char *vol, u64 ino, char *out, size_t n)
{
	snprintf(out, n, "posix/%s/dir/%llu/snap", vol, (unsigned long long)ino);
}

static void oid_chunk(const char *vol, u64 ino, u64 chunk, char *out, size_t n)
{
	snprintf(out, n, "posix/%s/data/%llu/c/%llu", vol, (unsigned long long)ino,
		 (unsigned long long)chunk);
}

static bool json_get_u64(const char *json, const char *key, u64 *out)
{
	char pat[64];
	const char *p;
	int n;
	char num[32];
	size_t i;

	n = snprintf(pat, sizeof(pat), "\"%s\":", key);
	if (n < 0 || n >= (int)sizeof(pat))
		return false;
	p = strstr(json, pat);
	if (!p)
		return false;
	p += n;
	while (*p == ' ' || *p == '\t')
		p++;
	i = 0;
	while (p[i] >= '0' && p[i] <= '9' && i < sizeof(num) - 1) {
		num[i] = p[i];
		i++;
	}
	if (i == 0)
		return false;
	num[i] = '\0';
	return kstrtou64(num, 10, out) == 0;
}

static bool json_get_u32(const char *json, const char *key, u32 *out)
{
	u64 v;

	if (!json_get_u64(json, key, &v))
		return false;
	*out = (u32)v;
	return true;
}

static int parse_u64_digits(const char *p, u64 *out, const char **end_out)
{
	char num[32];
	size_t i = 0;

	while (p[i] >= '0' && p[i] <= '9' && i < sizeof(num) - 1) {
		num[i] = p[i];
		i++;
	}
	if (i == 0)
		return -EINVAL;
	num[i] = '\0';
	if (kstrtou64(num, 10, out))
		return -EINVAL;
	if (end_out)
		*end_out = p + i;
	return 0;
}

/* Parse {"entries":{"a":1,"b":2}} — names are JSON-escaped. */
static int json_escape_append(char *dst, size_t cap, size_t *pos, const char *src)
{
	for (; *src; src++) {
		const char *esc = NULL;
		char hex[7];

		switch (*src) {
		case '"':
			esc = "\\\"";
			break;
		case '\\':
			esc = "\\\\";
			break;
		case '\b':
			esc = "\\b";
			break;
		case '\f':
			esc = "\\f";
			break;
		case '\n':
			esc = "\\n";
			break;
		case '\r':
			esc = "\\r";
			break;
		case '\t':
			esc = "\\t";
			break;
		default:
			if ((unsigned char)*src < 0x20) {
				snprintf(hex, sizeof(hex), "\\u%04x", (unsigned char)*src);
				esc = hex;
			}
			break;
		}
		if (esc) {
			size_t elen = strlen(esc);

			if (*pos + elen >= cap)
				return -EOVERFLOW;
			memcpy(dst + *pos, esc, elen);
			*pos += elen;
		} else {
			if (*pos + 1 >= cap)
				return -EOVERFLOW;
			dst[(*pos)++] = *src;
		}
	}
	return 0;
}

static int parse_json_quoted(const char **pp, const char *end, char *out, size_t out_cap)
{
	const char *p = *pp;
	size_t o = 0;

	if (p >= end || *p != '"')
		return -EINVAL;
	p++;
	while (p < end && *p != '"') {
		char c = *p++;

		if (c != '\\') {
			if (o + 1 >= out_cap)
				return -ENAMETOOLONG;
			out[o++] = c;
			continue;
		}
		if (p >= end)
			return -EINVAL;
		c = *p++;
		switch (c) {
		case '"':
		case '\\':
		case '/':
			break;
		case 'b':
			c = '\b';
			break;
		case 'f':
			c = '\f';
			break;
		case 'n':
			c = '\n';
			break;
		case 'r':
			c = '\r';
			break;
		case 't':
			c = '\t';
			break;
		case 'u': {
			char hex[5];
			unsigned int cp;
			int j;

			if (p + 4 > end)
				return -EINVAL;
			for (j = 0; j < 4; j++)
				hex[j] = p[j];
			hex[4] = '\0';
			p += 4;
			if (kstrtouint(hex, 16, &cp) || cp > 0x7f)
				return -EINVAL;
			c = (char)cp;
			break;
		}
		default:
			return -EINVAL;
		}
		if (o + 1 >= out_cap)
			return -ENAMETOOLONG;
		out[o++] = c;
	}
	if (p >= end || *p != '"')
		return -EINVAL;
	out[o] = '\0';
	*pp = p + 1;
	return 0;
}

static int parse_entries(const char *json, struct aios_dir_table *dt)
{
	const char *p = strstr(json, "\"entries\"");
	const char *end;

	dt->count = 0;
	if (!p)
		return 0;
	p = strchr(p, '{');
	if (!p)
		return 0;
	p++;
	end = json + strlen(json);
	while (p < end && *p) {
		u64 ino;
		const char *after;
		int err;

		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ','))
			p++;
		if (p >= end || *p == '}')
			break;
		if (*p != '"')
			return -EINVAL;
		if (dt->count >= AIOS_HTTP_MAX_DIR_ENTS)
			return -ENOSPC;
		err = parse_json_quoted(&p, end, dt->ents[dt->count].name,
					sizeof(dt->ents[0].name));
		if (err)
			return err;
		while (p < end && (*p == ' ' || *p == '\t' || *p == ':'))
			p++;
		err = parse_u64_digits(p, &ino, &after);
		if (err)
			return err;
		dt->ents[dt->count].ino = ino;
		dt->count++;
		p = after;
		while (p < end && *p && *p != ',' && *p != '}')
			p++;
	}
	return 0;
}

static bool read_le32(const char **pp, const char *end, u32 *v)
{
	__le32 raw;

	if (*pp + 4 > end)
		return false;
	memcpy(&raw, *pp, 4);
	*v = le32_to_cpu(raw);
	*pp += 4;
	return true;
}

static bool read_le64(const char **pp, const char *end, u64 *v)
{
	__le64 raw;

	if (*pp + 8 > end)
		return false;
	memcpy(&raw, *pp, 8);
	*v = le64_to_cpu(raw);
	*pp += 8;
	return true;
}

static int apply_dir_op(struct aios_dir_table *dt, u32 op, const char *a0, const char *a1)
{
	unsigned int i;

	if (op == AIOS_HTTP_OP_LINK) {
		u64 ino;

		if (!a0 || !a1 || kstrtou64(a1, 10, &ino))
			return -EINVAL;
		for (i = 0; i < dt->count; i++) {
			if (!strcmp(dt->ents[i].name, a0)) {
				dt->ents[i].ino = ino;
				return 0;
			}
		}
		if (dt->count >= AIOS_HTTP_MAX_DIR_ENTS)
			return -ENOSPC;
		strscpy(dt->ents[dt->count].name, a0, sizeof(dt->ents[0].name));
		dt->ents[dt->count].ino = ino;
		dt->count++;
		return 0;
	}
	if (op == AIOS_HTTP_OP_UNLINK) {
		for (i = 0; i < dt->count; i++) {
			if (!strcmp(dt->ents[i].name, a0)) {
				dt->ents[i] = dt->ents[dt->count - 1];
				dt->count--;
				return 0;
			}
		}
		return 0;
	}
	if (op == AIOS_HTTP_OP_RENAME) {
		for (i = 0; i < dt->count; i++) {
			if (!strcmp(dt->ents[i].name, a1)) {
				dt->ents[i] = dt->ents[dt->count - 1];
				dt->count--;
				break;
			}
		}
		for (i = 0; i < dt->count; i++) {
			if (!strcmp(dt->ents[i].name, a0)) {
				strscpy(dt->ents[i].name, a1, sizeof(dt->ents[i].name));
				return 0;
			}
		}
		return 0;
	}
	return 0;
}

static int decode_and_apply_log(struct aios_dir_table *dt, const char *buf, size_t len,
				u64 snapshot_op)
{
	const char *p = buf;
	const char *end = buf + len;

	while (p + 8 <= end) {
		u32 magic = 0, header_len = 0, op_u = 0, payload_len = 0;
		u64 op_id = 0;
		const char *h;
		const char *hend;
		const char *pay;
		const char *pend;
		char a0[AIOS_KABI_NAME_MAX + 1];
		char a1[AIOS_KABI_NAME_MAX + 1]; /* ino string, or new name for RENAME */
		unsigned int nargs = 0;

		if (!read_le32(&p, end, &magic) || magic != AIOS_HTTP_AOPK_MAGIC)
			return -EIO;
		if (!read_le32(&p, end, &header_len))
			return 0;
		if (p + header_len > end)
			return 0;
		h = p;
		hend = p + header_len;
		p = hend;
		if (!read_le64(&h, hend, &op_id) || !read_le32(&h, hend, &op_u) ||
		    !read_le32(&h, hend, &payload_len))
			return -EIO;
		if (p + payload_len > end)
			return 0;
		pay = p;
		pend = p + payload_len;
		p += payload_len;
		if (op_id <= snapshot_op)
			continue;
		a0[0] = a1[0] = '\0';
		while (pay < pend && nargs < 2) {
			u32 slen = 0;
			char *dst = nargs ? a1 : a0;
			size_t cap = nargs ? sizeof(a1) : sizeof(a0);

			if (!read_le32(&pay, pend, &slen) || pay + slen > pend)
				return -EIO;
			if (slen >= cap)
				return -ENAMETOOLONG;
			memcpy(dst, pay, slen);
			dst[slen] = '\0';
			pay += slen;
			nargs++;
		}
		apply_dir_op(dt, op_u, a0, a1);
	}
	return 0;
}

static int dir_table_init(struct aios_dir_table *dt, const char *vol, u64 ino)
{
	memset(dt, 0, sizeof(*dt));
	dt->ents = kcalloc(AIOS_HTTP_MAX_DIR_ENTS, sizeof(*dt->ents), GFP_KERNEL);
	if (!dt->ents)
		return -ENOMEM;
	dt->ino = ino;
	dt->next_op = 1;
	oid_dir_meta(vol, ino, dt->meta_oid, sizeof(dt->meta_oid));
	oid_dir_log(vol, ino, dt->log_oid, sizeof(dt->log_oid));
	oid_dir_snap(vol, ino, dt->snap_oid, sizeof(dt->snap_oid));
	return 0;
}

static void dir_table_free(struct aios_dir_table *dt)
{
	kfree(dt->ents);
	dt->ents = NULL;
}

static void dir_cache_free_all(struct aios_sb_info *info)
{
	unsigned int i;

	if (!info->dir_cache)
		return;
	for (i = 0; i < AIOS_DIR_CACHE_SLOTS; i++)
		kfree(info->dir_cache->slots[i].ents);
	kfree(info->dir_cache);
	info->dir_cache = NULL;
}

static void dir_cache_publish(struct aios_sb_info *info, const struct aios_dir_table *dt)
{
	struct aios_dir_cache_slot *slot;
	struct aios_dir_ent *copy = NULL;
	struct aios_dir_ent *old;
	unsigned int i, victim = 0;
	unsigned long oldest;

	if (!info->dir_cache || !dt->ents)
		return;
	/* Copy outside the lock; only the pointer swap happens under it. */
	if (dt->count) {
		copy = kmemdup(dt->ents, dt->count * sizeof(*dt->ents), GFP_KERNEL);
		if (!copy)
			return;
	}
	mutex_lock(&info->dir_cache_mu);
	oldest = info->dir_cache->slots[0].loaded;
	for (i = 0; i < AIOS_DIR_CACHE_SLOTS; i++) {
		slot = &info->dir_cache->slots[i];
		if (slot->ino == dt->ino) {
			victim = i;
			break;
		}
		if (!slot->ino) {
			victim = i;
			break;
		}
		if (time_before(slot->loaded, oldest)) {
			oldest = slot->loaded;
			victim = i;
		}
	}
	slot = &info->dir_cache->slots[victim];
	old = slot->ents;
	slot->ino = dt->ino;
	slot->ents = copy;
	slot->count = dt->count;
	slot->next_op = dt->next_op;
	slot->log_bytes = dt->log_bytes;
	slot->snapshot_op = dt->snapshot_op;
	slot->meta_cas = dt->meta_cas;
	slot->loaded = jiffies;
	mutex_unlock(&info->dir_cache_mu);
	kfree(old);
}

/* Forget a directory so the next load goes to the server (after a mutation
 * whose resulting table we do not have, e.g. a compaction race). */
static void dir_cache_invalidate(struct aios_sb_info *info, u64 ino)
{
	struct aios_dir_ent *old = NULL;
	unsigned int i;

	if (!info->dir_cache)
		return;
	mutex_lock(&info->dir_cache_mu);
	for (i = 0; i < AIOS_DIR_CACHE_SLOTS; i++) {
		struct aios_dir_cache_slot *slot = &info->dir_cache->slots[i];

		if (slot->ino == ino) {
			old = slot->ents;
			memset(slot, 0, sizeof(*slot));
			break;
		}
	}
	mutex_unlock(&info->dir_cache_mu);
	kfree(old);
}

static bool dir_cache_lookup(struct aios_sb_info *info, struct aios_dir_table *dt,
			     bool ignore_ttl)
{
	unsigned int i;
	bool hit = false;

	if (!info->dir_cache)
		return false;
	mutex_lock(&info->dir_cache_mu);
	for (i = 0; i < AIOS_DIR_CACHE_SLOTS; i++) {
		struct aios_dir_cache_slot *slot = &info->dir_cache->slots[i];

		if (!slot->ino || slot->ino != dt->ino)
			continue;
		if (!ignore_ttl &&
		    !time_before(jiffies, slot->loaded + msecs_to_jiffies(info->attr_ttl_ms)))
			continue;
		if (ignore_ttl)
			slot->loaded = jiffies; /* keep the leased table out of LRU's way */
		if (slot->count > AIOS_HTTP_MAX_DIR_ENTS)
			break;
		if (slot->count)
			memcpy(dt->ents, slot->ents, slot->count * sizeof(*slot->ents));
		dt->count = slot->count;
		dt->next_op = slot->next_op;
		dt->log_bytes = slot->log_bytes;
		dt->snapshot_op = slot->snapshot_op;
		dt->meta_cas = slot->meta_cas;
		hit = true;
		break;
	}
	mutex_unlock(&info->dir_cache_mu);
	return hit;
}

/* Load the directory tip exactly as the server has it: meta, snapshot and
 * the committed part of the log. No cache, no lease. */
static int dir_load_raw(struct aios_sb_info *info, struct aios_http_client *c,
			struct aios_dir_table *dt)
{
	struct aios_http_buf body = { 0 };
	int err;

	dt->count = 0;
	dt->next_op = 1;
	dt->log_bytes = 0;
	dt->snapshot_op = 0;
	dt->meta_cas = 0;

	err = aios_http_get(c, dt->meta_oid, &body, &dt->meta_cas);
	if (err == -ENOENT)
		return 0;
	if (err)
		return err;
	{
		char *js = kmalloc(body.len + 1, GFP_KERNEL);

		if (!js) {
			aios_http_buf_free(&body);
			return -ENOMEM;
		}
		memcpy(js, body.data, body.len);
		js[body.len] = '\0';
		json_get_u64(js, "next_op", &dt->next_op);
		json_get_u64(js, "log_bytes", &dt->log_bytes);
		json_get_u64(js, "snapshot_op", &dt->snapshot_op);
		kfree(js);
	}
	aios_http_buf_free(&body);

	if (dt->snapshot_op > 0) {
		err = aios_http_get(c, dt->snap_oid, &body, NULL);
		if (!err && body.len) {
			char *js = kmalloc(body.len + 1, GFP_KERNEL);

			if (!js) {
				aios_http_buf_free(&body);
				return -ENOMEM;
			}
			memcpy(js, body.data, body.len);
			js[body.len] = '\0';
			err = parse_entries(js, dt);
			kfree(js);
			aios_http_buf_free(&body);
			if (err)
				return err;
		} else if (err == -ENOENT) {
			err = 0;
		} else if (err) {
			return err;
		}
	}

	if (dt->log_bytes == 0)
		return 0;
	err = aios_http_get_range(c, dt->log_oid, 0, dt->log_bytes - 1, &body);
	if (err == -ENOENT)
		return 0;
	if (err)
		return err;
	err = decode_and_apply_log(dt, body.data, body.len, dt->snapshot_op);
	aios_http_buf_free(&body);
	return err;
}

static struct aios_dir_lease *dir_lease_find(struct aios_sb_info *info, u64 ino);
static bool dir_lease_owns(struct aios_dir_lease *l);
static int dir_lease_overlay(struct aios_sb_info *info, struct aios_http_client *c,
			     struct aios_dir_table *dt);

/*
 * Load a directory table for a namespace operation (caller holds http_mu).
 *
 * Without a lease this is the cached table when @allow_cache and it is within
 * actimeo, else the server tip. Under a lease the cached table is authoritative
 * for as long as the lease lasts (nobody else can write), so it is used
 * regardless of @allow_cache or age; on a cache miss the server tip is loaded
 * and the operations still queued in the lease are replayed on top.
 */
static int dir_load(struct aios_sb_info *info, struct aios_dir_table *dt, bool allow_cache)
{
	struct aios_dir_lease *l = dir_lease_find(info, dt->ino);
	/* Also while a lost lease still has records queued for replay: the
	 * server tip alone would not show them. */
	bool leased = l && (dir_lease_owns(l) || READ_ONCE(l->npending));
	int err;

	if ((allow_cache || leased) && dir_cache_lookup(info, dt, leased))
		return 0;
	if (leased)
		return dir_lease_overlay(info, info->http, dt);
	err = dir_load_raw(info, info->http, dt);
	if (!err && allow_cache)
		dir_cache_publish(info, dt);
	return err;
}

/* Allocates *snap_out / *meta_out (caller kfree). Updates dt next_op/snapshot fields. */
static int dir_plan_compact(struct aios_dir_table *dt, char **snap_out, char **meta_out)
{
	char *snap = NULL;
	char *meta = NULL;
	char *ename = NULL;
	size_t snap_cap;
	size_t pos;
	unsigned int i;
	u64 next = dt->next_op < 2 ? 2 : dt->next_op;
	u64 snap_op = next - 1;
	int err = -EOVERFLOW;

	snap_cap = 32 + dt->count * (AIOS_KABI_NAME_MAX * 6 + 32);
	snap = kmalloc(snap_cap, GFP_KERNEL);
	meta = kmalloc(512, GFP_KERNEL);
	ename = kmalloc(AIOS_KABI_NAME_MAX * 6 + 8, GFP_KERNEL);
	if (!snap || !meta || !ename) {
		err = -ENOMEM;
		goto out_err;
	}
	pos = scnprintf(snap, snap_cap, "{\"entries\":{");
	for (i = 0; i < dt->count; i++) {
		size_t epos = 0;

		if (i)
			pos += scnprintf(snap + pos, snap_cap - pos, ",");
		pos += scnprintf(snap + pos, snap_cap - pos, "\"");
		if (json_escape_append(ename, AIOS_KABI_NAME_MAX * 6 + 8, &epos,
				       dt->ents[i].name))
			goto out_err;
		ename[epos] = '\0';
		pos += scnprintf(snap + pos, snap_cap - pos, "%s\":%llu", ename,
				 (unsigned long long)dt->ents[i].ino);
		if (pos >= snap_cap)
			goto out_err;
	}
	scnprintf(snap + pos, snap_cap - pos, "}}");
	scnprintf(meta, 512,
		  "{\"aios_posix_dir\":1,\"next_op\":%llu,\"log_bytes\":0,"
		  "\"snapshot_op\":%llu,\"snapshot_oid\":\"%s\"}",
		  (unsigned long long)next, (unsigned long long)snap_op, dt->snap_oid);
	dt->next_op = next;
	dt->log_bytes = 0;
	dt->snapshot_op = snap_op;
	*snap_out = snap;
	*meta_out = meta;
	kfree(ename);
	return 0;

out_err:
	kfree(ename);
	kfree(snap);
	kfree(meta);
	return err;
}

struct aios_held_lock {
	char oid[160];
	char token[128];
};

static int held_lock_cmp(const void *a, const void *b)
{
	return strcmp(((const struct aios_held_lock *)a)->oid,
		      ((const struct aios_held_lock *)b)->oid);
}

static void release_held_locks(struct aios_http_client *c, struct aios_held_lock *locks,
			       unsigned int n)
{
	while (n--) {
		if (locks[n].token[0])
			aios_http_lock_release(c, locks[n].oid, locks[n].token);
		locks[n].token[0] = '\0';
	}
}

static int acquire_sorted_locks(struct aios_http_client *c, struct aios_held_lock *locks,
				unsigned int *n_inout)
{
	unsigned int n = *n_inout;
	unsigned int i;
	int err;

	sort(locks, n, sizeof(*locks), held_lock_cmp, NULL);
	/* Dedup adjacent identical oids. */
	for (i = 1; i < n;) {
		if (!strcmp(locks[i].oid, locks[i - 1].oid)) {
			memmove(&locks[i], &locks[i + 1], (n - i - 1) * sizeof(*locks));
			n--;
		} else {
			i++;
		}
	}
	*n_inout = n;
	for (i = 0; i < n; i++) {
		err = aios_http_lock_acquire(c, locks[i].oid, 30000, locks[i].token,
					     sizeof(locks[i].token));
		if (err) {
			/* A lease holder (kernel peer with a directory delegation)
			 * gives the lock back once asked; the caller's backoff
			 * loop covers the grace period. */
			if (err == -EAGAIN)
				aios_http_lock_break(c, locks[i].oid, AIOS_LEASE_BREAK_GRACE_MS);
			release_held_locks(c, locks, i);
			return err;
		}
	}
	return 0;
}

static const char *token_for(struct aios_held_lock *locks, unsigned int n, const char *oid)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (!strcmp(locks[i].oid, oid))
			return locks[i].token;
	}
	return NULL;
}

static int txn_put_dir(struct aios_http_client *c, const char *txn_id, struct aios_dir_table *dt,
		       struct aios_held_lock *locks, unsigned int nlocks)
{
	char *snap = NULL;
	char *meta = NULL;
	u64 meta_cas;
	int err;

	err = dir_plan_compact(dt, &snap, &meta);
	if (err)
		return err;
	err = aios_http_txn_prepare_put(c, txn_id, dt->snap_oid, snap, strlen(snap),
					token_for(locks, nlocks, dt->snap_oid), NULL);
	if (err)
		goto out;
	err = aios_http_txn_prepare_put(c, txn_id, dt->log_oid, "", 0,
					token_for(locks, nlocks, dt->log_oid), NULL);
	if (err)
		goto out;
	meta_cas = dt->meta_cas;
	err = aios_http_txn_prepare_put(c, txn_id, dt->meta_oid, meta, strlen(meta),
					token_for(locks, nlocks, dt->meta_oid), &meta_cas);
	if (!err)
		dt->meta_cas = meta_cas;
out:
	kfree(snap);
	kfree(meta);
	return err;
}

static int dir_find(struct aios_dir_table *dt, const char *name, u64 *ino_out);

static int dir_meta_json(const struct aios_dir_table *dt, char *out, size_t cap)
{
	int w = snprintf(out, cap,
			 "{\"aios_posix_dir\":1,\"next_op\":%llu,\"log_bytes\":%llu,"
			 "\"snapshot_op\":%llu,\"snapshot_oid\":\"%s\"}",
			 (unsigned long long)dt->next_op, (unsigned long long)dt->log_bytes,
			 (unsigned long long)dt->snapshot_op, dt->snap_oid);

	return (w < 0 || (size_t)w >= cap) ? -EOVERFLOW : w;
}

static void put_le32(u8 **p, u32 v)
{
	__le32 raw = cpu_to_le32(v);

	memcpy(*p, &raw, 4);
	*p += 4;
}

static void put_le64(u8 **p, u64 v)
{
	__le64 raw = cpu_to_le64(v);

	memcpy(*p, &raw, 8);
	*p += 8;
}

/*
 * One changelog record in the format libaios_posix's changelog::encode_record
 * produces and decode_and_apply_log above consumes:
 *   u32 magic 'AOPk', u32 header_len (16), { u64 op_id, u32 op, u32 payload_len },
 *   payload = [ u32 len, bytes ]* for each argument.
 * UNLINK carries a0 only; LINK / RENAME carry a0 and a1.
 */
static size_t dir_encode_record(u64 op_id, u32 op, const char *a0, const char *a1, u8 *out,
				size_t cap)
{
	const size_t l0 = strlen(a0);
	const size_t l1 = a1 ? strlen(a1) : 0;
	const size_t payload = 4 + l0 + (a1 ? 4 + l1 : 0);
	const size_t total = 8 + 16 + payload;
	u8 *p = out;

	if (total > cap)
		return 0;
	put_le32(&p, AIOS_HTTP_AOPK_MAGIC);
	put_le32(&p, 16);
	put_le64(&p, op_id);
	put_le32(&p, op);
	put_le32(&p, (u32)payload);
	put_le32(&p, (u32)l0);
	memcpy(p, a0, l0);
	p += l0;
	if (a1) {
		put_le32(&p, (u32)l1);
		memcpy(p, a1, l1);
		p += l1;
	}
	return total;
}

/*
 * Compact the directory tip under all three object locks and a /txn: snapshot
 * of the in-memory table, empty log, meta. dt already reflects the state to
 * publish (including any op the caller just applied). locks[0] is the meta
 * lock the caller holds; log and snap are acquired here and released before
 * returning. Returns -EAGAIN when a lock or the meta CAS is contended.
 */
static int dir_compact_locked(struct aios_http_client *c, struct aios_dir_table *dt,
			      struct aios_held_lock *locks, char *txn_id)
{
	unsigned int nlocks = 3;
	int err;

	strscpy(locks[1].oid, dt->log_oid, sizeof(locks[1].oid));
	strscpy(locks[2].oid, dt->snap_oid, sizeof(locks[2].oid));
	locks[1].token[0] = locks[2].token[0] = '\0';
	err = aios_http_lock_acquire(c, locks[1].oid, 30000, locks[1].token,
				     sizeof(locks[1].token));
	if (err)
		return err;
	err = aios_http_lock_acquire(c, locks[2].oid, 30000, locks[2].token,
				     sizeof(locks[2].token));
	if (err) {
		release_held_locks(c, &locks[1], 1);
		return err;
	}

	txn_id[0] = '\0';
	err = aios_http_txn_begin(c, txn_id, AIOS_HTTP_TXN_ID_LEN);
	if (!err)
		err = txn_put_dir(c, txn_id, dt, locks, nlocks);
	if (!err)
		err = aios_http_txn_commit(c, txn_id);
	if (err && txn_id[0])
		aios_http_txn_abort(c, txn_id);
	txn_id[0] = '\0';
	release_held_locks(c, &locks[1], 2);
	return err;
}

/*
 * Commit one directory operation: append a changelog record and advance meta
 * under the directory's meta lock (the same protocol libaios_posix's
 * DirTable::link_if_absent / unlink_if use), so the common case costs one
 * lock, the reload, one append and one CAS PUT instead of a full snapshot
 * rewrite through a transaction. The table is reloaded under the lock so a
 * peer's commit between the caller's load and here is not lost.
 *
 * A record whose append landed beyond the committed log_bytes has garbage in
 * front of it (a writer died between append and meta PUT, or a lock-free
 * peer is mid-commit). Publishing log_bytes past that garbage would replay it
 * on every load, so that case, and a log past AIOS_HTTP_LOG_COMPACT_BYTES,
 * are committed as a compaction instead: the snapshot carries the table, the
 * log is emptied, and whatever sat in it is gone.
 *
 * must_be_absent: LINK fails with -EEXIST if a0 already exists.
 * UNLINK / RENAME fail with -ENOENT if a0 vanished under the lock.
 */
static int dir_commit_sync(struct aios_sb_info *info, struct aios_http_client *c,
			   struct aios_dir_table *dt, u32 op, const char *a0, const char *a1,
			   bool must_be_absent)
{
	struct {
		struct aios_held_lock locks[3];
		char txn_id[AIOS_HTTP_TXN_ID_LEN];
		char meta[512];
		char extra[192];
		u8 rec[8 + 16 + 8 + 2 * (AIOS_KABI_NAME_MAX + 1)];
	} *b;
	int attempt;
	int err = -EAGAIN;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	for (attempt = 0; attempt < AIOS_HTTP_DIR_RETRIES; attempt++) {
		size_t rec_len;
		u64 new_size = 0;
		u64 cas;
		int mlen;

		strscpy(b->locks[0].oid, dt->meta_oid, sizeof(b->locks[0].oid));
		b->locks[0].token[0] = '\0';
		err = aios_http_lock_acquire(c, b->locks[0].oid, 30000,
					     b->locks[0].token, sizeof(b->locks[0].token));
		if (err == -EAGAIN) {
			/* Possibly a peer's lease: ask for it back (idempotent),
			 * then wait. */
			aios_http_lock_break(c, b->locks[0].oid, AIOS_LEASE_BREAK_GRACE_MS);
			http_retry_backoff(attempt);
			continue;
		}
		if (err)
			break;

		err = dir_load_raw(info, c, dt);
		if (err)
			goto unlock;
		if (op == AIOS_HTTP_OP_LINK && must_be_absent && !dir_find(dt, a0, NULL)) {
			err = -EEXIST;
			goto unlock;
		}
		if ((op == AIOS_HTTP_OP_UNLINK || op == AIOS_HTTP_OP_RENAME) &&
		    dir_find(dt, a0, NULL)) {
			err = -ENOENT;
			goto unlock;
		}

		rec_len = dir_encode_record(dt->next_op, op,
					    a0, op == AIOS_HTTP_OP_UNLINK ? NULL : a1, b->rec,
					    sizeof(b->rec));
		if (!rec_len) {
			err = -ENAMETOOLONG;
			goto unlock;
		}
		err = aios_http_append(c, dt->log_oid, b->rec, rec_len, NULL, &new_size);
		if (err)
			goto retry_or_fail;

		err = apply_dir_op(dt, op, a0, a1);
		if (err)
			goto unlock;
		dt->next_op += 1;

		if (new_size - rec_len != dt->log_bytes ||
		    new_size >= AIOS_HTTP_LOG_COMPACT_BYTES) {
			err = dir_compact_locked(c, dt, b->locks, b->txn_id);
			if (err)
				goto retry_or_fail;
			/* txn_put_dir advanced dt (next_op, log_bytes = 0, snapshot_op, meta_cas). */
			goto committed;
		}

		dt->log_bytes = new_size;
		mlen = dir_meta_json(dt, b->meta, sizeof(b->meta));
		if (mlen < 0) {
			err = mlen;
			goto unlock;
		}
		snprintf(b->extra, sizeof(b->extra), "x-aios-lock-token: %s\r\n",
			 b->locks[0].token);
		cas = dt->meta_cas;
		err = aios_http_put(c, dt->meta_oid, b->meta, mlen, b->extra, &cas);
		if (err)
			goto retry_or_fail;
		dt->meta_cas = cas;

committed:
		dir_cache_publish(info, dt);
		release_held_locks(c, b->locks, 1);
		err = 0;
		break;

retry_or_fail:
		release_held_locks(c, b->locks, 1);
		if (err == -EAGAIN) {
			http_retry_backoff(attempt);
			continue;
		}
		break;

unlock:
		release_held_locks(c, b->locks, 1);
		break;
	}
	/* The cached table is stale if we gave up after appending anything. */
	if (err)
		dir_cache_invalidate(info, dt->ino);
	kfree(b);
	return http_no_eagain(err);
}

/* ------------------------------------------------------------------------
 * Directory leases (see struct aios_dir_lease).
 * ------------------------------------------------------------------------ */

static int load_inode_c(struct aios_sb_info *info, struct aios_http_client *c, u64 ino,
			struct aios_inode_meta *m);
static int store_inode_c(struct aios_sb_info *info, struct aios_http_client *c,
			 struct aios_inode_meta *m);
static void inode_meta_reset(struct aios_inode_meta *m);
static void dir_lease_work(struct work_struct *w);

/* Caller holds http_mu (the list only changes under it). */
static struct aios_dir_lease *dir_lease_find(struct aios_sb_info *info, u64 ino)
{
	struct aios_dir_lease *l;

	list_for_each_entry(l, &info->leases, node) {
		if (l->ino == ino)
			return l;
	}
	return NULL;
}

static bool dir_lease_owns_locked(const struct aios_dir_lease *l)
{
	return l->held && time_before(jiffies, l->expires);
}

/* The server lock is ours: the cached table is authoritative. */
static bool dir_lease_owns(struct aios_dir_lease *l)
{
	bool ok;

	mutex_lock(&l->mu);
	ok = dir_lease_owns_locked(l);
	mutex_unlock(&l->mu);
	return ok;
}

/* ... and it may take new asynchronous operations. */
static bool dir_lease_active(struct aios_dir_lease *l)
{
	bool ok;

	mutex_lock(&l->mu);
	ok = dir_lease_owns_locked(l) && !l->break_requested && !l->release_wanted &&
	     time_before(jiffies, l->expires - msecs_to_jiffies(AIOS_LEASE_RENEW_MS));
	mutex_unlock(&l->mu);
	return ok;
}

/* Lockless: these are wait_event() conditions (no sleeping there). */
static bool dir_lease_busy(struct aios_dir_lease *l)
{
	return READ_ONCE(l->held) || READ_ONCE(l->npending) || READ_ONCE(l->parent_dirty);
}

static bool dir_lease_flushed(struct aios_dir_lease *l)
{
	return !READ_ONCE(l->npending) && !READ_ONCE(l->parent_dirty);
}

/* Cache miss under a lease: server tip plus everything still queued here. */
static int dir_lease_overlay(struct aios_sb_info *info, struct aios_http_client *c,
			     struct aios_dir_table *dt)
{
	struct aios_dir_lease *l = dir_lease_find(info, dt->ino);
	struct aios_lease_op *op;
	int err;

	if (!l)
		return dir_load_raw(info, c, dt);
	mutex_lock(&l->flush_mu);
	err = dir_load_raw(info, c, dt);
	if (!err) {
		mutex_lock(&l->mu);
		list_for_each_entry(op, &l->pending, node)
			apply_dir_op(dt, op->op, op->a0, op->a1);
		mutex_unlock(&l->mu);
	}
	mutex_unlock(&l->flush_mu);
	if (!err)
		dir_cache_publish(info, dt);
	return err;
}

static void dir_lease_set_ino(struct aios_dir_lease *l, struct aios_sb_info *info, u64 ino)
{
	l->ino = ino;
	oid_dir_meta(info->volume, ino, l->meta_oid, sizeof(l->meta_oid));
	oid_dir_log(info->volume, ino, l->log_oid, sizeof(l->log_oid));
	oid_dir_snap(info->volume, ino, l->snap_oid, sizeof(l->snap_oid));
	l->err = 0;
	l->next_try = 0;
}

/* New or recycled lease slot for @ino. Caller holds http_mu. */
static struct aios_dir_lease *dir_lease_alloc(struct aios_sb_info *info, u64 ino)
{
	struct aios_dir_lease *l;

	if (info->nleases >= AIOS_LEASE_MAX) {
		struct aios_dir_lease *victim = NULL;

		list_for_each_entry(l, &info->leases, node) {
			/* A slot with an unreported error keeps it for fsync. */
			if (dir_lease_busy(l) || READ_ONCE(l->err))
				continue;
			if (!victim || time_before(l->last_use, victim->last_use))
				victim = l;
		}
		if (!victim)
			return NULL;
		cancel_delayed_work_sync(&victim->work);
		mutex_lock(&victim->mu);
		dir_lease_set_ino(victim, info, ino);
		mutex_unlock(&victim->mu);
		return victim;
	}
	l = kzalloc(sizeof(*l), GFP_KERNEL);
	if (!l)
		return NULL;
	l->info = info;
	mutex_init(&l->mu);
	mutex_init(&l->flush_mu);
	INIT_LIST_HEAD(&l->pending);
	INIT_DELAYED_WORK(&l->work, dir_lease_work);
	init_waitqueue_head(&l->wq);
	dir_lease_set_ino(l, info, ino);
	list_add(&l->node, &info->leases);
	info->nleases++;
	return l;
}

/*
 * Flush what is queued and give the server lock back, then wait for it.
 * Used before a synchronous path takes the directory's locks itself
 * (cross-directory rename, rmdir of the directory) and by umount.
 * Caller holds http_mu.
 */
static void dir_lease_drop(struct aios_sb_info *info, u64 ino)
{
	struct aios_dir_lease *l = dir_lease_find(info, ino);

	if (!l || !info->wb_wq)
		return;
	mutex_lock(&l->mu);
	l->release_wanted = true;
	l->next_try = jiffies + msecs_to_jiffies(AIOS_LEASE_RETRY_MS);
	mutex_unlock(&l->mu);
	mod_delayed_work(info->wb_wq, &l->work, 0);
	wait_event(l->wq, !dir_lease_busy(l));
	mutex_lock(&l->mu);
	l->release_wanted = false;
	mutex_unlock(&l->mu);
}

/*
 * Lease for @ino usable for a new asynchronous op, acquiring it when we do
 * not hold one. NULL means: use the synchronous protocol (leases disabled,
 * lock held by a peer — a break has been requested —, or a recent failure).
 * Caller holds http_mu.
 */
static struct aios_dir_lease *dir_lease_get(struct aios_sb_info *info, u64 ino)
{
	struct aios_dir_lease *l;
	struct aios_dir_table dt;
	char token[128];
	int err;

	if (info->no_lease || !info->wb_wq)
		return NULL;
	l = dir_lease_find(info, ino);
	if (l) {
		if (dir_lease_active(l))
			return l;
		if (dir_lease_owns(l)) {
			/* Break requested / release wanted / about to expire:
			 * hand it back so the sync path can take the lock. */
			dir_lease_drop(info, ino);
			return NULL;
		}
		/* Lost with records still queued: they must reach the server
		 * before anything else is committed to this directory. */
		if (!dir_lease_flushed(l)) {
			mod_delayed_work(info->wb_wq, &l->work, 0);
			wait_event(l->wq, dir_lease_flushed(l));
		}
		mutex_lock(&l->mu);
		if (l->next_try && time_before(jiffies, l->next_try)) {
			mutex_unlock(&l->mu);
			return NULL;
		}
		mutex_unlock(&l->mu);
	} else {
		l = dir_lease_alloc(info, ino);
		if (!l)
			return NULL;
	}

	err = aios_http_lock_acquire(info->http, l->meta_oid, AIOS_LEASE_TTL_MS, token,
				     sizeof(token));
	if (err) {
		if (err == -EAGAIN)
			aios_http_lock_break(info->http, l->meta_oid, AIOS_LEASE_BREAK_GRACE_MS);
		mutex_lock(&l->mu);
		l->next_try = jiffies + msecs_to_jiffies(AIOS_LEASE_RETRY_MS);
		mutex_unlock(&l->mu);
		return NULL;
	}
	/* We own the directory now: load the tip once, it stays authoritative. */
	err = dir_table_init(&dt, info->volume, ino);
	if (!err) {
		err = dir_load_raw(info, info->http, &dt);
		if (err)
			dir_table_free(&dt);
	}
	if (err) {
		aios_http_lock_release(info->http, l->meta_oid, token);
		mutex_lock(&l->mu);
		l->next_try = jiffies + msecs_to_jiffies(AIOS_LEASE_RETRY_MS);
		mutex_unlock(&l->mu);
		return NULL;
	}
	mutex_lock(&l->mu);
	strscpy(l->token, token, sizeof(l->token));
	l->held = true;
	l->break_requested = false;
	l->release_wanted = false;
	l->expires = jiffies + msecs_to_jiffies(AIOS_LEASE_TTL_MS);
	l->last_renew = l->last_use = jiffies;
	l->next_op = dt.next_op;
	l->log_bytes = dt.log_bytes;
	l->snapshot_op = dt.snapshot_op;
	l->meta_cas = dt.meta_cas;
	mutex_unlock(&l->mu);
	dir_cache_publish(info, &dt);
	dir_table_free(&dt);
	mod_delayed_work(info->wb_wq, &l->work, msecs_to_jiffies(AIOS_LEASE_RENEW_MS));
	return l;
}

/*
 * Commit one directory operation. Under a lease the record is queued and the
 * cached table updated; otherwise the synchronous protocol runs. Caller holds
 * http_mu and has loaded @dt through dir_load.
 */
static int dir_commit_op(struct aios_sb_info *info, struct aios_dir_table *dt, u32 op,
			 const char *a0, const char *a1, bool must_be_absent)
{
	struct aios_dir_lease *l = dir_lease_get(info, dt->ino);
	struct aios_lease_op *lop;
	int err;

	if (!l)
		return dir_commit_sync(info, info->http, dt, op, a0, a1, must_be_absent);

	/* The lease may have been acquired just now, after the caller loaded
	 * dt; the cached table is the authoritative one. */
	err = dir_load(info, dt, true);
	if (err)
		return err;
	if (op == AIOS_HTTP_OP_LINK && must_be_absent && !dir_find(dt, a0, NULL))
		return -EEXIST;
	if ((op == AIOS_HTTP_OP_UNLINK || op == AIOS_HTTP_OP_RENAME) && dir_find(dt, a0, NULL))
		return -ENOENT;
	if (strlen(a0) > AIOS_KABI_NAME_MAX || (a1 && strlen(a1) > AIOS_KABI_NAME_MAX))
		return -ENAMETOOLONG;

	lop = kmalloc(sizeof(*lop), GFP_KERNEL);
	if (!lop)
		return -ENOMEM;
	lop->op = op;
	strscpy(lop->a0, a0, sizeof(lop->a0));
	if (a1 && op != AIOS_HTTP_OP_UNLINK)
		strscpy(lop->a1, a1, sizeof(lop->a1));
	else
		lop->a1[0] = '\0';

	err = apply_dir_op(dt, op, a0, lop->a1);
	if (err) {
		kfree(lop);
		return err;
	}
	dt->next_op += 1;

	/* Bound the queue: a flood of creates waits for the flusher. */
	mutex_lock(&l->mu);
	while (l->npending >= AIOS_LEASE_MAX_PENDING) {
		mutex_unlock(&l->mu);
		mod_delayed_work(info->wb_wq, &l->work, 0);
		wait_event(l->wq, l->npending < AIOS_LEASE_MAX_PENDING);
		mutex_lock(&l->mu);
	}
	list_add_tail(&lop->node, &l->pending);
	l->npending++;
	l->last_use = jiffies;
	mutex_unlock(&l->mu);
	dir_cache_publish(info, dt);
	mod_delayed_work(info->wb_wq, &l->work,
			 l->npending >= 64 ? 0 : msecs_to_jiffies(AIOS_LEASE_FLUSH_MS));
	return 0;
}

/*
 * mtime/ctime/nlink of a directory after a namespace change. Under a lease
 * the in-core inode is updated and the server copy is written once per
 * flush; otherwise the CAS PUT happens here.
 */
static int touch_parent(struct aios_sb_info *info, struct aios_inode_meta *pm, u64 ts,
			int nlink_delta);
static int load_inode(struct aios_sb_info *info, u64 ino, struct aios_inode_meta *m);
static void meta_to_stat(const struct aios_inode_meta *m, struct aios_kabi_stat *st);

static int touch_dir_inode(struct aios_sb_info *info, struct inode *dir,
			   struct aios_inode_meta *pm, u64 ts, int nlink_delta)
{
	struct aios_dir_lease *l = dir_lease_find(info, dir->i_ino);
	int err;

	if (l && dir_lease_owns(l)) {
		mutex_lock(&l->mu);
		l->parent_dirty = true;
		if (ts > l->parent_mtime_ns)
			l->parent_mtime_ns = ts;
		l->nlink_delta += nlink_delta;
		l->last_use = jiffies;
		mutex_unlock(&l->mu);
		dir->i_mtime = dir->i_ctime = ns_to_timespec64(ts);
		if (nlink_delta > 0)
			inc_nlink(dir);
		else if (nlink_delta < 0 && dir->i_nlink > 2)
			drop_nlink(dir);
		mod_delayed_work(info->wb_wq, &l->work, msecs_to_jiffies(AIOS_LEASE_FLUSH_MS));
		return 0;
	}
	if (!pm->exists) {
		err = load_inode(info, dir->i_ino, pm);
		if (err)
			return err;
	}
	err = touch_parent(info, pm, ts, nlink_delta);
	if (!err) {
		struct aios_kabi_stat st;

		meta_to_stat(pm, &st);
		aios_stat_to_inode(dir, &st);
	}
	return err;
}

/* Deferred parent update from the flusher: load-modify-CAS with retries. */
static int dir_lease_touch_parent(struct aios_sb_info *info, struct aios_http_client *c,
				  u64 ino, u64 ts, int delta)
{
	struct aios_inode_meta pm = { 0 };
	int attempt;
	int err = -EAGAIN;

	for (attempt = 0; attempt < AIOS_HTTP_META_RETRIES && err == -EAGAIN; attempt++) {
		if (attempt)
			http_retry_backoff(attempt);
		err = load_inode_c(info, c, ino, &pm);
		if (err == -ENOENT) {
			err = 0; /* directory removed meanwhile */
			break;
		}
		if (err)
			break;
		if (delta < 0)
			pm.nlink = (u32)max_t(int, 2, (int)pm.nlink + delta);
		else
			pm.nlink += delta;
		if (ts > pm.mtime_ns)
			pm.mtime_ns = ts;
		if (ts > pm.ctime_ns)
			pm.ctime_ns = ts;
		err = store_inode_c(info, c, &pm);
	}
	inode_meta_reset(&pm);
	return http_no_eagain(err);
}

/* Compact under the lease: table = server tip + the @n records just appended. */
static int dir_lease_compact(struct aios_sb_info *info, struct aios_http_client *c,
			     struct aios_dir_lease *l, unsigned int n)
{
	struct {
		struct aios_held_lock locks[3];
		char txn_id[AIOS_HTTP_TXN_ID_LEN];
		struct aios_dir_table dt;
	} *b;
	struct aios_lease_op *op;
	unsigned int i = 0;
	int attempt;
	int err;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	err = dir_table_init(&b->dt, info->volume, l->ino);
	if (err)
		goto out;
	err = dir_load_raw(info, c, &b->dt);
	if (err)
		goto out_dt;
	mutex_lock(&l->mu);
	list_for_each_entry(op, &l->pending, node) {
		if (i++ == n)
			break;
		apply_dir_op(&b->dt, op->op, op->a0, op->a1);
	}
	b->dt.next_op = l->next_op + n;
	strscpy(b->locks[0].oid, l->meta_oid, sizeof(b->locks[0].oid));
	strscpy(b->locks[0].token, l->token, sizeof(b->locks[0].token));
	mutex_unlock(&l->mu);

	err = -EAGAIN;
	for (attempt = 0; attempt < 4 && err == -EAGAIN; attempt++) {
		if (attempt)
			http_retry_backoff(attempt);
		err = dir_compact_locked(c, &b->dt, b->locks, b->txn_id);
	}
	if (!err) {
		mutex_lock(&l->mu);
		l->next_op = b->dt.next_op;
		l->log_bytes = 0;
		l->snapshot_op = b->dt.snapshot_op;
		l->meta_cas = b->dt.meta_cas;
		mutex_unlock(&l->mu);
	}
out_dt:
	dir_table_free(&b->dt);
out:
	kfree(b);
	return err;
}

/*
 * Append the queued records in batches and advance meta once per batch; then
 * the deferred parent inode update. Returns -ESTALE when the lease is gone
 * (the queue is left intact for the replay), another -errno on a transient
 * failure (queue intact, retried by the next run).
 */
static int dir_lease_flush(struct aios_sb_info *info, struct aios_http_client *c,
			   struct aios_dir_lease *l)
{
	u8 *buf;
	char *meta;
	char extra[192];
	int conflicts = 0;
	int err = 0;

	buf = kvmalloc(AIOS_LEASE_BATCH_BYTES, GFP_KERNEL);
	meta = kmalloc(512, GFP_KERNEL);
	if (!buf || !meta) {
		kvfree(buf);
		kfree(meta);
		return -ENOMEM;
	}
	snprintf(extra, sizeof(extra), "x-aios-lock-token: %s\r\n", l->token);

	for (;;) {
		struct aios_lease_op *op, *tmp;
		size_t len = 0;
		unsigned int n = 0;
		u64 new_size = 0;
		bool pd;
		u64 pts;
		int pdelta;

		mutex_lock(&l->flush_mu);
		mutex_lock(&l->mu);
		list_for_each_entry(op, &l->pending, node) {
			size_t rl = dir_encode_record(l->next_op + n, op->op, op->a0,
						      op->op == AIOS_HTTP_OP_UNLINK ? NULL : op->a1,
						      buf + len, AIOS_LEASE_BATCH_BYTES - len);

			if (!rl)
				break;
			len += rl;
			n++;
		}
		pd = l->parent_dirty;
		pts = l->parent_mtime_ns;
		pdelta = l->nlink_delta;
		l->parent_dirty = false;
		l->nlink_delta = 0;
		mutex_unlock(&l->mu);

		if (!n && !pd) {
			mutex_unlock(&l->flush_mu);
			break;
		}
		if (n) {
			err = aios_http_append(c, l->log_oid, buf, len, l->token, &new_size);
			if (err)
				goto fail;
			if (new_size - len != l->log_bytes ||
			    new_size >= AIOS_HTTP_LOG_COMPACT_BYTES) {
				err = dir_lease_compact(info, c, l, n);
				if (err)
					goto fail;
			} else {
				struct aios_dir_table hdr = { 0 };
				u64 cas = l->meta_cas;
				int mlen;

				hdr.next_op = l->next_op + n;
				hdr.log_bytes = new_size;
				hdr.snapshot_op = l->snapshot_op;
				strscpy(hdr.snap_oid, l->snap_oid, sizeof(hdr.snap_oid));
				mlen = dir_meta_json(&hdr, meta, 512);
				if (mlen < 0) {
					err = mlen;
					goto fail;
				}
				err = aios_http_put(c, l->meta_oid, meta, mlen, extra, &cas);
				if (err)
					goto fail;
				mutex_lock(&l->mu);
				l->meta_cas = cas;
				l->log_bytes = new_size;
				l->next_op += n;
				mutex_unlock(&l->mu);
			}
			mutex_lock(&l->mu);
			list_for_each_entry_safe(op, tmp, &l->pending, node) {
				if (!n)
					break;
				list_del(&op->node);
				kfree(op);
				l->npending--;
				n--;
			}
			mutex_unlock(&l->mu);
		}
		if (pd) {
			err = dir_lease_touch_parent(info, c, l->ino, pts, pdelta);
			if (err) {
				mutex_lock(&l->mu);
				if (!l->err)
					l->err = err;
				mutex_unlock(&l->mu);
				err = 0;
			}
		}
		mutex_unlock(&l->flush_mu);
		wake_up_all(&l->wq);
		continue;

fail:
		mutex_lock(&l->mu);
		if (pd) {
			l->parent_dirty = true;
			l->nlink_delta += pdelta;
			if (pts > l->parent_mtime_ns)
				l->parent_mtime_ns = pts;
		}
		mutex_unlock(&l->mu);
		mutex_unlock(&l->flush_mu);
		/* -EAGAIN is also what a peer transiently holding the log/snap
		 * lock (cross-directory rename) produces, and what our own PUT
		 * that timed out but was applied produces; give those a few
		 * tries. A duplicate append is harmless: it shows up as garbage
		 * past log_bytes and forces a compaction. */
		if (err == -EAGAIN && ++conflicts < 3) {
			http_retry_backoff(conflicts);
			continue;
		}
		/* lock_held / lock_expired / CAS mismatch: someone else owns the
		 * directory now. Anything else is a transport error. */
		if (err == -EAGAIN || err == -ESTALE)
			err = -ESTALE;
		break;
	}
	kvfree(buf);
	kfree(meta);
	return err;
}

/*
 * The lease is gone with records still queued: commit them one by one with
 * the synchronous protocol. What cannot be committed is reported through
 * fsync of the directory.
 */
static void dir_lease_replay(struct aios_sb_info *info, struct aios_http_client *c,
			     struct aios_dir_lease *l)
{
	struct aios_dir_table dt;
	bool pd;
	u64 pts;
	int pdelta;
	int err;

	if (dir_table_init(&dt, info->volume, l->ino))
		return;
	mutex_lock(&l->flush_mu);
	for (;;) {
		struct aios_lease_op *op;

		mutex_lock(&l->mu);
		op = list_first_entry_or_null(&l->pending, struct aios_lease_op, node);
		if (op) {
			list_del(&op->node);
			l->npending--;
		}
		mutex_unlock(&l->mu);
		if (!op)
			break;
		err = dir_commit_sync(info, c, &dt, op->op, op->a0,
				      op->op == AIOS_HTTP_OP_UNLINK ? NULL : op->a1, false);
		if (err) {
			pr_warn("aiosfs: dir %llu: lost lease, op %u on \"%s\" not committed: %d\n",
				(unsigned long long)l->ino, op->op, op->a0, err);
			mutex_lock(&l->mu);
			if (!l->err)
				l->err = err;
			mutex_unlock(&l->mu);
		}
		kfree(op);
		wake_up_all(&l->wq);
	}
	mutex_lock(&l->mu);
	pd = l->parent_dirty;
	pts = l->parent_mtime_ns;
	pdelta = l->nlink_delta;
	l->parent_dirty = false;
	l->nlink_delta = 0;
	mutex_unlock(&l->mu);
	if (pd) {
		err = dir_lease_touch_parent(info, c, l->ino, pts, pdelta);
		if (err) {
			mutex_lock(&l->mu);
			if (!l->err)
				l->err = err;
			mutex_unlock(&l->mu);
		}
	}
	mutex_unlock(&l->flush_mu);
	dir_table_free(&dt);
	dir_cache_invalidate(info, l->ino);
	wake_up_all(&l->wq);
}

static void dir_lease_work(struct work_struct *w)
{
	struct aios_dir_lease *l = container_of(to_delayed_work(w), struct aios_dir_lease, work);
	struct aios_sb_info *info = l->info;
	struct aios_http_client *c = http_client_get(info);
	char token[128];
	bool held, lost = false;
	int err;

	mutex_lock(&l->mu);
	held = dir_lease_owns_locked(l);
	strscpy(token, l->token, sizeof(token));
	if (l->held && !held) {
		/* Expired without a successful renew: treat as lost. */
		l->held = false;
		lost = true;
	}
	mutex_unlock(&l->mu);

	if (held) {
		err = dir_lease_flush(info, c, l);
		if (err == -ESTALE) {
			/* Give the lock back in case we still have it (the conflict
			 * may have been on the log object), so the replay's own
			 * acquire does not have to break our lease. Only touch the
			 * lease if nobody re-acquired it meanwhile. */
			mutex_lock(&l->mu);
			if (l->held && !strcmp(token, l->token)) {
				l->held = false;
				l->break_requested = false;
				mutex_unlock(&l->mu);
				aios_http_lock_release(c, l->meta_oid, token);
				lost = true;
			} else {
				mutex_unlock(&l->mu);
			}
		}
	}
	if (lost) {
		dir_cache_invalidate(info, l->ino);
		dir_lease_replay(info, c, l);
	} else if (!held && (l->npending || l->parent_dirty)) {
		/* Queued after the lease went away (or never acquired). */
		dir_lease_replay(info, c, l);
	}

	mutex_lock(&l->mu);
	if (dir_lease_owns_locked(l)) {
		bool idle = !l->npending && !l->parent_dirty &&
			    time_after(jiffies, l->last_use + msecs_to_jiffies(AIOS_LEASE_IDLE_MS));

		if (l->release_wanted || l->break_requested || idle) {
			char token[128];

			/* Anything still queued (a flush just failed) is
			 * re-committed synchronously by the next run. */
			strscpy(token, l->token, sizeof(token));
			l->held = false;
			l->break_requested = false;
			mutex_unlock(&l->mu);
			aios_http_lock_release(c, l->meta_oid, token);
			mutex_lock(&l->mu);
		} else if (time_after_eq(jiffies, l->last_renew +
						  msecs_to_jiffies(AIOS_LEASE_RENEW_MS))) {
			char token[128];
			bool brk = false;

			strscpy(token, l->token, sizeof(token));
			mutex_unlock(&l->mu);
			err = aios_http_lock_renew(c, l->meta_oid, token, AIOS_LEASE_TTL_MS, &brk);
			mutex_lock(&l->mu);
			/* A re-acquire may have raced us; only touch our own lease. */
			if (!l->held || strcmp(token, l->token)) {
				/* nothing */
			} else if (err == -ESTALE) {
				l->held = false;
			} else if (!err) {
				l->last_renew = jiffies;
				if (brk) {
					/* The server shortened us to the grace period. */
					l->break_requested = true;
					l->expires = min(l->expires, jiffies +
						msecs_to_jiffies(AIOS_LEASE_BREAK_GRACE_MS));
				} else {
					l->expires = jiffies + msecs_to_jiffies(AIOS_LEASE_TTL_MS);
				}
			}
			/* Other errors: keep going on the current expiry. */
		}
	}
	if (l->held && (l->break_requested || l->release_wanted))
		mod_delayed_work(info->wb_wq, &l->work, 0);
	else if (l->npending || l->parent_dirty)
		mod_delayed_work(info->wb_wq, &l->work, msecs_to_jiffies(AIOS_LEASE_FLUSH_MS));
	else if (l->held)
		mod_delayed_work(info->wb_wq, &l->work, msecs_to_jiffies(AIOS_LEASE_RENEW_MS));
	mutex_unlock(&l->mu);
	wake_up_all(&l->wq);
	http_client_put(info, c);
}

/*
 * fsync(2) on a directory: everything queued for it must be on the server.
 * Returns and clears the sticky error of an earlier failed async commit.
 */
static int dir_lease_fsync(struct aios_sb_info *info, u64 ino)
{
	struct aios_dir_lease *l;
	int err;

	mutex_lock(&info->http_mu);
	l = dir_lease_find(info, ino);
	mutex_unlock(&info->http_mu);
	if (!l || !info->wb_wq)
		return 0;
	mod_delayed_work(info->wb_wq, &l->work, 0);
	wait_event(l->wq, dir_lease_flushed(l));
	mutex_lock(&l->mu);
	err = l->err;
	l->err = 0;
	mutex_unlock(&l->mu);
	return err;
}

/* syncfs / umount: flush every lease; @release also hands the locks back. */
static int dir_lease_sync_all(struct aios_sb_info *info, bool release)
{
	struct aios_dir_lease *l;
	int err = 0;

	if (!info->wb_wq)
		return 0;
	mutex_lock(&info->http_mu);
	list_for_each_entry(l, &info->leases, node) {
		int e;

		if (release) {
			dir_lease_drop(info, l->ino);
		} else {
			mod_delayed_work(info->wb_wq, &l->work, 0);
			wait_event(l->wq, dir_lease_flushed(l));
		}
		mutex_lock(&l->mu);
		e = l->err;
		l->err = 0;
		mutex_unlock(&l->mu);
		if (e && !err)
			err = e;
	}
	mutex_unlock(&info->http_mu);
	return err;
}

static void dir_lease_destroy_all(struct aios_sb_info *info)
{
	struct aios_dir_lease *l, *tmp;

	dir_lease_sync_all(info, true);
	list_for_each_entry_safe(l, tmp, &info->leases, node) {
		struct aios_lease_op *op, *otmp;

		cancel_delayed_work_sync(&l->work);
		list_del(&l->node);
		list_for_each_entry_safe(op, otmp, &l->pending, node)
			kfree(op);
		kfree(l);
	}
	info->nleases = 0;
}

static int dir_find(struct aios_dir_table *dt, const char *name, u64 *ino_out)
{
	unsigned int i;

	for (i = 0; i < dt->count; i++) {
		if (!strcmp(dt->ents[i].name, name)) {
			if (ino_out)
				*ino_out = dt->ents[i].ino;
			return 0;
		}
	}
	return -ENOENT;
}

static void inode_meta_reset(struct aios_inode_meta *m)
{
	if (!m)
		return;
	kfree(m->xattrs_obj);
	kfree(m->symlink);
	memset(m, 0, sizeof(*m));
}

static int json_get_quoted(const char *js, const char *key, char *out, size_t cap)
{
	char pat[64];
	const char *p;
	const char *end;
	int n;

	n = snprintf(pat, sizeof(pat), "\"%s\":", key);
	if (n < 0 || n >= (int)sizeof(pat))
		return -EINVAL;
	p = strstr(js, pat);
	if (!p)
		return -ENOENT;
	p += n;
	while (*p == ' ' || *p == '\t')
		p++;
	end = js + strlen(js);
	return parse_json_quoted(&p, end, out, cap);
}

static int inode_load_extras(const char *js, struct aios_inode_meta *m)
{
	char *tmp;

	kfree(m->xattrs_obj);
	kfree(m->symlink);
	m->xattrs_obj = extract_xattrs_object(js);
	m->symlink = NULL;
	tmp = kmalloc(AIOS_KABI_SYMLINK_MAX + 1, GFP_KERNEL);
	if (!tmp) {
		m->extras_loaded = false;
		return -ENOMEM;
	}
	if (json_get_quoted(js, "symlink", tmp, AIOS_KABI_SYMLINK_MAX + 1) == 0 && tmp[0])
		m->symlink = kstrdup(tmp, GFP_KERNEL);
	kfree(tmp);
	m->extras_loaded = true;
	return 0;
}

static int inode_from_json(const char *js, u64 cas, struct aios_inode_meta *m)
{
	inode_meta_reset(m);
	m->exists = true;
	m->cas = cas;
	m->nlink = 1;
	m->stripe_unit = AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	m->stripe_width = AIOS_HTTP_DEFAULT_STRIPE_WIDTH;
	json_get_u64(js, "ino", &m->ino);
	json_get_u32(js, "mode", &m->mode);
	json_get_u32(js, "nlink", &m->nlink);
	json_get_u32(js, "uid", &m->uid);
	json_get_u32(js, "gid", &m->gid);
	json_get_u64(js, "size", &m->size);
	json_get_u64(js, "atime_ns", &m->atime_ns);
	json_get_u64(js, "mtime_ns", &m->mtime_ns);
	json_get_u64(js, "ctime_ns", &m->ctime_ns);
	json_get_u64(js, "stripe_unit", &m->stripe_unit);
	json_get_u32(js, "stripe_width", &m->stripe_width);
	return 0;
}

static int inode_to_json(const struct aios_inode_meta *m, char *buf, size_t n)
{
	int w = snprintf(buf, n,
			 "{\"aios_posix_ino\":1,\"ino\":%llu,\"mode\":%u,\"nlink\":%u,"
			 "\"uid\":%u,\"gid\":%u,\"size\":%llu,\"atime_ns\":%llu,"
			 "\"mtime_ns\":%llu,\"ctime_ns\":%llu,\"stripe_unit\":%llu,"
			 "\"stripe_width\":%u}",
			 (unsigned long long)m->ino, m->mode, m->nlink, m->uid, m->gid,
			 (unsigned long long)m->size, (unsigned long long)m->atime_ns,
			 (unsigned long long)m->mtime_ns, (unsigned long long)m->ctime_ns,
			 (unsigned long long)m->stripe_unit, m->stripe_width);
	return (w < 0 || (size_t)w >= n) ? -EOVERFLOW : 0;
}

/* Extract `"xattrs":{...}` object body (including braces). Caller kfree. */
static char *extract_xattrs_object(const char *js)
{
	const char *p = strstr(js, "\"xattrs\"");
	const char *start;
	const char *q;
	int depth;
	size_t len;
	char *out;

	if (!p)
		return NULL;
	p = strchr(p + 8, '{');
	if (!p)
		return NULL;
	start = p;
	depth = 0;
	for (q = p; *q; q++) {
		if (*q == '{')
			depth++;
		else if (*q == '}') {
			depth--;
			if (depth == 0) {
				q++;
				break;
			}
		}
	}
	if (depth != 0)
		return NULL;
	len = q - start;
	out = kmalloc(len + 1, GFP_KERNEL);
	if (!out)
		return NULL;
	memcpy(out, start, len);
	out[len] = '\0';
	return out;
}

/* Merge base inode JSON (no xattrs) with optional xattrs object "{...}". */
static int inode_json_merge_xattrs(const char *base, const char *xattrs_obj, char **out,
				  size_t *out_len)
{
	size_t blen = strlen(base);
	size_t xlen = xattrs_obj ? strlen(xattrs_obj) : 0;
	char *full;
	size_t n;

	if (blen < 2 || base[blen - 1] != '}')
		return -EINVAL;
	if (!xattrs_obj || xattrs_obj[0] != '{') {
		full = kmalloc(blen + 1, GFP_KERNEL);
		if (!full)
			return -ENOMEM;
		memcpy(full, base, blen);
		full[blen] = '\0';
		*out = full;
		if (out_len)
			*out_len = blen;
		return 0;
	}
	n = blen + xlen + sizeof(",\"xattrs\":");
	full = kmalloc(n, GFP_KERNEL);
	if (!full)
		return -ENOMEM;
	memcpy(full, base, blen - 1);
	scnprintf(full + blen - 1, n - (blen - 1), ",\"xattrs\":%s}", xattrs_obj);
	*out = full;
	if (out_len)
		*out_len = strlen(full);
	return 0;
}

static const char b64_tbl[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

static int base64_encode(const u8 *data, size_t len, char **out, size_t *out_len)
{
	size_t n = ((len + 2) / 3) * 4;
	char *s;
	size_t i, o = 0;

	s = kmalloc(n + 1, GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	for (i = 0; i < len; i += 3) {
		u32 v = ((u32)data[i] << 16) | ((i + 1 < len ? data[i + 1] : 0) << 8) |
			(i + 2 < len ? data[i + 2] : 0);

		s[o++] = b64_tbl[(v >> 18) & 63];
		s[o++] = b64_tbl[(v >> 12) & 63];
		s[o++] = (i + 1 < len) ? b64_tbl[(v >> 6) & 63] : '=';
		s[o++] = (i + 2 < len) ? b64_tbl[v & 63] : '=';
	}
	s[o] = '\0';
	*out = s;
	if (out_len)
		*out_len = o;
	return 0;
}

static int base64_decode(const char *in, size_t in_len, u8 **out, size_t *out_len)
{
	u8 *buf;
	size_t i, o = 0;

	if (in_len % 4)
		return -EINVAL;
	buf = kmalloc(in_len / 4 * 3 + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	for (i = 0; i < in_len; i += 4) {
		int a = b64_val(in[i]);
		int b = b64_val(in[i + 1]);
		int c = in[i + 2] == '=' ? -2 : b64_val(in[i + 2]);
		int d = in[i + 3] == '=' ? -2 : b64_val(in[i + 3]);

		if (a < 0 || b < 0 || c == -1 || d == -1) {
			kfree(buf);
			return -EINVAL;
		}
		if (c == -2 && d != -2) {
			kfree(buf);
			return -EINVAL;
		}
		buf[o++] = (u8)((a << 2) | (b >> 4));
		if (c >= 0)
			buf[o++] = (u8)(((b & 15) << 4) | (c >> 2));
		if (d >= 0)
			buf[o++] = (u8)(((c & 3) << 6) | d);
	}
	*out = buf;
	if (out_len)
		*out_len = o;
	return 0;
}

struct aios_xa_ent {
	char name[AIOS_KABI_NAME_MAX + 1];
	u8 *value;
	size_t value_len;
};

static void free_xa_ents(struct aios_xa_ent *ents, unsigned int n)
{
	unsigned int i;

	if (!ents)
		return;
	for (i = 0; i < n; i++)
		kfree(ents[i].value);
	kfree(ents);
}

/* Parse xattrs object into heap array. */
static int parse_xattrs_object(const char *obj, struct aios_xa_ent **ents_out, unsigned int *n_out)
{
	struct aios_xa_ent *ents;
	const char *p, *end;
	unsigned int n = 0;
	/* Base64 of a 64 KiB value; far too large for the kernel stack. */
	const size_t b64_cap = AIOS_HTTP_MAX_XATTR_VALUE * 2 + 8;
	char *b64;
	int err = 0;

	*ents_out = NULL;
	*n_out = 0;
	if (!obj || obj[0] != '{')
		return 0;
	ents = kcalloc(AIOS_HTTP_MAX_XATTRS, sizeof(*ents), GFP_KERNEL);
	if (!ents)
		return -ENOMEM;
	b64 = kvmalloc(b64_cap, GFP_KERNEL);
	if (!b64) {
		kfree(ents);
		return -ENOMEM;
	}
	p = obj + 1;
	end = obj + strlen(obj);
	while (p < end && *p) {
		u8 *raw = NULL;
		size_t raw_len = 0;

		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ','))
			p++;
		if (p >= end || *p == '}')
			break;
		if (*p != '"' || n >= AIOS_HTTP_MAX_XATTRS) {
			err = n >= AIOS_HTTP_MAX_XATTRS ? -ENOSPC : -EINVAL;
			goto out_err;
		}
		err = parse_json_quoted(&p, end, ents[n].name, sizeof(ents[n].name));
		if (err)
			goto out_err;
		while (p < end && (*p == ' ' || *p == '\t' || *p == ':'))
			p++;
		if (p >= end || *p != '"') {
			err = -EINVAL;
			goto out_err;
		}
		err = parse_json_quoted(&p, end, b64, b64_cap);
		if (err)
			goto out_err;
		err = base64_decode(b64, strlen(b64), &raw, &raw_len);
		if (err)
			goto out_err;
		ents[n].value = raw;
		ents[n].value_len = raw_len;
		n++;
	}
	kvfree(b64);
	*ents_out = ents;
	*n_out = n;
	return 0;

out_err:
	kvfree(b64);
	free_xa_ents(ents, n);
	return err;
}

static int build_xattrs_object(struct aios_xa_ent *ents, unsigned int n, char **out)
{
	size_t cap = 2;
	char *s;
	char *ename;
	size_t pos;
	unsigned int i;

	for (i = 0; i < n; i++)
		cap += (strlen(ents[i].name) * 6) + ((ents[i].value_len + 2) / 3) * 4 + 8;
	s = kmalloc(cap + 16, GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	ename = kmalloc(AIOS_KABI_NAME_MAX * 6 + 8, GFP_KERNEL);
	if (!ename) {
		kfree(s);
		return -ENOMEM;
	}
	pos = scnprintf(s, cap + 16, "{");
	for (i = 0; i < n; i++) {
		size_t epos = 0;
		char *b64 = NULL;
		size_t blen = 0;
		int err;

		if (i)
			pos += scnprintf(s + pos, cap + 16 - pos, ",");
		pos += scnprintf(s + pos, cap + 16 - pos, "\"");
		err = json_escape_append(ename, AIOS_KABI_NAME_MAX * 6 + 8, &epos, ents[i].name);
		if (err) {
			kfree(ename);
			kfree(s);
			return err;
		}
		ename[epos] = '\0';
		err = base64_encode(ents[i].value, ents[i].value_len, &b64, &blen);
		if (err) {
			kfree(ename);
			kfree(s);
			return err;
		}
		pos += scnprintf(s + pos, cap + 16 - pos, "%s\":\"%s\"", ename, b64);
		kfree(b64);
	}
	scnprintf(s + pos, cap + 16 - pos, "}");
	kfree(ename);
	*out = s;
	return 0;
}

static int inode_json_merge_symlink(const char *base, const char *target, char **out)
{
	char *esc;
	size_t epos = 0;
	size_t blen = strlen(base);
	size_t esc_cap = AIOS_KABI_SYMLINK_MAX * 6 + 8;
	size_t n;
	char *s;
	int err;

	if (!target || !target[0]) {
		s = kstrdup(base, GFP_KERNEL);
		if (!s)
			return -ENOMEM;
		*out = s;
		return 0;
	}
	if (blen < 2 || base[blen - 1] != '}')
		return -EINVAL;
	esc = kmalloc(esc_cap, GFP_KERNEL);
	if (!esc)
		return -ENOMEM;
	err = json_escape_append(esc, esc_cap, &epos, target);
	if (err) {
		kfree(esc);
		return err;
	}
	esc[epos] = '\0';
	n = blen + epos + sizeof(",\"symlink\":\"\"}");
	s = kmalloc(n, GFP_KERNEL);
	if (!s) {
		kfree(esc);
		return -ENOMEM;
	}
	memcpy(s, base, blen - 1);
	scnprintf(s + blen - 1, n - (blen - 1), ",\"symlink\":\"%s\"}", esc);
	kfree(esc);
	*out = s;
	return 0;
}

/* Build full inode JSON. Uses in-memory xattrs/symlink when extras_loaded. */
static int inode_to_json_full(struct aios_sb_info *info, struct aios_inode_meta *m, char **out,
			      size_t *out_len)
{
	char base[512];
	char *with_sym = NULL;
	char *xattrs = NULL;
	bool xattrs_owned = false;
	char *js = NULL;
	int err;

	err = inode_to_json(m, base, sizeof(base));
	if (err)
		return err;
	err = inode_json_merge_symlink(base, m->symlink, &with_sym);
	if (err)
		return err;
	if (m->extras_loaded) {
		xattrs = m->xattrs_obj;
	} else {
		char oid[160];
		struct aios_http_buf body = { 0 };

		oid_ino(info->volume, m->ino, oid, sizeof(oid));
		err = aios_http_get(info->http, oid, &body, NULL);
		if (!err && body.len) {
			char *tmp = kmalloc(body.len + 1, GFP_KERNEL);

			if (!tmp) {
				aios_http_buf_free(&body);
				kfree(with_sym);
				return -ENOMEM;
			}
			memcpy(tmp, body.data, body.len);
			tmp[body.len] = '\0';
			xattrs = extract_xattrs_object(tmp);
			xattrs_owned = true;
			kfree(tmp);
		} else if (err == -ENOENT) {
			err = 0;
		}
		aios_http_buf_free(&body);
		if (err) {
			kfree(with_sym);
			return err;
		}
	}
	err = inode_json_merge_xattrs(with_sym, xattrs, &js, out_len);
	if (xattrs_owned)
		kfree(xattrs);
	kfree(with_sym);
	if (err)
		return err;
	*out = js;
	return 0;
}

/* GET the inode object on an explicit client (pool client on the data path). */
static int load_inode_c(struct aios_sb_info *info, struct aios_http_client *c, u64 ino,
			struct aios_inode_meta *m)
{
	char oid[160];
	struct aios_http_buf body = { 0 };
	char *js;
	u64 cas = 0;
	int err;

	oid_ino(info->volume, ino, oid, sizeof(oid));
	err = aios_http_get(c, oid, &body, &cas);
	if (err == -ENOENT) {
		inode_meta_reset(m);
		return -ENOENT;
	}
	if (err)
		return err;
	js = kmalloc(body.len + 1, GFP_KERNEL);
	if (!js) {
		aios_http_buf_free(&body);
		return -ENOMEM;
	}
	memcpy(js, body.data, body.len);
	js[body.len] = '\0';
	inode_from_json(js, cas, m);
	inode_load_extras(js, m);
	kfree(js);
	aios_http_buf_free(&body);
	return 0;
}

static int load_inode(struct aios_sb_info *info, u64 ino, struct aios_inode_meta *m)
{
	return load_inode_c(info, info->http, ino, m);
}

/* CAS PUT of the inode object on an explicit client. m must have extras
 * loaded (inode_to_json_full would otherwise GET on info->http). */
static int store_inode_c(struct aios_sb_info *info, struct aios_http_client *c,
			 struct aios_inode_meta *m)
{
	char oid[160];
	char *js = NULL;
	size_t jslen = 0;
	int err;

	err = inode_to_json_full(info, m, &js, &jslen);
	if (err)
		return err;
	oid_ino(info->volume, m->ino, oid, sizeof(oid));
	err = aios_http_put(c, oid, js, jslen, NULL, &m->cas);
	kfree(js);
	return err;
}

static int store_inode(struct aios_sb_info *info, struct aios_inode_meta *m)
{
	return store_inode_c(info, info->http, m);
}

static void meta_to_stat(const struct aios_inode_meta *m, struct aios_kabi_stat *st)
{
	memset(st, 0, sizeof(*st));
	st->ino = m->ino;
	st->mode = m->mode;
	st->nlink = m->nlink;
	st->uid = m->uid;
	st->gid = m->gid;
	st->size = m->size;
	st->atime_ns = m->atime_ns;
	st->mtime_ns = m->mtime_ns;
	st->ctime_ns = m->ctime_ns;
}

static void attach_iinfo(struct inode *inode, const struct aios_inode_meta *m)
{
	struct aios_inode_aux *ii;
	bool fresh = false;

	ii = aios_inode_aux_get(inode, &fresh);
	if (!ii)
		return;
	ii->cas = m->cas;
	ii->stripe_unit = m->stripe_unit;
	ii->stripe_width = m->stripe_width;
	ii->meta_jiffies = jiffies;
	if (fresh)
		ii->last_synced_size = m->size;
	if (m->extras_loaded) {
		char *new_x = NULL;
		char *new_s = NULL;
		char *old_x;
		char *old_s;

		/* Allocate outside the spinlock; swap under it; free after.
		 * getattr can run here with no inode lock, racing get_link and
		 * other attach_iinfo callers. */
		if (m->xattrs_obj) {
			new_x = kstrdup(m->xattrs_obj, GFP_KERNEL);
			if (!new_x)
				return;
		}
		if (m->symlink) {
			new_s = kstrdup(m->symlink, GFP_KERNEL);
			if (!new_s) {
				kfree(new_x);
				return;
			}
		}
		spin_lock(&ii->extras_lock);
		old_x = ii->xattrs_obj;
		old_s = ii->symlink;
		ii->xattrs_obj = new_x;
		ii->symlink = new_s;
		ii->extras_valid = true;
		spin_unlock(&ii->extras_lock);
		kfree(old_x);
		kfree(old_s);
	}
}

/*
 * Copy one of the aux extras (symlink target or xattrs object) under
 * extras_lock. *valid_out reports whether the extras were loaded at all (and,
 * when max_age is non-zero, loaded within that many jiffies); the string is
 * NULL when the field is unset or on ENOMEM.
 */
static char *aux_extra_dup(struct aios_inode_aux *aux, bool want_symlink, unsigned long max_age,
			   bool *valid_out)
{
	char *s = NULL;
	size_t cap = 0;

	if (valid_out)
		*valid_out = false;
	for (;;) {
		const char *src;
		size_t len;

		spin_lock(&aux->extras_lock);
		if (!aux->extras_valid ||
		    (max_age && !time_before(jiffies, aux->meta_jiffies + max_age))) {
			spin_unlock(&aux->extras_lock);
			kfree(s);
			return NULL;
		}
		if (valid_out)
			*valid_out = true;
		src = want_symlink ? aux->symlink : aux->xattrs_obj;
		if (!src) {
			spin_unlock(&aux->extras_lock);
			kfree(s);
			return NULL;
		}
		len = strlen(src);
		if (s && len < cap) {
			memcpy(s, src, len + 1);
			spin_unlock(&aux->extras_lock);
			return s;
		}
		spin_unlock(&aux->extras_lock);
		kfree(s);
		cap = len + 1;
		s = kmalloc(cap, GFP_KERNEL);
		if (!s) {
			/* Not "no xattrs": make the caller take the slow path. */
			if (valid_out)
				*valid_out = false;
			return NULL;
		}
	}
}

/* Copy aux->symlink under extras_lock. Returns NULL if unset or on ENOMEM. */
static char *aux_symlink_dup(struct aios_inode_aux *aux)
{
	return aux_extra_dup(aux, true, 0, NULL);
}

static struct inode *aios_http_iget(struct super_block *sb, const struct aios_inode_meta *m)
{
	struct inode *inode;
	struct aios_kabi_stat st;

	meta_to_stat(m, &st);
	inode = iget_locked(sb, m->ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW)) {
		aios_stat_to_inode(inode, &st);
		attach_iinfo(inode, m);
		return inode;
	}
	aios_stat_to_inode(inode, &st);
	attach_iinfo(inode, m);
	if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &aios_http_dir_inode_ops;
		inode->i_fop = &aios_http_dir_ops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &aios_http_symlink_inode_ops;
	} else {
		inode->i_op = &aios_http_file_inode_ops;
		aios_setup_file_inode(inode);
	}
	unlock_new_inode(inode);
	return inode;
}

static int ensure_super(struct aios_sb_info *info)
{
	char oid[160];
	struct aios_http_buf body = { 0 };
	u64 cas = 0;
	int err;

	oid_super(info->volume, oid, sizeof(oid));
	err = aios_http_get(info->http, oid, &body, &cas);
	if (!err) {
		aios_http_buf_free(&body);
		return 0;
	}
	if (err != -ENOENT)
		return err;
	{
		char js[256];
		u64 stripe = info->stripe_unit ? info->stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
		u32 width = info->stripe_width ? info->stripe_width : AIOS_HTTP_DEFAULT_STRIPE_WIDTH;

		snprintf(js, sizeof(js),
			 "{\"aios_posix_super\":1,\"next_ino\":2,\"stripe_unit\":%llu,"
			 "\"stripe_width\":%u,\"uuid\":\"fs-%s\"}",
			 (unsigned long long)stripe, width, info->volume);
		cas = 0;
		err = aios_http_put(info->http, oid, js, strlen(js), NULL, &cas);
	}
	return err;
}

/*
 * Reserve AIOSFS_INO_BATCH inode numbers with one CAS on the super object.
 * Numbers left unused at unmount are simply skipped; inode numbers only need
 * to be unique, not dense, and this turns two round trips per create into
 * two per 256 creates. The same field is what libaios_posix's alloc_ino
 * advances one at a time, so both allocators stay disjoint.
 */
static int alloc_ino_range(struct aios_sb_info *info)
{
	char oid[160];
	int attempt;
	int err = -EAGAIN;

	oid_super(info->volume, oid, sizeof(oid));
	for (attempt = 0; attempt < 16; attempt++) {
		struct aios_http_buf body = { 0 };
		char *js;
		char out[256];
		u64 cas = 0, next = 2, stripe = AIOS_HTTP_DEFAULT_STRIPE_UNIT;
		u32 width = AIOS_HTTP_DEFAULT_STRIPE_WIDTH;

		err = aios_http_get(info->http, oid, &body, &cas);
		if (err)
			return err;
		js = kmalloc(body.len + 1, GFP_KERNEL);
		if (!js) {
			aios_http_buf_free(&body);
			return -ENOMEM;
		}
		memcpy(js, body.data, body.len);
		js[body.len] = '\0';
		json_get_u64(js, "next_ino", &next);
		json_get_u64(js, "stripe_unit", &stripe);
		json_get_u32(js, "stripe_width", &width);
		kfree(js);
		aios_http_buf_free(&body);

		snprintf(out, sizeof(out),
			 "{\"aios_posix_super\":1,\"next_ino\":%llu,\"stripe_unit\":%llu,"
			 "\"stripe_width\":%u,\"uuid\":\"fs-%s\"}",
			 (unsigned long long)(next + AIOSFS_INO_BATCH), (unsigned long long)stripe,
			 width, info->volume);
		err = aios_http_put(info->http, oid, out, strlen(out), NULL, &cas);
		if (!err) {
			info->ino_next = next;
			info->ino_end = next + AIOSFS_INO_BATCH;
			return 0;
		}
		if (err != -EAGAIN)
			return err;
		http_retry_backoff(attempt);
	}
	return http_no_eagain(err);
}

static int alloc_ino(struct aios_sb_info *info, u64 *ino_out)
{
	int err = 0;

	mutex_lock(&info->ino_mu);
	if (info->ino_next >= info->ino_end)
		err = alloc_ino_range(info);
	if (!err)
		*ino_out = info->ino_next++;
	mutex_unlock(&info->ino_mu);
	return err;
}

static int ensure_root(struct aios_sb_info *info)
{
	struct aios_inode_meta root = { 0 };
	int err;

	err = load_inode(info, AIOS_HTTP_ROOT_INO, &root);
	if (!err)
		return 0;
	if (err != -ENOENT)
		return err;
	{
		u64 ts = now_ns();

		memset(&root, 0, sizeof(root));
		root.ino = AIOS_HTTP_ROOT_INO;
		root.mode = S_IFDIR | 0755;
		root.nlink = 2;
		root.uid = info->uid;
		root.gid = info->gid;
		root.atime_ns = root.mtime_ns = root.ctime_ns = ts;
		root.stripe_unit =
			info->stripe_unit ? info->stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
		root.stripe_width =
			info->stripe_width ? info->stripe_width : AIOS_HTTP_DEFAULT_STRIPE_WIDTH;
		root.cas = 0;
		root.extras_loaded = true;
		err = store_inode(info, &root);
	}
	return err;
}

/*
 * Update a directory inode's mtime/ctime (and nlink by @nlink_delta) after a
 * namespace change. @pm is the caller's already-loaded copy; a CAS conflict
 * means a peer touched the directory meanwhile, so reload and apply again
 * rather than lose a subdirectory link count.
 */
static int touch_parent(struct aios_sb_info *info, struct aios_inode_meta *pm, u64 ts,
			int nlink_delta)
{
	int attempt;
	int err = -EAGAIN;

	for (attempt = 0; attempt < AIOS_HTTP_META_RETRIES && err == -EAGAIN; attempt++) {
		if (attempt) {
			http_retry_backoff(attempt);
			err = load_inode(info, pm->ino, pm);
			if (err)
				break;
		}
		if (nlink_delta < 0 && pm->nlink > 2)
			pm->nlink -= 1;
		else if (nlink_delta > 0)
			pm->nlink += 1;
		pm->mtime_ns = pm->ctime_ns = ts;
		err = store_inode(info, pm);
	}
	return http_no_eagain(err);
}

static void delete_dir_objects(struct aios_sb_info *info, u64 ino)
{
	char oid[160];

	oid_ino(info->volume, ino, oid, sizeof(oid));
	aios_http_delete(info->http, oid);
	oid_dir_meta(info->volume, ino, oid, sizeof(oid));
	aios_http_delete(info->http, oid);
	oid_dir_log(info->volume, ino, oid, sizeof(oid));
	aios_http_delete(info->http, oid);
	oid_dir_snap(info->volume, ino, oid, sizeof(oid));
	aios_http_delete(info->http, oid);
}

/*
 * Chunk deletion runs on wb_wq, striped over up to pool_size workers, so
 * unlink+close and truncate return after the metadata update instead of after
 * one DELETE round trip per chunk. Object deletion is idempotent, so a worker
 * that dies with the mount simply leaves chunks for a later sweep; put_super
 * drains the queue before the pool goes away.
 */
struct aios_chunk_del_work {
	struct work_struct work;
	struct aios_sb_info *info;
	u64 ino;
	u64 first;
	u64 end;
	u64 stride;
};

static void delete_chunk_stripe(struct aios_sb_info *info, struct aios_http_client *c, u64 ino,
				u64 first, u64 end, u64 stride)
{
	char oid[160];
	u64 i;

	for (i = first; i < end; i += stride) {
		oid_chunk(info->volume, ino, i, oid, sizeof(oid));
		aios_http_delete(c, oid);
	}
}

static void chunk_del_worker(struct work_struct *w)
{
	struct aios_chunk_del_work *dw = container_of(w, struct aios_chunk_del_work, work);
	struct aios_http_client *c = http_client_get(dw->info);

	delete_chunk_stripe(dw->info, c, dw->ino, dw->first, dw->end, dw->stride);
	http_client_put(dw->info, c);
	kfree(dw);
}

/* Delete chunk objects [first, end) of @ino. @c is used for the synchronous
 * fallback when a work item cannot be allocated. */
static void delete_chunks_deferred(struct aios_sb_info *info, struct aios_http_client *c,
				   u64 ino, u64 first, u64 end)
{
	u64 n = end > first ? end - first : 0;
	unsigned int workers, w;

	if (!n)
		return;
	if (!info->wb_wq) {
		delete_chunk_stripe(info, c, ino, first, end, 1);
		return;
	}
	workers = (unsigned int)min_t(u64, info->pool_size, (n + 15) / 16);
	if (!workers)
		workers = 1;
	for (w = 0; w < workers; w++) {
		struct aios_chunk_del_work *dw = kmalloc(sizeof(*dw), GFP_KERNEL);

		if (!dw) {
			delete_chunk_stripe(info, c, ino, first + w, end, workers);
			continue;
		}
		INIT_WORK(&dw->work, chunk_del_worker);
		dw->info = info;
		dw->ino = ino;
		dw->first = first + w;
		dw->end = end;
		dw->stride = workers;
		queue_work(info->wb_wq, &dw->work);
	}
}

/* Delete every chunk object of a regular file up to @size bytes. */
static void delete_file_chunks(struct aios_sb_info *info, struct aios_http_client *c, u64 ino,
			       u64 unit, u64 size)
{
	if (!unit)
		unit = AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	delete_chunks_deferred(info, c, ino, 0, (size + unit - 1) / unit);
}

/*
 * Drop one link of a non-directory. The data and the inode object are kept
 * even when nlink reaches 0: an open file descriptor may still be reading or
 * writing it. The VFS drops the in-core nlink and, once the last reference
 * goes away, evict_inode → aios_http_evict_unlinked removes the objects.
 * Directories cannot be held open for data, so an (empty) directory victim
 * is removed immediately.
 */
static int aios_http_drop_link(struct aios_sb_info *info, u64 ino)
{
	struct aios_inode_meta m = { 0 };
	int attempt;
	int err = -EAGAIN;

	/* The inode object is also written by size flushes and setattr on
	 * pool clients; a CAS conflict just means reload and apply again. */
	for (attempt = 0; attempt < AIOS_HTTP_META_RETRIES && err == -EAGAIN; attempt++) {
		if (attempt)
			http_retry_backoff(attempt);
		err = load_inode(info, ino, &m);
		if (err == -ENOENT) {
			err = 0;
			break;
		}
		if (err)
			break;
		if (S_ISDIR(m.mode)) {
			delete_dir_objects(info, ino);
			err = 0;
			break;
		}
		m.nlink = m.nlink ? m.nlink - 1 : 0;
		m.ctime_ns = now_ns();
		err = store_inode(info, &m);
	}
	inode_meta_reset(&m);
	return http_no_eagain(err);
}

void aios_http_evict_unlinked(struct inode *inode)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m = { 0 };
	struct aios_inode_aux *aux = inode->i_private;
	char oid[160];
	u64 size;
	u64 unit;
	int err;

	struct aios_http_client *c;

	if (!info || !info->http_pool)
		return;
	c = http_client_get(info);
	err = load_inode_c(info, c, inode->i_ino, &m);
	if (err && err != -ENOENT)
		goto out;
	if (!err && m.nlink > 0) {
		/* Re-linked (or nlink drift) on the server: keep it. */
		goto out;
	}
	size = (u64)i_size_read(inode);
	unit = aux && aux->stripe_unit ? aux->stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	if (!err) {
		size = max_t(u64, size, m.size);
		if (m.stripe_unit)
			unit = m.stripe_unit;
	}
	if (aux && aux->last_synced_size > size)
		size = aux->last_synced_size;
	/* Inode object first (one round trip), chunks in the background. */
	oid_ino(info->volume, inode->i_ino, oid, sizeof(oid));
	aios_http_delete(c, oid);
	if (S_ISREG(inode->i_mode))
		delete_file_chunks(info, c, inode->i_ino, unit, size);
out:
	inode_meta_reset(&m);
	http_client_put(info, c);
}

/*
 * Bring the in-core inode up to date from the server. When the cached CAS is
 * known, a HEAD first compares the server's aios.posix.cas with it: unchanged
 * means nothing to fetch (headers only), so a stat() storm past the TTL costs
 * a HEAD per TTL instead of a full inode GET + JSON parse. Uses a pool client
 * and no http_mu, so it never queues behind a namespace operation.
 */
static int http_refresh(struct inode *inode)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_aux *aux = inode->i_private;
	struct aios_inode_meta m = { 0 };
	struct aios_kabi_stat st;
	struct aios_http_client *c;
	int err;

	c = http_client_get(info);
	if (aux && aux->cas) {
		char oid[160];
		u64 size = 0, cas = 0;

		oid_ino(info->volume, inode->i_ino, oid, sizeof(oid));
		err = aios_http_head(c, oid, &size, &cas);
		if (!err && cas && cas == aux->cas) {
			aux->meta_jiffies = jiffies;
			http_client_put(info, c);
			return 0;
		}
		/* Changed, missing or HEAD failed: the GET below is authoritative. */
	}
	err = load_inode_c(info, c, inode->i_ino, &m);
	http_client_put(info, c);
	if (err)
		return err;
	meta_to_stat(&m, &st);
	aios_stat_to_inode(inode, &st);
	attach_iinfo(inode, &m);
	inode_meta_reset(&m);
	return 0;
}

static struct dentry *http_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_dir_table dt;
	struct aios_inode_meta m = { 0 };
	struct inode *inode = NULL;
	char name[AIOS_KABI_NAME_MAX + 1];
	u64 child;
	int err;

	if (dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	memcpy(name, dentry->d_name.name, dentry->d_name.len);
	name[dentry->d_name.len] = '\0';

	mutex_lock(&info->http_mu);
	err = dir_table_init(&dt, info->volume, dir->i_ino);
	if (err)
		goto out;
	err = dir_load(info, &dt, true);
	if (err)
		goto out_dt;
	err = dir_find(&dt, name, &child);
	if (err) {
		d_add(dentry, NULL);
		aios_d_mark_fresh(dentry);
		err = 0;
		inode = NULL;
		goto out_dt;
	}
	err = load_inode(info, child, &m);
	if (err)
		goto out_dt;
	inode = aios_http_iget(dir->i_sb, &m);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_dt;
	}
out_dt:
	dir_table_free(&dt);
	inode_meta_reset(&m);
out:
	mutex_unlock(&info->http_mu);
	if (err)
		return ERR_PTR(err);
	if (!inode)
		return NULL;
	{
		struct dentry *res = d_splice_alias(inode, dentry);

		aios_d_mark_fresh(dentry);
		if (res && !IS_ERR(res))
			aios_d_mark_fresh(res);
		return res;
	}
}

static int http_create_common(struct inode *dir, struct dentry *dentry, umode_t mode, bool is_dir)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_dir_table dt;
	struct aios_inode_meta pmeta = { 0 }, m = { 0 };
	struct inode *inode;
	char name[AIOS_KABI_NAME_MAX + 1];
	char inos[32];
	u64 ino, ts;
	bool leased;
	int err;

	if (dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(name, dentry->d_name.name, dentry->d_name.len);
	name[dentry->d_name.len] = '\0';
	if (strchr(name, '/'))
		return -EINVAL;

	mutex_lock(&info->http_mu);
	/* Under a lease the directory is ours: the in-core inode and the cached
	 * table are authoritative and the parent update is deferred, so a
	 * create costs the child inode PUT and nothing else synchronous. */
	leased = !!dir_lease_get(info, dir->i_ino);
	if (!leased) {
		err = load_inode(info, dir->i_ino, &pmeta);
		if (err)
			goto out;
		if (!S_ISDIR(pmeta.mode)) {
			err = -ENOTDIR;
			goto out;
		}
	}
	err = dir_table_init(&dt, info->volume, dir->i_ino);
	if (err)
		goto out;
	err = dir_load(info, &dt, false);
	if (err)
		goto out_dt;
	if (!dir_find(&dt, name, NULL)) {
		err = -EEXIST;
		goto out_dt;
	}
	err = alloc_ino(info, &ino);
	if (err)
		goto out_dt;
	ts = now_ns();
	memset(&m, 0, sizeof(m));
	m.ino = ino;
	m.mode = is_dir ? (S_IFDIR | (mode & 0777)) : (S_IFREG | (mode & 0777));
	m.nlink = is_dir ? 2 : 1;
	m.uid = from_kuid(&init_user_ns, current_fsuid());
	m.gid = from_kgid(&init_user_ns, current_fsgid());
	m.atime_ns = m.mtime_ns = m.ctime_ns = ts;
	m.stripe_unit = info->stripe_unit ? info->stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	m.stripe_width = info->stripe_width ? info->stripe_width : AIOS_HTTP_DEFAULT_STRIPE_WIDTH;
	m.cas = 0;
	m.extras_loaded = true;
	err = store_inode(info, &m);
	if (err)
		goto out_dt;
	snprintf(inos, sizeof(inos), "%llu", (unsigned long long)ino);
	err = dir_commit_op(info, &dt, AIOS_HTTP_OP_LINK, name, inos, true);
	if (err) {
		char oid[160];

		/* The child inode object was created above; do not leak it. */
		oid_ino(info->volume, ino, oid, sizeof(oid));
		aios_http_delete(info->http, oid);
		goto out_dt;
	}
	err = touch_dir_inode(info, dir, &pmeta, ts, is_dir ? 1 : 0);
	if (err)
		goto out_dt;
	inode = aios_http_iget(dir->i_sb, &m);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_dt;
	}
	d_instantiate(dentry, inode);
	aios_d_mark_fresh(dentry);
out_dt:
	dir_table_free(&dt);
out:
	inode_meta_reset(&pmeta);
	inode_meta_reset(&m);
	mutex_unlock(&info->http_mu);
	return err;
}

static int http_create(AIOS_IDMAP *mnt_userns, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl)
{
	return http_create_common(dir, dentry, mode, false);
}

static int http_mkdir(AIOS_IDMAP *mnt_userns, struct inode *dir,
		      struct dentry *dentry, umode_t mode)
{
	return http_create_common(dir, dentry, mode, true);
}

static int http_unlink(struct inode *dir, struct dentry *dentry)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_dir_table dt;
	char name[AIOS_KABI_NAME_MAX + 1];
	u64 child;
	int err;

	if (dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(name, dentry->d_name.name, dentry->d_name.len);
	name[dentry->d_name.len] = '\0';

	mutex_lock(&info->http_mu);
	err = dir_table_init(&dt, info->volume, dir->i_ino);
	if (err)
		goto out;
	/* The cached table is enough to find the child: dir_commit_op reloads
	 * under the directory lock and fails with -ENOENT if the name is gone.
	 * The VFS already rejected directories (may_delete → -EISDIR). */
	err = dir_load(info, &dt, true);
	if (err)
		goto out_dt;
	err = dir_find(&dt, name, &child);
	if (err)
		goto out_dt;
	err = dir_commit_op(info, &dt, AIOS_HTTP_OP_UNLINK, name, NULL, false);
	if (err)
		goto out_dt;
	aios_http_drop_link(info, child);
	drop_nlink(d_inode(dentry));
	d_drop(dentry);
out_dt:
	dir_table_free(&dt);
out:
	mutex_unlock(&info->http_mu);
	return err;
}

static int http_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_dir_table dt, child_dt;
	struct aios_inode_meta m = { 0 }, pmeta = { 0 };
	char name[AIOS_KABI_NAME_MAX + 1];
	u64 child;
	int err;

	if (dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(name, dentry->d_name.name, dentry->d_name.len);
	name[dentry->d_name.len] = '\0';

	mutex_lock(&info->http_mu);
	err = dir_table_init(&dt, info->volume, dir->i_ino);
	if (err)
		goto out;
	err = dir_load(info, &dt, false);
	if (err)
		goto out_dt;
	err = dir_find(&dt, name, &child);
	if (err)
		goto out_dt;
	err = load_inode(info, child, &m);
	if (err)
		goto out_dt;
	if (!S_ISDIR(m.mode)) {
		err = -ENOTDIR;
		goto out_dt;
	}
	err = dir_table_init(&child_dt, info->volume, child);
	if (err)
		goto out_dt;
	err = dir_load(info, &child_dt, false);
	if (err) {
		dir_table_free(&child_dt);
		goto out_dt;
	}
	if (child_dt.count) {
		dir_table_free(&child_dt);
		err = -ENOTEMPTY;
		goto out_dt;
	}
	dir_table_free(&child_dt);

	/* The removed directory may itself be leased by us; its lock would
	 * refuse the object deletes below. */
	dir_lease_drop(info, child);
	err = dir_commit_op(info, &dt, AIOS_HTTP_OP_UNLINK, name, NULL, false);
	if (err)
		goto out_dt;
	touch_dir_inode(info, dir, &pmeta, now_ns(), -1);
	delete_dir_objects(info, child);
	clear_nlink(d_inode(dentry));
	d_drop(dentry);
	err = 0;
out_dt:
	dir_table_free(&dt);
	inode_meta_reset(&m);
	inode_meta_reset(&pmeta);
out:
	mutex_unlock(&info->http_mu);
	return err;
}

static int http_rename_same_dir(struct aios_sb_info *info, u64 parent, const char *old_name,
				const char *new_name, bool noreplace)
{
	struct aios_dir_table dt;
	struct aios_inode_meta victim = { 0 };
	u64 victim_ino = 0;
	int err;

	err = dir_table_init(&dt, info->volume, parent);
	if (err)
		return err;
	err = dir_load(info, &dt, false);
	if (err)
		goto out;
	if (dir_find(&dt, old_name, NULL)) {
		err = -ENOENT;
		goto out;
	}
	if (!dir_find(&dt, new_name, &victim_ino)) {
		if (noreplace) {
			err = -EEXIST;
			goto out;
		}
		err = load_inode(info, victim_ino, &victim);
		if (err)
			goto out;
		if (S_ISDIR(victim.mode)) {
			struct aios_dir_table child_dt;

			err = dir_table_init(&child_dt, info->volume, victim_ino);
			if (err)
				goto out;
			err = dir_load(info, &child_dt, false);
			if (!err && child_dt.count) {
				dir_table_free(&child_dt);
				err = -ENOTEMPTY;
				goto out;
			}
			dir_table_free(&child_dt);
			dir_lease_drop(info, victim_ino);
		}
	}
	err = dir_commit_op(info, &dt, AIOS_HTTP_OP_RENAME, old_name, new_name, false);
	if (err)
		goto out;
	if (victim_ino)
		aios_http_drop_link(info, victim_ino);
out:
	dir_table_free(&dt);
	inode_meta_reset(&victim);
	return err;
}

struct aios_rename_ctx {
	struct aios_dir_table old_dir;
	struct aios_dir_table new_dir;
	struct aios_inode_meta moved;
	struct aios_inode_meta victim;
	struct aios_inode_meta old_p;
	struct aios_inode_meta new_p;
	struct aios_held_lock locks[6];
	char txn_id[AIOS_HTTP_TXN_ID_LEN];
	char oid[160];
	char inos[32];
};

static void rename_ctx_reset(struct aios_rename_ctx *rc)
{
	dir_table_free(&rc->old_dir);
	dir_table_free(&rc->new_dir);
	inode_meta_reset(&rc->moved);
	inode_meta_reset(&rc->victim);
	inode_meta_reset(&rc->old_p);
	inode_meta_reset(&rc->new_p);
}

/*
 * One attempt of a cross-directory rename. Returns 0 on success, -EAGAIN when
 * the caller should retry (lock held, CAS conflict, directory changed under
 * us), or a definitive -errno. Everything sizeable lives in *rc (heap).
 */
static int http_rename_cross_dir_once(struct aios_sb_info *info, struct aios_rename_ctx *rc,
				      u64 old_parent, const char *old_name, u64 new_parent,
				      const char *new_name, bool noreplace)
{
	u64 ino = 0, victim_ino = 0;
	u64 ts;
	u64 old_cas, new_cas, victim_cas;
	bool victim_exists = false;
	unsigned int nlocks = 6;
	unsigned int i;
	int err;

	memset(rc, 0, sizeof(*rc));
	err = dir_table_init(&rc->old_dir, info->volume, old_parent);
	if (err)
		return err;
	err = dir_table_init(&rc->new_dir, info->volume, new_parent);
	if (err)
		goto out;
	err = dir_load(info, &rc->old_dir, false);
	if (err)
		goto out;
	err = dir_load(info, &rc->new_dir, false);
	if (err)
		goto out;

	err = dir_find(&rc->old_dir, old_name, &ino);
	if (err)
		goto out;
	if (ino == new_parent) {
		err = -EINVAL;
		goto out;
	}
	err = load_inode(info, ino, &rc->moved);
	if (err)
		goto out;

	if (!dir_find(&rc->new_dir, new_name, &victim_ino)) {
		if (noreplace && victim_ino != ino) {
			err = -EEXIST;
			goto out;
		}
		if (victim_ino != ino) {
			err = load_inode(info, victim_ino, &rc->victim);
			if (err && err != -ENOENT)
				goto out;
			victim_exists = !err && rc->victim.exists;
			err = 0;
			if (victim_exists && S_ISDIR(rc->victim.mode)) {
				err = -EISDIR;
				goto out;
			}
			if (S_ISDIR(rc->moved.mode) && victim_exists && S_ISREG(rc->victim.mode)) {
				err = -ENOTDIR;
				goto out;
			}
		}
	} else {
		victim_ino = 0;
	}

	err = load_inode(info, old_parent, &rc->old_p);
	if (err)
		goto out;
	err = load_inode(info, new_parent, &rc->new_p);
	if (err)
		goto out;
	if (!S_ISDIR(rc->old_p.mode) || !S_ISDIR(rc->new_p.mode)) {
		err = -ENOTDIR;
		goto out;
	}

	strscpy(rc->locks[0].oid, rc->old_dir.meta_oid, sizeof(rc->locks[0].oid));
	strscpy(rc->locks[1].oid, rc->old_dir.log_oid, sizeof(rc->locks[1].oid));
	strscpy(rc->locks[2].oid, rc->old_dir.snap_oid, sizeof(rc->locks[2].oid));
	strscpy(rc->locks[3].oid, rc->new_dir.meta_oid, sizeof(rc->locks[3].oid));
	strscpy(rc->locks[4].oid, rc->new_dir.log_oid, sizeof(rc->locks[4].oid));
	strscpy(rc->locks[5].oid, rc->new_dir.snap_oid, sizeof(rc->locks[5].oid));
	for (i = 0; i < nlocks; i++)
		rc->locks[i].token[0] = '\0';

	err = acquire_sorted_locks(info->http, rc->locks, &nlocks);
	if (err)
		goto out;

	/* Reload under locks. */
	err = dir_load(info, &rc->old_dir, false);
	if (err)
		goto unlock;
	err = dir_load(info, &rc->new_dir, false);
	if (err)
		goto unlock;
	{
		u64 cur_ino = 0;

		err = dir_find(&rc->old_dir, old_name, &cur_ino);
		if (err)
			goto unlock;
		if (cur_ino != ino) {
			err = -EAGAIN;
			goto unlock;
		}
	}
	{
		u64 cur_victim = 0;
		int fe = dir_find(&rc->new_dir, new_name, &cur_victim);

		if (!fe) {
			if (cur_victim != victim_ino && cur_victim != ino) {
				err = -EAGAIN;
				goto unlock;
			}
		} else if (victim_ino && victim_ino != ino) {
			err = -EAGAIN;
			goto unlock;
		}
	}

	err = load_inode(info, old_parent, &rc->old_p);
	if (err)
		goto unlock;
	err = load_inode(info, new_parent, &rc->new_p);
	if (err)
		goto unlock;
	err = load_inode(info, ino, &rc->moved);
	if (err)
		goto unlock;
	if (victim_ino && victim_ino != ino) {
		err = load_inode(info, victim_ino, &rc->victim);
		victim_exists = !err && rc->victim.exists;
		if (err && err != -ENOENT)
			goto unlock;
		err = 0;
	}

	apply_dir_op(&rc->old_dir, AIOS_HTTP_OP_UNLINK, old_name, NULL);
	if (victim_ino && victim_ino != ino)
		apply_dir_op(&rc->new_dir, AIOS_HTTP_OP_UNLINK, new_name, NULL);
	snprintf(rc->inos, sizeof(rc->inos), "%llu", (unsigned long long)ino);
	apply_dir_op(&rc->new_dir, AIOS_HTTP_OP_LINK, new_name, rc->inos);

	ts = now_ns();
	rc->old_p.mtime_ns = rc->old_p.ctime_ns = ts;
	rc->new_p.mtime_ns = rc->new_p.ctime_ns = ts;
	if (S_ISDIR(rc->moved.mode)) {
		if (rc->old_p.nlink > 2)
			rc->old_p.nlink -= 1;
		rc->new_p.nlink += 1;
	}

	rc->txn_id[0] = '\0';
	err = aios_http_txn_begin(info->http, rc->txn_id, sizeof(rc->txn_id));
	if (err)
		goto unlock;

	err = txn_put_dir(info->http, rc->txn_id, &rc->old_dir, rc->locks, nlocks);
	if (err)
		goto abort;
	err = txn_put_dir(info->http, rc->txn_id, &rc->new_dir, rc->locks, nlocks);
	if (err)
		goto abort;

	{
		char *full = NULL;
		size_t flen = 0;

		err = inode_to_json_full(info, &rc->old_p, &full, &flen);
		if (err)
			goto abort;
		oid_ino(info->volume, old_parent, rc->oid, sizeof(rc->oid));
		old_cas = rc->old_p.cas;
		err = aios_http_txn_prepare_put(info->http, rc->txn_id, rc->oid, full, flen, NULL,
						&old_cas);
		kfree(full);
		if (err)
			goto abort;

		err = inode_to_json_full(info, &rc->new_p, &full, &flen);
		if (err)
			goto abort;
		oid_ino(info->volume, new_parent, rc->oid, sizeof(rc->oid));
		new_cas = rc->new_p.cas;
		err = aios_http_txn_prepare_put(info->http, rc->txn_id, rc->oid, full, flen, NULL,
						&new_cas);
		kfree(full);
		if (err)
			goto abort;
	}

	/*
	 * The replaced victim only loses a link here. Its chunks and inode
	 * object stay until the in-core inode is evicted with i_nlink == 0
	 * (an open descriptor may still use it); see aios_http_evict_unlinked.
	 */
	if (victim_ino && victim_ino != ino && victim_exists) {
		char *full = NULL;
		size_t flen = 0;

		oid_ino(info->volume, victim_ino, rc->oid, sizeof(rc->oid));
		rc->victim.nlink = rc->victim.nlink ? rc->victim.nlink - 1 : 0;
		rc->victim.ctime_ns = ts;
		err = inode_to_json_full(info, &rc->victim, &full, &flen);
		if (err)
			goto abort;
		victim_cas = rc->victim.cas;
		err = aios_http_txn_prepare_put(info->http, rc->txn_id, rc->oid, full, flen, NULL,
						&victim_cas);
		kfree(full);
		if (err)
			goto abort;
	}

	err = aios_http_txn_commit(info->http, rc->txn_id);
	if (err)
		goto abort;
	rc->txn_id[0] = '\0';
	dir_cache_publish(info, &rc->old_dir);
	dir_cache_publish(info, &rc->new_dir);
	release_held_locks(info->http, rc->locks, nlocks);
	rename_ctx_reset(rc);
	return 0;

abort:
	if (rc->txn_id[0])
		aios_http_txn_abort(info->http, rc->txn_id);
unlock:
	release_held_locks(info->http, rc->locks, nlocks);
out:
	rename_ctx_reset(rc);
	return err;
}

static int http_rename_cross_dir(struct aios_sb_info *info, u64 old_parent, const char *old_name,
				 u64 new_parent, const char *new_name, bool noreplace)
{
	struct aios_rename_ctx *rc;
	int attempt;
	int err = -EAGAIN;

	rc = kzalloc(sizeof(*rc), GFP_KERNEL);
	if (!rc)
		return -ENOMEM;
	/* The transaction below locks both directories itself; our own leases
	 * on them would refuse those locks. Flush and hand them back first. */
	dir_lease_drop(info, old_parent);
	dir_lease_drop(info, new_parent);
	for (attempt = 0; attempt < AIOS_HTTP_DIR_RETRIES; attempt++) {
		err = http_rename_cross_dir_once(info, rc, old_parent, old_name, new_parent,
						 new_name, noreplace);
		if (err != -EAGAIN)
			break;
		http_retry_backoff(attempt);
	}
	kfree(rc);
	return http_no_eagain(err);
}

static int http_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_dir_table new_dt;
	struct aios_inode_meta m = { 0 }, np = { 0 };
	struct inode *inode = d_inode(old_dentry);
	char new_name[AIOS_KABI_NAME_MAX + 1];
	char inos[32];
	u64 ts;
	int err;

	if (!inode || !S_ISREG(inode->i_mode))
		return -EPERM;
	if (dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(new_name, dentry->d_name.name, dentry->d_name.len);
	new_name[dentry->d_name.len] = '\0';
	if (strchr(new_name, '/'))
		return -EINVAL;

	mutex_lock(&info->http_mu);
	err = load_inode(info, inode->i_ino, &m);
	if (err)
		goto out;
	if (S_ISDIR(m.mode)) {
		err = -EPERM;
		goto out;
	}
	if (!dir_lease_get(info, dir->i_ino)) {
		err = load_inode(info, dir->i_ino, &np);
		if (err)
			goto out;
		if (!S_ISDIR(np.mode)) {
			err = -ENOTDIR;
			goto out;
		}
	}
	err = dir_table_init(&new_dt, info->volume, dir->i_ino);
	if (err)
		goto out;
	err = dir_load(info, &new_dt, false);
	if (err)
		goto out_dt;
	if (!dir_find(&new_dt, new_name, NULL)) {
		err = -EEXIST;
		goto out_dt;
	}
	ts = now_ns();
	m.nlink += 1;
	m.ctime_ns = ts;
	err = store_inode(info, &m);
	if (err)
		goto out_dt;
	snprintf(inos, sizeof(inos), "%llu", (unsigned long long)m.ino);
	err = dir_commit_op(info, &new_dt, AIOS_HTTP_OP_LINK, new_name, inos, true);
	if (err) {
		/* Undo the nlink bump; best effort. */
		m.nlink -= 1;
		store_inode(info, &m);
		goto out_dt;
	}
	err = touch_dir_inode(info, dir, &np, ts, 0);
	if (err)
		goto out_dt;
	{
		struct aios_kabi_stat st;

		meta_to_stat(&m, &st);
		aios_stat_to_inode(inode, &st);
		attach_iinfo(inode, &m);
	}
	ihold(inode);
	d_instantiate(dentry, inode);
	aios_d_mark_fresh(dentry);
out_dt:
	dir_table_free(&new_dt);
out:
	inode_meta_reset(&m);
	inode_meta_reset(&np);
	mutex_unlock(&info->http_mu);
	return err;
}

static int http_rename(AIOS_IDMAP *mnt_userns, struct inode *old_dir,
		       struct dentry *old_dentry, struct inode *new_dir,
		       struct dentry *new_dentry, unsigned int flags)
{
	struct aios_sb_info *info = AIOS_SB(old_dir->i_sb);
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	char old_name[AIOS_KABI_NAME_MAX + 1];
	char new_name[AIOS_KABI_NAME_MAX + 1];
	int err;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;
	if (old_dentry->d_name.len > AIOS_KABI_NAME_MAX ||
	    new_dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(old_name, old_dentry->d_name.name, old_dentry->d_name.len);
	old_name[old_dentry->d_name.len] = '\0';
	memcpy(new_name, new_dentry->d_name.name, new_dentry->d_name.len);
	new_name[new_dentry->d_name.len] = '\0';
	if (strchr(old_name, '/') || strchr(new_name, '/'))
		return -EINVAL;

	mutex_lock(&info->http_mu);
	if (old_dir->i_ino == new_dir->i_ino)
		err = http_rename_same_dir(info, old_dir->i_ino, old_name, new_name,
					   flags & RENAME_NOREPLACE);
	else
		err = http_rename_cross_dir(info, old_dir->i_ino, old_name, new_dir->i_ino,
					    new_name, flags & RENAME_NOREPLACE);
	mutex_unlock(&info->http_mu);
	if (err)
		return err;

	/* Mirror the server-side link changes on the in-core inodes so a replaced
	 * file reaches i_nlink == 0 and is cleaned up by evict_inode. */
	if (new_inode && new_inode != old_inode) {
		if (S_ISDIR(new_inode->i_mode))
			clear_nlink(new_inode);
		else
			drop_nlink(new_inode);
	}
	if (old_inode && S_ISDIR(old_inode->i_mode) && old_dir != new_dir) {
		drop_nlink(old_dir);
		inc_nlink(new_dir);
	}
	return 0;
}

static int http_getattr(AIOS_IDMAP *mnt_userns, const struct path *path,
			struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);

	if (!aios_d_is_fresh(path->dentry)) {
		int err = http_refresh(inode);

		if (err)
			return err;
		aios_d_mark_fresh(path->dentry);
	}
	aios_fillattr(mnt_userns, inode, stat);
	return 0;
}

/*
 * Wait for a set of deferred chunk deletions. Truncate must not return while
 * its dropped chunks are still being deleted: a write that re-extends the file
 * could otherwise land in a chunk the worker deletes a moment later.
 */
struct aios_chunk_del_sync {
	atomic_t pending;
	struct completion done;
};

struct aios_chunk_del_sync_work {
	struct work_struct work;
	struct aios_sb_info *info;
	struct aios_chunk_del_sync *sync;
	u64 ino;
	u64 first;
	u64 end;
	u64 stride;
};

static void chunk_del_sync_worker(struct work_struct *w)
{
	struct aios_chunk_del_sync_work *dw =
		container_of(w, struct aios_chunk_del_sync_work, work);
	struct aios_http_client *c = http_client_get(dw->info);

	delete_chunk_stripe(dw->info, c, dw->ino, dw->first, dw->end, dw->stride);
	http_client_put(dw->info, c);
	if (atomic_dec_and_test(&dw->sync->pending))
		complete(&dw->sync->done);
	kfree(dw);
}

/* Delete chunks [first, end) of @ino in parallel over the pool and wait. The
 * chunks below @first are untouched, so no stripe lock is needed: callers
 * have already cut the page cache back and hold the inode lock, so nothing
 * writes at or beyond @first while this runs. */
static void delete_chunks_parallel(struct aios_sb_info *info, u64 ino, u64 first, u64 end)
{
	struct aios_chunk_del_sync sync;
	u64 n = end > first ? end - first : 0;
	unsigned int workers, w;

	if (!n)
		return;
	workers = (unsigned int)min_t(u64, info->pool_size, (n + 15) / 16);
	if (workers <= 1 || !info->wb_wq) {
		struct aios_http_client *c = http_client_get(info);

		delete_chunk_stripe(info, c, ino, first, end, 1);
		http_client_put(info, c);
		return;
	}
	init_completion(&sync.done);
	/* Our own reference keeps a worker from ever completing while we might
	 * still return early; only the last dropper touches sync.done. The
	 * caller must not hold a pool client here: the workers each take one. */
	atomic_set(&sync.pending, 1);
	for (w = 0; w < workers; w++) {
		struct aios_chunk_del_sync_work *dw = kmalloc(sizeof(*dw), GFP_KERNEL);

		if (!dw) {
			struct aios_http_client *c = http_client_get(info);

			delete_chunk_stripe(info, c, ino, first + w, end, workers);
			http_client_put(info, c);
			continue;
		}
		INIT_WORK(&dw->work, chunk_del_sync_worker);
		dw->info = info;
		dw->sync = &sync;
		dw->ino = ino;
		dw->first = first + w;
		dw->end = end;
		dw->stride = workers;
		atomic_inc(&sync.pending);
		queue_work(info->wb_wq, &dw->work);
	}
	if (!atomic_dec_and_test(&sync.pending))
		wait_for_completion(&sync.done);
}

/*
 * Set the file size on the server: drop whole chunks past the new end, trim
 * the last partial chunk, then CAS PUT the inode object. Grow is metadata
 * only. The stripe lock on the trimmed chunk keeps a writeback of pages below
 * the new end from being lost between the GET and the PUT.
 *
 * @old_local is the largest size this client has seen for the file: chunk
 * data is PUT by writeback at once while the inode object's size is flushed
 * lazily, so the server's m->size may lag behind the chunks that exist. The
 * drop range is computed from the larger of the two, or a later extension
 * would read the stale bytes back instead of zeros.
 *
 * *cp is released while the drop runs on pool workers (they each need a
 * client) and reacquired afterwards.
 */
static int truncate_file(struct aios_sb_info *info, struct aios_http_client **cp,
			 struct aios_inode_aux *aux, struct aios_inode_meta *m, u64 size,
			 u64 old_local)
{
	u64 unit = m->stripe_unit ? m->stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	u64 old = max_t(u64, m->size, old_local);
	struct aios_http_client *c = *cp;
	char oid[160];
	int err;

	if (size < old) {
		u64 first_drop = (size + unit - 1) / unit;
		u64 old_chunks = (old + unit - 1) / unit;

		http_client_put(info, c);
		delete_chunks_parallel(info, m->ino, first_drop, old_chunks);
		c = http_client_get(info);
		*cp = c;
		if (size > 0) {
			u64 last = (size - 1) / unit;
			u64 keep = size - last * unit;
			struct mutex *mu = aios_chunk_lock(aux, last);
			struct aios_http_buf body = { 0 };

			oid_chunk(info->volume, m->ino, last, oid, sizeof(oid));
			mutex_lock(mu);
			err = aios_http_get(c, oid, &body, NULL);
			if (!err && body.len > keep)
				err = aios_http_put(c, oid, body.data, keep, NULL, NULL);
			else if (err == -ENOENT)
				err = 0;
			aios_http_buf_free(&body);
			mutex_unlock(mu);
			if (err)
				return err;
		}
	}
	m->size = size;
	m->mtime_ns = m->ctime_ns = now_ns();
	return store_inode_c(info, c, m);
}

/*
 * chmod / chown / utimes / truncate. Runs on a pool client under the inode's
 * meta_mu (not http_mu), so it neither waits for nor blocks namespace
 * operations; a CAS conflict with a concurrent nlink change or deferred size
 * flush is retried on a fresh load. For a truncate the page cache is cut
 * back first: truncate_setsize waits for writeback of the pages it removes,
 * so no in-flight chunk write can resurrect data past the new end after the
 * server-side delete.
 */
static int http_setattr(AIOS_IDMAP *mnt_userns, struct dentry *dentry,
			struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m = { 0 };
	struct aios_inode_aux *aux;
	struct aios_http_client *c;
	u64 old_local = 0;
	int attempt;
	int err;

	err = setattr_prepare(mnt_userns, dentry, attr);
	if (err)
		return err;
	aux = aios_inode_aux_get(inode, NULL);
	if (!aux)
		return -ENOMEM;

	if (attr->ia_valid & ATTR_SIZE) {
		old_local = max_t(u64, (u64)i_size_read(inode), aux->last_synced_size);
		truncate_setsize(inode, attr->ia_size);
	}

	c = http_client_get(info);
	mutex_lock(&aux->meta_mu);
	err = -EAGAIN;
	for (attempt = 0; attempt < AIOS_HTTP_META_RETRIES && err == -EAGAIN; attempt++) {
		if (attempt)
			http_retry_backoff(attempt);
		err = load_inode_c(info, c, inode->i_ino, &m);
		if (err)
			break;
		if (attr->ia_valid & ATTR_MODE)
			m.mode = (m.mode & S_IFMT) | (attr->ia_mode & 07777);
		if (attr->ia_valid & ATTR_UID)
			m.uid = aios_iattr_uid(mnt_userns, attr);
		if (attr->ia_valid & ATTR_GID)
			m.gid = aios_iattr_gid(mnt_userns, attr);
		if (attr->ia_valid & ATTR_SIZE) {
			err = truncate_file(info, &c, aux, &m, (u64)attr->ia_size, old_local);
		} else if (attr->ia_valid &
			   (ATTR_MODE | ATTR_UID | ATTR_GID | ATTR_MTIME | ATTR_ATIME)) {
			if (attr->ia_valid & ATTR_MTIME)
				m.mtime_ns = (u64)attr->ia_mtime.tv_sec * 1000000000ull +
					     attr->ia_mtime.tv_nsec;
			if (attr->ia_valid & ATTR_ATIME)
				m.atime_ns = (u64)attr->ia_atime.tv_sec * 1000000000ull +
					     attr->ia_atime.tv_nsec;
			m.ctime_ns = now_ns();
			err = store_inode_c(info, c, &m);
		} else {
			err = 0;
		}
	}
	if (err)
		goto out;
	{
		struct aios_kabi_stat st;

		meta_to_stat(&m, &st);
		aios_stat_to_inode(inode, &st);
		attach_iinfo(inode, &m);
	}
	if (attr->ia_valid & ATTR_SIZE) {
		/* The server now holds exactly ia_size; a stale last_synced_size
		 * would make write_inode / evict push the pre-truncate size. */
		http_clear_size_dirty(inode, (u64)attr->ia_size);
	}
	setattr_copy(mnt_userns, inode, attr);
	mark_inode_dirty(inode);
out:
	mutex_unlock(&aux->meta_mu);
	http_client_put(info, c);
	inode_meta_reset(&m);
	return http_no_eagain(err);
}

static void http_kfree_link(void *p)
{
	kfree(p);
}

static int http_symlink(AIOS_IDMAP *mnt_userns, struct inode *dir, struct dentry *dentry,
			const char *target)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_dir_table dt;
	struct aios_inode_meta pmeta = { 0 }, m = { 0 };
	struct inode *inode;
	char name[AIOS_KABI_NAME_MAX + 1];
	char inos[32];
	size_t tlen;
	u64 ino, ts;
	int err;

	(void)mnt_userns;
	tlen = target ? strlen(target) : 0;
	if (!tlen || tlen > AIOS_KABI_SYMLINK_MAX)
		return -ENAMETOOLONG;
	if (dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(name, dentry->d_name.name, dentry->d_name.len);
	name[dentry->d_name.len] = '\0';
	if (strchr(name, '/'))
		return -EINVAL;

	mutex_lock(&info->http_mu);
	if (!dir_lease_get(info, dir->i_ino)) {
		err = load_inode(info, dir->i_ino, &pmeta);
		if (err)
			goto out;
		if (!S_ISDIR(pmeta.mode)) {
			err = -ENOTDIR;
			goto out;
		}
	}
	err = dir_table_init(&dt, info->volume, dir->i_ino);
	if (err)
		goto out;
	err = dir_load(info, &dt, false);
	if (err)
		goto out_dt;
	if (!dir_find(&dt, name, NULL)) {
		err = -EEXIST;
		goto out_dt;
	}
	err = alloc_ino(info, &ino);
	if (err)
		goto out_dt;
	ts = now_ns();
	m.ino = ino;
	m.mode = S_IFLNK | 0777;
	m.nlink = 1;
	m.uid = from_kuid(&init_user_ns, current_fsuid());
	m.gid = from_kgid(&init_user_ns, current_fsgid());
	m.size = tlen;
	m.symlink = kstrdup(target, GFP_KERNEL);
	if (!m.symlink) {
		err = -ENOMEM;
		goto out_dt;
	}
	m.atime_ns = m.mtime_ns = m.ctime_ns = ts;
	m.stripe_unit = info->stripe_unit ? info->stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	m.stripe_width = info->stripe_width ? info->stripe_width : AIOS_HTTP_DEFAULT_STRIPE_WIDTH;
	m.cas = 0;
	m.extras_loaded = true;
	err = store_inode(info, &m);
	if (err)
		goto out_dt;
	snprintf(inos, sizeof(inos), "%llu", (unsigned long long)ino);
	err = dir_commit_op(info, &dt, AIOS_HTTP_OP_LINK, name, inos, true);
	if (err) {
		char oid[160];

		oid_ino(info->volume, ino, oid, sizeof(oid));
		aios_http_delete(info->http, oid);
		goto out_dt;
	}
	err = touch_dir_inode(info, dir, &pmeta, ts, 0);
	if (err)
		goto out_dt;
	inode = aios_http_iget(dir->i_sb, &m);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_dt;
	}
	d_instantiate(dentry, inode);
	aios_d_mark_fresh(dentry);
out_dt:
	dir_table_free(&dt);
out:
	inode_meta_reset(&pmeta);
	inode_meta_reset(&m);
	mutex_unlock(&info->http_mu);
	return err;
}

static const char *http_get_link(struct dentry *dentry, struct inode *inode,
				 struct delayed_call *done)
{
	struct aios_inode_aux *aux = inode->i_private;
	struct aios_sb_info *info;
	struct aios_inode_meta m = { 0 };
	char *s;
	int err;

	if (!dentry)
		return ERR_PTR(-ECHILD);
	if (aux) {
		s = aux_symlink_dup(aux);
		if (s) {
			set_delayed_call(done, http_kfree_link, s);
			return s;
		}
	}
	info = AIOS_SB(inode->i_sb);
	mutex_lock(&info->http_mu);
	err = load_inode(info, inode->i_ino, &m);
	mutex_unlock(&info->http_mu);
	if (err) {
		inode_meta_reset(&m);
		return ERR_PTR(err);
	}
	if (!S_ISLNK(m.mode) || !m.symlink) {
		inode_meta_reset(&m);
		return ERR_PTR(-EINVAL);
	}
	s = kstrdup(m.symlink, GFP_KERNEL);
	attach_iinfo(inode, &m);
	inode_meta_reset(&m);
	if (!s)
		return ERR_PTR(-ENOMEM);
	set_delayed_call(done, http_kfree_link, s);
	return s;
}

const struct inode_operations aios_http_dir_inode_ops = {
	.lookup = http_lookup,
	.create = http_create,
	.mkdir = http_mkdir,
	.unlink = http_unlink,
	.rmdir = http_rmdir,
	.rename = http_rename,
	.link = http_link,
	.symlink = http_symlink,
	.getattr = http_getattr,
	.setattr = http_setattr,
	.listxattr = aios_listxattr,
};

const struct inode_operations aios_http_file_inode_ops = {
	.getattr = http_getattr,
	.setattr = http_setattr,
	.listxattr = aios_listxattr,
};

const struct inode_operations aios_http_symlink_inode_ops = {
	.get_link = http_get_link,
	.getattr = http_getattr,
	.setattr = http_setattr,
	.listxattr = aios_listxattr,
};

static int http_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_dir_table dt;
	unsigned int i;
	int err;

	if (ctx->pos == 0) {
		if (!dir_emit_dot(file, ctx))
			return 0;
	}
	if (ctx->pos == 1) {
		if (!dir_emit_dotdot(file, ctx))
			return 0;
	}

	mutex_lock(&info->http_mu);
	err = dir_table_init(&dt, info->volume, inode->i_ino);
	if (err)
		goto out;
	err = dir_load(info, &dt, true);
	if (err)
		goto out_dt;

	/* pos 0,1 are . and ..; entries start at pos 2. d_type comes from the
	 * in-core inode when we have one (a GET per entry would make a listing
	 * cost N round trips); otherwise DT_UNKNOWN, which is what POSIX
	 * readdir permits and what ls/find handle by stat()ing on demand. */
	for (i = 0; i < dt.count; i++) {
		loff_t pos = (loff_t)i + 2;
		unsigned char type = DT_UNKNOWN;
		struct inode *child;

		if (ctx->pos > pos)
			continue;
		child = ilookup(inode->i_sb, dt.ents[i].ino);
		if (child) {
			type = fs_umode_to_dtype(child->i_mode);
			iput(child);
		}
		if (!dir_emit(ctx, dt.ents[i].name, strlen(dt.ents[i].name), dt.ents[i].ino,
			      type))
			break;
		ctx->pos = pos + 1;
	}
out_dt:
	dir_table_free(&dt);
out:
	mutex_unlock(&info->http_mu);
	return err;
}

/* fsync(2) of a directory: commit the operations queued under its lease. */
static int http_dir_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file_inode(file);

	(void)start;
	(void)end;
	(void)datasync;
	return dir_lease_fsync(AIOS_SB(inode->i_sb), inode->i_ino);
}

const struct file_operations aios_http_dir_ops = {
	.owner = THIS_MODULE,
	.iterate_shared = http_readdir,
	.llseek = generic_file_llseek,
	.fsync = http_dir_fsync,
};

/*
 * Stripe unit of a regular file. The aux is filled by every inode load
 * (lookup, getattr, create), so this is normally a field read; the GET only
 * happens for an inode whose aux was never populated.
 */
static int http_file_unit(struct aios_sb_info *info, struct aios_http_client *c,
			  struct inode *inode, struct aios_inode_aux *aux, u64 *unit_out)
{
	struct aios_inode_meta m = { 0 };
	int err;

	if (aux && aux->stripe_unit) {
		*unit_out = aux->stripe_unit;
		return 0;
	}
	err = load_inode_c(info, c, inode->i_ino, &m);
	if (err)
		return err;
	if (!S_ISREG(m.mode)) {
		inode_meta_reset(&m);
		return -EISDIR;
	}
	attach_iinfo(inode, &m);
	*unit_out = m.stripe_unit ? m.stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	inode_meta_reset(&m);
	return 0;
}

/*
 * Read [pos, pos+len) into buf: one ranged GET per chunk touched, on a pool
 * client, without http_mu. The size is the local i_size: the VFS bounds reads
 * by it, our own writes advance it before they return, and getattr refreshes
 * it from the server within the attribute TTL. Bytes past what the server
 * holds (a chunk not yet written or shorter than the file) read as zeros.
 */
/* Ranged GET per chunk into buf (pre-zeroed by the caller) on client @c;
 * [pos, end) must already be clipped to the file size. */
static int http_read_range(struct aios_sb_info *info, struct aios_http_client *c, u64 ino,
			   u64 unit, u64 pos, u64 end, char *buf)
{
	u64 p = pos;
	int err = 0;

	while (p < end) {
		u64 chunk = p / unit;
		u64 chunk_off = p % unit;
		u64 chunk_end = min_t(u64, end, (chunk + 1) * unit);
		size_t n = (size_t)(chunk_end - p);
		char oid[160];
		struct aios_http_buf body = { 0 };

		oid_chunk(info->volume, ino, chunk, oid, sizeof(oid));
		err = aios_http_get_range(c, oid, chunk_off, chunk_off + n - 1, &body);
		if (!err && body.len)
			memcpy(buf + (p - pos), body.data, min(n, body.len));
		else if (err && err != -ENOENT && err != -ERANGE) {
			aios_http_buf_free(&body);
			return err;
		}
		aios_http_buf_free(&body);
		p += n;
		err = 0;
	}
	return 0;
}

int aios_http_io_read(struct inode *inode, loff_t pos, void *buf, size_t len, size_t *out_len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_aux *aux = inode->i_private;
	struct aios_http_client *c;
	u64 unit, end, file_size;
	int err;

	if (out_len)
		*out_len = 0;
	if (!len)
		return 0;
	memset(buf, 0, len);
	file_size = (u64)i_size_read(inode);
	if ((u64)pos >= file_size)
		return 0;
	end = min_t(u64, (u64)pos + len, file_size);

	c = http_client_get(info);
	err = http_file_unit(info, c, inode, aux, &unit);
	if (!err)
		err = http_read_range(info, c, inode->i_ino, unit, (u64)pos, end, buf);
	http_client_put(info, c);
	if (!err && out_len)
		*out_len = (size_t)(end - (u64)pos);
	return err;
}

/*
 * Read-ahead: the VFS hands us a batch of locked, not-uptodate pages that are
 * contiguous in the file. One aios_http_io_read call issues one ranged GET
 * per chunk the batch touches (a whole batch inside a chunk is a single GET),
 * instead of the per-page GET ->readpage would do. bdi ra_pages is set to the
 * stripe unit so a sequential reader fetches whole chunks per round trip.
 */
void aios_http_readahead(struct readahead_control *rac)
{
	struct inode *inode = rac->mapping->host;
	loff_t pos = readahead_pos(rac);
	size_t len = readahead_length(rac);
	size_t got = 0;
	size_t off = 0;
	char *buf;
	int err;

	if (!len)
		return;
	buf = kvmalloc(len, GFP_KERNEL);
	if (!buf) {
		/* Leave the pages !uptodate; ->readpage fills them on demand. */
		return;
	}
	err = aios_http_io_read(inode, pos, buf, len, &got);
	/* aios_http_io_read zero-fills whatever the server did not return, so
	 * every byte of buf is valid once it succeeds. */
#ifdef AIOS_HAS_FOLIO_AOPS
	{
		struct folio *folio;

		/* readahead_folio() drops the reference; we only unlock. */
		while ((folio = readahead_folio(rac))) {
			struct page *page = folio_page(folio, 0);

			if (!err) {
				void *kaddr = kmap(page);

				memcpy(kaddr, buf + off, min_t(size_t, PAGE_SIZE, len - off));
				kunmap(page);
				flush_dcache_page(page);
				folio_mark_uptodate(folio);
			}
			off += PAGE_SIZE;
			folio_unlock(folio);
		}
	}
#else
	{
		struct page *page;

		while ((page = readahead_page(rac))) {
			if (!err) {
				void *kaddr = kmap(page);

				memcpy(kaddr, buf + off, min_t(size_t, PAGE_SIZE, len - off));
				kunmap(page);
				flush_dcache_page(page);
				SetPageUptodate(page);
			}
			off += PAGE_SIZE;
			unlock_page(page);
			put_page(page);
		}
	}
#endif
	kvfree(buf);
}

/*
 * Write @n bytes at @chunk_off into one chunk object on client @c. Callers hold
 * the chunk's stripe mutex. A write covering the whole chunk is a plain PUT;
 * anything smaller is a Content-Range PUT that the server patches in place
 * (zero-filling any gap), so a small write costs one round trip and moves only
 * its own bytes instead of GET + full-chunk PUT. Erasure-coded or compressed
 * tips cannot be patched (-EOPNOTSUPP); those fall back to the GET → patch →
 * PUT rewrite.
 */
static int http_chunk_put(struct aios_http_client *c, const char *oid, u64 unit, u64 chunk_off,
			  const void *data, size_t n)
{
	struct aios_http_buf body = { 0 };
	size_t nlen;
	char *nb;
	int err;

	if (chunk_off == 0 && n >= unit)
		return aios_http_put(c, oid, data, n, NULL, NULL);
	err = aios_http_put_range(c, oid, chunk_off, data, n, NULL);
	if (err != -EOPNOTSUPP)
		return err;

	err = aios_http_get(c, oid, &body, NULL);
	if (err && err != -ENOENT)
		return err;
	nlen = max_t(size_t, body.len, chunk_off + n);
	nb = kvmalloc(nlen, GFP_KERNEL);
	if (!nb) {
		aios_http_buf_free(&body);
		return -ENOMEM;
	}
	memset(nb, 0, nlen);
	if (body.len)
		memcpy(nb, body.data, body.len);
	aios_http_buf_free(&body);
	memcpy(nb + chunk_off, data, n);
	err = aios_http_put(c, oid, nb, nlen, NULL, NULL);
	kvfree(nb);
	return err;
}

/*
 * Write a byte range across chunks using an explicit client (no http_mu). The
 * inode's stripe mutex for each chunk is held across the update; every chunk
 * writer (buffered writeback workers, writepage, O_DIRECT, punch, truncate)
 * takes the same lock so concurrent partial updates to one chunk cannot lose
 * each other.
 */
static int http_chunk_write(struct aios_http_client *c, struct aios_inode_aux *aux,
			    const char *volume, u64 ino, u64 unit, u64 pos, const void *buf,
			    size_t len)
{
	u64 p = pos;
	size_t done = 0;
	int err = 0;

	while (done < len) {
		u64 chunk = p / unit;
		u64 chunk_off = p % unit;
		size_t n = min_t(size_t, (size_t)(unit - chunk_off), len - done);
		struct mutex *mu = aios_chunk_lock(aux, chunk);
		char oid[160];

		oid_chunk(volume, ino, chunk, oid, sizeof(oid));
		mutex_lock(mu);
		err = http_chunk_put(c, oid, unit, chunk_off, (const char *)buf + done, n);
		mutex_unlock(mu);
		if (err)
			return err;
		p += n;
		done += n;
	}
	return 0;
}

/* Account @wrote bytes of chunk data whose size/mtime is not yet on the
 * server; grow i_size to @new_size if that is larger (0 = leave i_size). */
static void mark_http_size_dirty(struct inode *inode, u64 new_size, size_t wrote)
{
	struct aios_inode_aux *aux = inode->i_private;

	if (new_size > (u64)i_size_read(inode))
		i_size_write(inode, new_size);
	inode->i_mtime = inode->i_ctime = current_time(inode);
	aios_set_inode_blocks(inode);
	if (aux) {
		if (!aux->dirty_since)
			aux->dirty_since = jiffies;
		aux->dirty_bytes += wrote;
	}
	mark_inode_dirty_sync(inode);
}

static bool http_should_flush_size(struct aios_inode_aux *aux)
{
	if (!aux || !aux->dirty_since)
		return false;
	if (aux->dirty_bytes >= AIOS_DIRTY_FLUSH_BYTES)
		return true;
	return time_after(jiffies, aux->dirty_since + msecs_to_jiffies(AIOS_DIRTY_FLUSH_MS));
}

static void http_clear_size_dirty(struct inode *inode, u64 size)
{
	struct aios_inode_aux *aux = inode->i_private;

	if (!aux)
		return;
	aux->last_synced_size = size;
	aux->dirty_bytes = 0;
	aux->dirty_since = 0;
}

/*
 * Push size/mtime to the inode object after chunk writes. Serialised per inode
 * by aux->meta_mu against setattr / xattr writers on other clients of this
 * mount; a CAS conflict with a namespace op (link count change) is retried
 * with a fresh load. The server copy only grows: another client may have
 * extended the file meanwhile, and an explicit shrink goes through setattr.
 * Not an error path for the data: chunks are already durable, so a failure
 * leaves the size dirty for the next write_inode / evict to retry.
 */
static int http_flush_size(struct aios_sb_info *info, struct aios_http_client *c,
			   struct inode *inode, struct aios_inode_aux *aux, u64 size)
{
	struct aios_inode_meta m = { 0 };
	int attempt;
	int err = -EAGAIN;

	mutex_lock(&aux->meta_mu);
	for (attempt = 0; attempt < AIOS_HTTP_META_RETRIES && err == -EAGAIN; attempt++) {
		err = load_inode_c(info, c, inode->i_ino, &m);
		if (err)
			break;
		if (!S_ISREG(m.mode)) {
			err = -EISDIR;
			break;
		}
		if (m.size >= size) {
			/* Nothing to publish (a peer or an earlier flush got there). */
			attach_iinfo(inode, &m);
			http_clear_size_dirty(inode, m.size);
			err = 0;
			break;
		}
		m.size = size;
		m.mtime_ns = m.ctime_ns = now_ns();
		err = store_inode_c(info, c, &m);
		if (!err) {
			attach_iinfo(inode, &m);
			http_clear_size_dirty(inode, m.size);
		} else if (err == -EAGAIN) {
			http_retry_backoff(attempt);
		}
	}
	mutex_unlock(&aux->meta_mu);
	inode_meta_reset(&m);
	return err;
}

/* After chunk data landed: bump the local size/mtime and flush the inode
 * object if enough bytes or time accumulated since the last flush. */
static int http_note_written(struct aios_sb_info *info, struct aios_http_client *c,
			     struct inode *inode, struct aios_inode_aux *aux, u64 end,
			     size_t wrote)
{
	u64 size = max_t(u64, (u64)i_size_read(inode), end);

	mark_http_size_dirty(inode, size, wrote);
	if (!http_should_flush_size(aux))
		return 0;
	return http_flush_size(info, c, inode, aux, size);
}

int aios_http_io_write(struct inode *inode, loff_t pos, const void *buf, size_t len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_aux *aux;
	struct aios_http_client *c;
	u64 unit;
	int err;

	if (!len)
		return 0;

	aux = aios_inode_aux_get(inode, NULL);
	if (!aux)
		return -ENOMEM;

	c = http_client_get(info);
	err = http_file_unit(info, c, inode, aux, &unit);
	if (err)
		goto out;
	err = http_chunk_write(c, aux, info->volume, inode->i_ino, unit, (u64)pos, buf, len);
	if (err)
		goto out;
	err = http_note_written(info, c, inode, aux, (u64)pos + len, len);
out:
	http_client_put(info, c);
	return err;
}

/*
 * O_DIRECT. The request is cut at chunk boundaries and up to pool_size
 * segments (capped at AIOS_HTTP_DIO_WINDOW bytes of buffers) are in flight
 * at once on wb_wq, each on its own pool client, so a large direct read or
 * write moves at the pool's aggregate rate rather than one chunk per round
 * trip. User memory is copied in the caller's context (workers have no mm);
 * segments complete in file order so a short or failed segment truncates the
 * result at exactly the bytes that are known to have landed / arrived.
 */
#define AIOS_HTTP_DIO_WINDOW (16u * 1024u * 1024u)

struct aios_dio_seg {
	struct work_struct work;
	struct aios_sb_info *info;
	struct aios_inode_aux *aux;
	u64 ino;
	u64 unit;
	u64 pos;
	size_t len;
	char *buf;
	bool write;
	int err;
	atomic_t *pending;
	struct completion *done;
};

static void aios_dio_seg_work(struct work_struct *w)
{
	struct aios_dio_seg *s = container_of(w, struct aios_dio_seg, work);
	struct aios_http_client *c = http_client_get(s->info);

	if (s->write)
		s->err = http_chunk_write(c, s->aux, s->info->volume, s->ino, s->unit, s->pos,
					  s->buf, s->len);
	else
		s->err = http_read_range(s->info, c, s->ino, s->unit, s->pos, s->pos + s->len,
					 s->buf);
	http_client_put(s->info, c);
	if (atomic_dec_and_test(s->pending))
		complete(s->done);
}

ssize_t aios_http_dio(struct inode *inode, loff_t pos, struct iov_iter *iter, bool write)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_aux *aux;
	struct aios_http_client *c;
	struct aios_dio_seg *segs;
	struct completion done;
	atomic_t pending;
	unsigned int nseg_max, i;
	u64 unit, cur = (u64)pos;
	u64 limit = write ? U64_MAX : (u64)i_size_read(inode);
	ssize_t total = 0;
	int err;

	if (!iov_iter_count(iter))
		return 0;
	if (!write && cur >= limit)
		return 0;
	aux = aios_inode_aux_get(inode, NULL);
	if (!aux)
		return -ENOMEM;
	/* Never hold a pool client while waiting for workers that need one:
	 * with every client owned by a waiting caller the workers would block
	 * in aios_http_pool_get forever. */
	c = http_client_get(info);
	err = http_file_unit(info, c, inode, aux, &unit);
	http_client_put(info, c);
	if (err)
		return err;
	nseg_max = clamp_t(u64, AIOS_HTTP_DIO_WINDOW / unit, 1, info->pool_size);
	segs = kcalloc(nseg_max, sizeof(*segs), GFP_KERNEL);
	if (!segs)
		return -ENOMEM;
	init_completion(&done);

	while (iov_iter_count(iter) && cur < limit && !err) {
		unsigned int nseg = 0;
		bool stop = false;

		/* Fill a window of chunk-aligned segments. */
		while (nseg < nseg_max && iov_iter_count(iter) && cur < limit) {
			struct aios_dio_seg *s = &segs[nseg];
			size_t n = min_t(u64, unit - (cur % unit), iov_iter_count(iter));

			n = min_t(u64, n, limit - cur);
			s->buf = write ? kvmalloc(n, GFP_KERNEL) : kvzalloc(n, GFP_KERNEL);
			if (!s->buf) {
				err = -ENOMEM;
				break;
			}
			if (write && copy_from_iter(s->buf, n, iter) != n) {
				kvfree(s->buf);
				s->buf = NULL;
				err = -EFAULT;
				break;
			}
			s->info = info;
			s->aux = aux;
			s->ino = inode->i_ino;
			s->unit = unit;
			s->pos = cur;
			s->len = n;
			s->write = write;
			s->err = 0;
			s->pending = &pending;
			s->done = &done;
			INIT_WORK(&s->work, aios_dio_seg_work);
			cur += n;
			nseg++;
		}
		if (!nseg)
			break;

		reinit_completion(&done);
		atomic_set(&pending, nseg);
		for (i = 0; i < nseg; i++)
			queue_work(info->wb_wq, &segs[i].work);
		wait_for_completion(&done);

		/* Consume in order; the first failure ends the transfer there. */
		for (i = 0; i < nseg; i++) {
			struct aios_dio_seg *s = &segs[i];

			if (!stop && s->err) {
				if (!err)
					err = s->err;
				stop = true;
			}
			if (!stop && !write && copy_to_iter(s->buf, s->len, iter) != s->len) {
				err = -EFAULT;
				stop = true;
			}
			if (!stop)
				total += s->len;
			kvfree(s->buf);
			s->buf = NULL;
		}
		if (stop)
			break;
	}
	kfree(segs);

	if (write && total > 0) {
		int serr;

		c = http_client_get(info);
		serr = http_note_written(info, c, inode, aux, (u64)pos + total, total);
		http_client_put(info, c);
		if (serr && !err)
			err = serr;
	}
	if (total > 0)
		return total;
	return err ? err : 0;
}

/*
 * Buffered writeback.
 *
 * write_cache_pages hands us dirty pages in file order. They are copied into
 * a round buffer sized to one stripe unit (so a sequential writer's round is
 * exactly one chunk), grouped by chunk, and each chunk group is queued to
 * wb_wq. Up to AIOS_HTTP_WB_INFLIGHT rounds are in flight at once: the
 * collector keeps copying the next round while earlier ones are on the wire,
 * so the pool is busy instead of idling between rounds.
 *
 * A worker ends writeback on its own pages as soon as its chunk landed. That
 * matters for WB_SYNC_ALL, where write_cache_pages waits for a page that is
 * still under writeback: it must not wait on this thread, which is the one
 * collecting. It also gives the ordering guarantee for two rounds touching one
 * chunk: a page can only be in a later round after its earlier writeback
 * ended, i.e. after that chunk's earlier PUT completed.
 *
 * Contiguous pages in a round are contiguous in the round buffer, so a full
 * chunk is one PUT straight from the buffer and a partial run is one ranged
 * PUT. A chunk with more than a few disjoint runs is patched with one GET/PUT.
 */
#define AIOS_HTTP_WB_MIN_PAGES 64
#define AIOS_HTTP_WB_MAX_PAGES 1024 /* 4 MiB rounds with 4 KiB pages */
#define AIOS_HTTP_WB_INFLIGHT 4
#define AIOS_HTTP_WB_MAX_RUNS 4
/* Upper bound on collect rounds per ->writepages call; each round that
 * makes no progress or fails terminates the loop earlier. */
#define AIOS_HTTP_WB_MAX_ROUNDS 65536

struct aios_http_wb_page {
	struct page *page;
	loff_t pos;
	size_t len; /* bytes below i_size; 0 = nothing to write */
	char *data; /* slot in the round buffer */
};

struct aios_http_wb_round;

struct aios_http_wb_chunk {
	struct work_struct work;
	struct aios_http_wb_round *round;
	u64 chunk;
	struct aios_http_wb_page *pages; /* ascending pos, all in this chunk */
	unsigned int npages;
	int err;
};

struct aios_http_wb_round {
	struct aios_sb_info *info;
	struct aios_inode_aux *aux;
	struct address_space *mapping;
	struct writeback_control *wbc;
	u64 ino;
	u64 unit;
	unsigned int cap; /* pages per round */
	unsigned int n; /* pages collected */
	char *buf; /* cap * PAGE_SIZE */
	struct aios_http_wb_page *pages;
	struct aios_http_wb_chunk *chunks;
	unsigned int nchunks;
	atomic_t pending; /* chunk works not yet finished */
	struct completion done;
	u64 max_end; /* highest file offset written by this round */
	size_t wrote;
	int err;
};

static void aios_http_wb_round_free(struct aios_http_wb_round *r)
{
	if (!r)
		return;
	kfree(r->chunks);
	kvfree(r->buf);
	kfree(r->pages);
	kfree(r);
}

static struct aios_http_wb_round *aios_http_wb_round_alloc(struct address_space *mapping,
							   struct writeback_control *wbc,
							   struct aios_inode_aux *aux, u64 unit)
{
	struct inode *inode = mapping->host;
	struct aios_http_wb_round *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return NULL;
	r->info = AIOS_SB(inode->i_sb);
	r->aux = aux;
	r->mapping = mapping;
	r->wbc = wbc;
	r->ino = inode->i_ino;
	r->unit = unit;
	r->cap = clamp_t(u64, unit / PAGE_SIZE, AIOS_HTTP_WB_MIN_PAGES, AIOS_HTTP_WB_MAX_PAGES);
	r->pages = kcalloc(r->cap, sizeof(*r->pages), GFP_KERNEL);
	r->buf = kvmalloc((size_t)r->cap * PAGE_SIZE, GFP_KERNEL);
	if (!r->pages || !r->buf) {
		aios_http_wb_round_free(r);
		return NULL;
	}
	init_completion(&r->done);
	return r;
}

/* Finish one page after its chunk was written (or failed). */
static void aios_http_wb_end_page(struct aios_http_wb_round *r, struct aios_http_wb_page *wp,
				  int err)
{
	if (err) {
		SetPageError(wp->page);
		mapping_set_error(r->mapping, err);
		redirty_page_for_writepage(r->wbc, wp->page);
	} else {
		ClearPageError(wp->page);
	}
	end_page_writeback(wp->page);
}

/* Write pages[i..j) — a contiguous run inside one chunk — from the round buffer. */
static int aios_http_wb_put_run(struct aios_http_client *c, struct aios_http_wb_round *r,
				const char *oid, struct aios_http_wb_page *pages, unsigned int i,
				unsigned int j)
{
	u64 base = pages[i].pos - (pages[i].pos % r->unit);
	u64 off = (u64)pages[i].pos - base;
	size_t len = (size_t)((u64)pages[j - 1].pos + pages[j - 1].len - (u64)pages[i].pos);

	return http_chunk_put(c, oid, r->unit, off, pages[i].data, len);
}

/*
 * Apply every page of one work item to its chunk under the stripe lock. The
 * pages arrive in ascending order and belong to cw->chunk; zero-length pages
 * (beyond i_size at collect time) are skipped.
 */
static int aios_http_wb_chunk_apply(struct aios_http_client *c, struct aios_http_wb_chunk *cw)
{
	struct aios_http_wb_round *r = cw->round;
	struct mutex *mu = aios_chunk_lock(r->aux, cw->chunk);
	struct aios_http_wb_page *pg = cw->pages;
	struct aios_http_buf body = { 0 };
	u64 base = cw->chunk * r->unit;
	unsigned int runs = 0, i, j;
	size_t nlen = 0;
	char oid[160];
	char *nb;
	int err = 0;

	/* Count runs and the extent this chunk needs. */
	for (i = 0; i < cw->npages; i = j) {
		u64 end;

		if (!pg[i].len) {
			j = i + 1;
			continue;
		}
		for (j = i + 1; j < cw->npages; j++) {
			if (!pg[j].len || pg[j].pos != pg[j - 1].pos + PAGE_SIZE ||
			    pg[j - 1].len != PAGE_SIZE)
				break;
		}
		runs++;
		end = (u64)pg[j - 1].pos + pg[j - 1].len - base;
		if (end > nlen)
			nlen = (size_t)end;
	}
	if (!runs)
		return 0;

	oid_chunk(r->info->volume, r->ino, cw->chunk, oid, sizeof(oid));
	mutex_lock(mu);
	if (runs <= AIOS_HTTP_WB_MAX_RUNS) {
		for (i = 0; i < cw->npages && !err; i = j) {
			if (!pg[i].len) {
				j = i + 1;
				continue;
			}
			for (j = i + 1; j < cw->npages; j++) {
				if (!pg[j].len || pg[j].pos != pg[j - 1].pos + PAGE_SIZE ||
				    pg[j - 1].len != PAGE_SIZE)
					break;
			}
			err = aios_http_wb_put_run(c, r, oid, pg, i, j);
		}
		mutex_unlock(mu);
		return err;
	}

	/* Many scattered pages: one GET → patch → PUT. */
	err = aios_http_get(c, oid, &body, NULL);
	if (err && err != -ENOENT) {
		mutex_unlock(mu);
		return err;
	}
	if (body.len > nlen)
		nlen = body.len;
	nb = kvmalloc(nlen, GFP_KERNEL);
	if (!nb) {
		aios_http_buf_free(&body);
		mutex_unlock(mu);
		return -ENOMEM;
	}
	memset(nb, 0, nlen);
	if (body.len && body.data)
		memcpy(nb, body.data, body.len);
	aios_http_buf_free(&body);
	for (i = 0; i < cw->npages; i++) {
		if (!pg[i].len)
			continue;
		memcpy(nb + ((u64)pg[i].pos - base), pg[i].data, pg[i].len);
	}
	err = aios_http_put(c, oid, nb, nlen, NULL, NULL);
	kvfree(nb);
	mutex_unlock(mu);
	return err;
}

static void aios_http_wb_chunk_work(struct work_struct *work)
{
	struct aios_http_wb_chunk *cw = container_of(work, struct aios_http_wb_chunk, work);
	struct aios_http_wb_round *r = cw->round;
	struct aios_http_client *c;
	unsigned int noio;
	unsigned int i;
	int err;

	/* This is writeback: every allocation below, including the ones inside
	 * aios_http, must not be allowed to recurse into reclaim and wait on the
	 * very pages this item was queued to write out. The flag is per-task, so
	 * it has to be set here rather than inherited from the caller. */
	noio = memalloc_noio_save();
	c = http_client_get(r->info);
	err = aios_http_wb_chunk_apply(c, cw);
	http_client_put(r->info, c);
	memalloc_noio_restore(noio);
	cw->err = err;
	for (i = 0; i < cw->npages; i++)
		aios_http_wb_end_page(r, &cw->pages[i], err);
	if (atomic_dec_and_test(&r->pending))
		complete(&r->done);
}

#ifdef AIOS_HAS_FOLIO_AOPS
static int aios_http_wb_collect(struct folio *folio, struct writeback_control *wbc, void *data)
{
	struct page *page = folio_page(folio, 0);
#else
static int aios_http_wb_collect(struct page *page, struct writeback_control *wbc, void *data)
{
#endif
	struct aios_http_wb_round *r = data;
	struct inode *inode = page->mapping->host;
	loff_t pos = page_offset(page);
	loff_t i_size = i_size_read(inode);
	struct aios_http_wb_page *wp;
	void *kaddr;

	if (r->n >= r->cap) {
		/* Round full — leave dirty; the next round picks it up. */
		redirty_page_for_writepage(wbc, page);
		unlock_page(page);
		return 0;
	}

	set_page_writeback(page);
	wp = &r->pages[r->n];
	wp->page = page;
	wp->pos = pos;
	wp->len = 0;
	wp->data = r->buf + (size_t)r->n * PAGE_SIZE;
	if (pos < i_size) {
		wp->len = min_t(loff_t, PAGE_SIZE, i_size - pos);
		kaddr = kmap(page);
		memcpy(wp->data, kaddr, wp->len);
		kunmap(page);
	}
	r->n++;
	unlock_page(page);
	return 0;
}

/*
 * Group a collected round by chunk and queue one work item per chunk. On
 * failure to even start, every page is finished with the error (redirtied).
 */
static int aios_http_wb_round_dispatch(struct aios_http_wb_round *r)
{
	unsigned int i, c;

	for (i = 0; i < r->n; i++) {
		if (r->pages[i].len) {
			r->max_end = max_t(u64, r->max_end, (u64)r->pages[i].pos + r->pages[i].len);
			r->wrote += r->pages[i].len;
		}
	}
	r->nchunks = 1;
	for (i = 1; i < r->n; i++) {
		if (r->pages[i].pos / r->unit != r->pages[i - 1].pos / r->unit)
			r->nchunks++;
	}
	r->chunks = kcalloc(r->nchunks, sizeof(*r->chunks), GFP_KERNEL);
	if (!r->chunks) {
		for (i = 0; i < r->n; i++)
			aios_http_wb_end_page(r, &r->pages[i], -ENOMEM);
		r->err = -ENOMEM;
		complete(&r->done);
		return -ENOMEM;
	}
	c = 0;
	for (i = 0; i < r->n; i++) {
		u64 ch = r->pages[i].pos / r->unit;

		if (i && ch == r->chunks[c].chunk) {
			r->chunks[c].npages++;
			continue;
		}
		if (i)
			c++;
		r->chunks[c].round = r;
		r->chunks[c].chunk = ch;
		r->chunks[c].pages = &r->pages[i];
		r->chunks[c].npages = 1;
		INIT_WORK(&r->chunks[c].work, aios_http_wb_chunk_work);
	}
	atomic_set(&r->pending, r->nchunks);
	for (i = 0; i < r->nchunks; i++)
		queue_work(r->info->wb_wq, &r->chunks[i].work);
	return 0;
}

/* Wait for a dispatched round; returns its first error. */
static int aios_http_wb_round_wait(struct aios_http_wb_round *r)
{
	unsigned int i;

	wait_for_completion(&r->done);
	if (r->err)
		return r->err;
	for (i = 0; i < r->nchunks; i++) {
		if (r->chunks[i].err)
			return r->chunks[i].err;
	}
	return 0;
}

/*
 * Collect one round of dirty pages. write_cache_pages is driven in
 * WB_SYNC_NONE mode with nr_to_write = the round capacity so the scan stops
 * as soon as the round is full instead of walking (and redirtying) every
 * remaining dirty page of a large file each round. For a WB_SYNC_ALL caller
 * tagged_writepages keeps the TOWRITE tagging (pages dirtied after the sync
 * began are not chased) and pages under writeback from our earlier rounds are
 * skipped rather than waited for: the caller's filemap_fdatawait covers them.
 * The caller's nr_to_write is charged for what was collected.
 */
static int aios_http_wb_collect_round(struct address_space *mapping,
				      struct writeback_control *wbc,
				      struct aios_http_wb_round *r, bool *full)
{
	const enum writeback_sync_modes saved_mode = wbc->sync_mode;
	const long saved_nr = wbc->nr_to_write;
	const unsigned int saved_tagged = wbc->tagged_writepages;
	long budget = r->cap;
	int err;

	if (saved_mode == WB_SYNC_NONE && saved_nr < budget)
		budget = saved_nr;
	*full = false;
	if (budget <= 0)
		return 0;

	wbc->sync_mode = WB_SYNC_NONE;
	wbc->nr_to_write = budget;
	if (saved_mode != WB_SYNC_NONE)
		wbc->tagged_writepages = 1;
	err = write_cache_pages(mapping, wbc, aios_http_wb_collect, r);
	*full = wbc->nr_to_write <= 0;
	wbc->sync_mode = saved_mode;
	wbc->tagged_writepages = saved_tagged;
	wbc->nr_to_write = saved_mode == WB_SYNC_NONE ? saved_nr - (budget - wbc->nr_to_write) :
							saved_nr;
	return err;
}

/*
 * ->writepages is called once by do_writepages; for WB_SYNC_ALL (fsync,
 * sync) every page dirty at the start must be written before returning, so
 * rounds continue until a scan finds nothing more (bounded by the number of
 * cached pages, so a writer racing the fsync cannot keep us here). For
 * WB_SYNC_NONE the loop stops once wbc->nr_to_write is exhausted. A failed
 * round leaves its pages redirtied and terminates the loop so an erroring
 * server cannot spin us forever. The size is pushed to the inode object once
 * at the end (or when the dirty budget says so), not per round.
 */
static int aios_http_writepages_noio(struct address_space *mapping,
				     struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_http_wb_round *ring[AIOS_HTTP_WB_INFLIGHT] = { NULL };
	struct aios_inode_aux *aux;
	struct aios_http_client *c;
	unsigned int head = 0, tail = 0, inflight = 0, round, depth;
	unsigned long max_rounds;
	u64 unit, max_end = 0;
	size_t wrote = 0;
	int err = 0;

	if (!info->http_pool || !info->wb_wq)
		return -EINVAL;
	aux = aios_inode_aux_get(inode, NULL);
	if (!aux)
		return -ENOMEM;
	/* Do not sit on a pool client while the ring waits for workers that
	 * each need one (see aios_http_dio). */
	c = http_client_get(info);
	err = http_file_unit(info, c, inode, aux, &unit);
	http_client_put(info, c);
	if (err)
		return err;
	depth = clamp_t(unsigned int, info->pool_size, 1, AIOS_HTTP_WB_INFLIGHT);
	max_rounds = mapping->nrpages / AIOS_HTTP_WB_MIN_PAGES + 2;
	if (max_rounds > AIOS_HTTP_WB_MAX_ROUNDS)
		max_rounds = AIOS_HTTP_WB_MAX_ROUNDS;

	for (round = 0; round < max_rounds; round++) {
		struct aios_http_wb_round *r;
		bool full;
		int cerr;

		if (wbc->sync_mode == WB_SYNC_NONE && wbc->nr_to_write <= 0)
			break;
		r = aios_http_wb_round_alloc(mapping, wbc, aux, unit);
		if (!r) {
			err = -ENOMEM;
			break;
		}
		cerr = aios_http_wb_collect_round(mapping, wbc, r, &full);
		if (!r->n) {
			aios_http_wb_round_free(r);
			err = cerr;
			break;
		}
		if (aios_http_wb_round_dispatch(r)) {
			aios_http_wb_round_free(r);
			err = -ENOMEM;
			break;
		}
		ring[tail] = r;
		tail = (tail + 1) % AIOS_HTTP_WB_INFLIGHT;
		inflight++;
		if (inflight == depth) {
			struct aios_http_wb_round *old = ring[head];

			ring[head] = NULL;
			head = (head + 1) % AIOS_HTTP_WB_INFLIGHT;
			inflight--;
			err = aios_http_wb_round_wait(old);
			if (!err) {
				max_end = max(max_end, old->max_end);
				wrote += old->wrote;
			}
			aios_http_wb_round_free(old);
			if (err)
				break;
		}
		if (cerr) {
			err = cerr;
			break;
		}
		/* A round that did not fill up means the scan saw the whole
		 * range without us skipping anything. */
		if (!full)
			break;
	}

	while (inflight) {
		struct aios_http_wb_round *old = ring[head];
		int werr;

		ring[head] = NULL;
		head = (head + 1) % AIOS_HTTP_WB_INFLIGHT;
		inflight--;
		werr = aios_http_wb_round_wait(old);
		if (!werr) {
			max_end = max(max_end, old->max_end);
			wrote += old->wrote;
		} else if (!err) {
			err = werr;
		}
		aios_http_wb_round_free(old);
	}

	if (max_end) {
		int serr;

		/* The pages' writeback has ended, so a truncate may already have
		 * shrunk i_size below max_end: never grow i_size from here, only
		 * publish whatever it is now. On fsync/sync publish it always;
		 * otherwise let the dirty budget decide, as for direct writes. */
		mark_http_size_dirty(inode, 0, wrote);
		if (wbc->sync_mode == WB_SYNC_ALL || http_should_flush_size(aux)) {
			c = http_client_get(info);
			serr = http_flush_size(info, c, inode, aux, (u64)i_size_read(inode));
			http_client_put(info, c);
		} else {
			serr = 0;
		}
		if (serr && !err)
			err = serr;
	}
	return err;
}

int aios_http_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	/* Everything below writes dirty pages out over a socket. Allocations on
	 * that path must not re-enter reclaim, which would wait on writeback of
	 * the pages this call is holding. */
	unsigned int noio;
	int err;

	noio = memalloc_noio_save();
	err = aios_http_writepages_noio(mapping, wbc);
	memalloc_noio_restore(noio);
	return err;
}

/*
 * Push the local i_size to the server. Only write_inode / evict_inode reach
 * this (deferred size flush after chunk writes); explicit truncates go
 * through http_setattr. The local size therefore never shrinks the server
 * copy: another client may legitimately have extended the file meanwhile.
 */
int aios_http_io_set_size(struct inode *inode, loff_t size)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_aux *aux;
	struct aios_http_client *c;
	int err;

	aux = aios_inode_aux_get(inode, NULL);
	if (!aux)
		return -ENOMEM;
	c = http_client_get(info);
	err = http_flush_size(info, c, inode, aux, (u64)size);
	http_client_put(info, c);
	return err;
}

/*
 * Best-effort punch hole with KEEP_SIZE: zero overlapping chunk ranges. A
 * fully covered chunk is deleted (reads of a missing chunk are zeros); a
 * partial one gets a ranged PUT of zeros, so the hole costs one round trip
 * per chunk and moves only the punched bytes. Runs on a pool client under
 * the stripe locks and meta_mu, never http_mu.
 */
int aios_http_io_punch(struct inode *inode, loff_t offset, loff_t len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m = { 0 };
	struct aios_inode_aux *aux;
	struct aios_http_client *c;
	u64 unit, start, end, p;
	char *zeros = NULL;
	size_t zeros_len = 0;
	int attempt;
	int err;

	if (offset < 0 || len <= 0)
		return -EINVAL;
	aux = aios_inode_aux_get(inode, NULL);
	if (!aux)
		return -ENOMEM;

	c = http_client_get(info);
	err = http_file_unit(info, c, inode, aux, &unit);
	if (err)
		goto out;
	start = (u64)offset;
	end = min_t(u64, (u64)offset + (u64)len, (u64)i_size_read(inode));
	p = start;
	while (p < end) {
		u64 chunk = p / unit;
		u64 chunk_off = p % unit;
		u64 chunk_end = min_t(u64, end, (chunk + 1) * unit);
		size_t n = (size_t)(chunk_end - p);
		struct mutex *mu = aios_chunk_lock(aux, chunk);
		char oid[160];

		oid_chunk(info->volume, inode->i_ino, chunk, oid, sizeof(oid));
		if (chunk_off == 0 && n == unit) {
			mutex_lock(mu);
			err = aios_http_delete(c, oid);
			mutex_unlock(mu);
			if (err && err != -ENOENT)
				goto out;
			err = 0;
			p += n;
			continue;
		}
		if (zeros_len < n) {
			kvfree(zeros);
			zeros = kvzalloc(n, GFP_KERNEL);
			if (!zeros) {
				err = -ENOMEM;
				goto out;
			}
			zeros_len = n;
		}
		mutex_lock(mu);
		err = http_chunk_put(c, oid, unit, chunk_off, zeros, n);
		mutex_unlock(mu);
		if (err)
			goto out;
		p += n;
	}

	mutex_lock(&aux->meta_mu);
	err = -EAGAIN;
	for (attempt = 0; attempt < AIOS_HTTP_META_RETRIES && err == -EAGAIN; attempt++) {
		if (attempt)
			http_retry_backoff(attempt);
		err = load_inode_c(info, c, inode->i_ino, &m);
		if (err)
			break;
		m.ctime_ns = now_ns();
		err = store_inode_c(info, c, &m);
		if (!err)
			attach_iinfo(inode, &m);
	}
	mutex_unlock(&aux->meta_mu);
out:
	kvfree(zeros);
	inode_meta_reset(&m);
	http_client_put(info, c);
	return http_no_eagain(err);
}

/*
 * Xattrs travel inside the inode object, which every lookup / getattr /
 * setattr already loads into the aux, so a getxattr or listxattr within the
 * attribute TTL is served from that copy without a round trip (the same
 * window in which getattr trusts its cached attributes). Past the TTL, or when
 * the aux was never filled, one GET on a pool client refreshes both.
 */
static int http_xattrs_read(struct inode *inode, struct aios_xa_ent **ents, unsigned int *n)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_aux *aux = inode->i_private;
	struct aios_inode_meta m = { 0 };
	struct aios_http_client *c;
	char *xobj;
	bool fresh = false;
	int err;

	if (aux) {
		xobj = aux_extra_dup(aux, false, msecs_to_jiffies(aios_attr_ttl_ms(inode->i_sb)),
				     &fresh);
		if (fresh) {
			err = parse_xattrs_object(xobj, ents, n);
			kfree(xobj);
			return err;
		}
	}
	c = http_client_get(info);
	err = load_inode_c(info, c, inode->i_ino, &m);
	http_client_put(info, c);
	if (err)
		return err;
	attach_iinfo(inode, &m);
	err = parse_xattrs_object(m.xattrs_obj, ents, n);
	inode_meta_reset(&m);
	return err;
}

/*
 * Load → edit → CAS PUT the inode object's xattrs on a pool client under
 * meta_mu; @edit mutates the entry array and returns 0, or -errno to abort.
 * A CAS conflict (nlink change, size flush) is retried on a fresh load.
 */
static int http_xattrs_update(struct inode *inode,
			      int (*edit)(struct aios_xa_ent *ents, unsigned int *n, void *arg),
			      void *arg)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m = { 0 };
	struct aios_inode_aux *aux;
	struct aios_http_client *c;
	int attempt;
	int err = -EAGAIN;

	aux = aios_inode_aux_get(inode, NULL);
	if (!aux)
		return -ENOMEM;
	c = http_client_get(info);
	mutex_lock(&aux->meta_mu);
	for (attempt = 0; attempt < AIOS_HTTP_META_RETRIES && err == -EAGAIN; attempt++) {
		struct aios_xa_ent *ents = NULL;
		unsigned int n = 0;

		if (attempt)
			http_retry_backoff(attempt);
		err = load_inode_c(info, c, inode->i_ino, &m);
		if (err)
			break;
		err = parse_xattrs_object(m.xattrs_obj, &ents, &n);
		if (err)
			break;
		if (!ents) {
			ents = kcalloc(AIOS_HTTP_MAX_XATTRS, sizeof(*ents), GFP_KERNEL);
			if (!ents) {
				err = -ENOMEM;
				break;
			}
		}
		err = edit(ents, &n, arg);
		if (!err) {
			kfree(m.xattrs_obj);
			m.xattrs_obj = NULL;
			if (n)
				err = build_xattrs_object(ents, n, &m.xattrs_obj);
		}
		free_xa_ents(ents, n);
		if (err)
			break;
		m.extras_loaded = true;
		m.ctime_ns = now_ns();
		err = store_inode_c(info, c, &m);
		if (!err)
			attach_iinfo(inode, &m);
	}
	mutex_unlock(&aux->meta_mu);
	http_client_put(info, c);
	inode_meta_reset(&m);
	return http_no_eagain(err);
}

int aios_http_getxattr(struct inode *inode, const char *name, void *buf, size_t size)
{
	struct aios_xa_ent *ents = NULL;
	unsigned int n = 0, i;
	int err;

	if (!name || !*name || strlen(name) > AIOS_KABI_NAME_MAX)
		return -EINVAL;

	err = http_xattrs_read(inode, &ents, &n);
	if (err)
		return err;
	err = -ENODATA;
	for (i = 0; i < n; i++) {
		if (!strcmp(ents[i].name, name)) {
			if (size == 0) {
				err = (int)ents[i].value_len;
			} else if (size < ents[i].value_len) {
				err = -ERANGE;
			} else {
				if (buf && ents[i].value_len)
					memcpy(buf, ents[i].value, ents[i].value_len);
				err = (int)ents[i].value_len;
			}
			break;
		}
	}
	free_xa_ents(ents, n);
	return err;
}

struct http_setxattr_arg {
	const char *name;
	const void *buf;
	size_t size;
	int flags;
};

static int http_setxattr_edit(struct aios_xa_ent *ents, unsigned int *n_inout, void *varg)
{
	struct http_setxattr_arg *a = varg;
	unsigned int n = *n_inout, i;
	bool present = false;

	for (i = 0; i < n; i++) {
		if (!strcmp(ents[i].name, a->name)) {
			present = true;
			break;
		}
	}
	if ((a->flags & AIOS_KABI_XATTR_CREATE) && present)
		return -EEXIST;
	if ((a->flags & AIOS_KABI_XATTR_REPLACE) && !present)
		return -ENODATA;
	if (!present) {
		if (n >= AIOS_HTTP_MAX_XATTRS)
			return -ENOSPC;
		i = n++;
		strscpy(ents[i].name, a->name, sizeof(ents[i].name));
		ents[i].value = NULL;
		ents[i].value_len = 0;
	}
	kfree(ents[i].value);
	ents[i].value = NULL;
	ents[i].value_len = 0;
	if (a->size) {
		ents[i].value = kmemdup(a->buf, a->size, GFP_KERNEL);
		if (!ents[i].value) {
			*n_inout = n;
			return -ENOMEM;
		}
		ents[i].value_len = a->size;
	}
	*n_inout = n;
	return 0;
}

int aios_http_setxattr(struct inode *inode, const char *name, const void *buf, size_t size,
		       int flags)
{
	struct http_setxattr_arg a = { .name = name, .buf = buf, .size = size, .flags = flags };

	if (!name || !*name || strlen(name) > AIOS_KABI_NAME_MAX)
		return -EINVAL;
	if (size > AIOS_HTTP_MAX_XATTR_VALUE)
		return -E2BIG;
	if (size && !buf)
		return -EINVAL;
	return http_xattrs_update(inode, http_setxattr_edit, &a);
}

int aios_http_listxattr(struct inode *inode, char *list, size_t size)
{
	struct aios_xa_ent *ents = NULL;
	unsigned int n = 0, i;
	size_t need = 0, off = 0;
	int err;

	err = http_xattrs_read(inode, &ents, &n);
	if (err)
		return err;
	for (i = 0; i < n; i++)
		need += strlen(ents[i].name) + 1;
	if (size == 0) {
		err = (int)need;
		goto out_free;
	}
	if (size < need) {
		err = -ERANGE;
		goto out_free;
	}
	for (i = 0; i < n; i++) {
		size_t nl = strlen(ents[i].name);

		if (list) {
			memcpy(list + off, ents[i].name, nl);
			list[off + nl] = '\0';
		}
		off += nl + 1;
	}
	err = (int)need;
out_free:
	free_xa_ents(ents, n);
	return err;
}

static int http_removexattr_edit(struct aios_xa_ent *ents, unsigned int *n_inout, void *varg)
{
	const char *name = varg;
	unsigned int n = *n_inout, i;

	for (i = 0; i < n; i++) {
		if (!strcmp(ents[i].name, name)) {
			kfree(ents[i].value);
			ents[i] = ents[n - 1];
			ents[n - 1].value = NULL;
			*n_inout = n - 1;
			return 0;
		}
	}
	return -ENODATA;
}

int aios_http_removexattr(struct inode *inode, const char *name)
{
	if (!name || !*name || strlen(name) > AIOS_KABI_NAME_MAX)
		return -EINVAL;
	return http_xattrs_update(inode, http_removexattr_edit, (void *)name);
}

static int http_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct aios_sb_info *info = AIOS_SB(dentry->d_sb);
	u64 su = info->stripe_unit ? info->stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;

	buf->f_type = AIOSFS_MAGIC;
	buf->f_bsize = su;
	buf->f_blocks = 1ull << 40;
	buf->f_bfree = 1ull << 39;
	buf->f_bavail = 1ull << 39;
	buf->f_files = 1ull << 20;
	buf->f_ffree = 1ull << 19;
	buf->f_namelen = AIOS_KABI_NAME_MAX;
	return 0;
}

static void http_put_super(struct super_block *sb)
{
	struct aios_sb_info *info = AIOS_SB(sb);

	if (!info)
		return;
	/* Queued directory operations must be on the server before the
	 * connections go away; this also hands every lease back. */
	dir_lease_destroy_all(info);
	if (info->wb_wq) {
		/* Chunk work dereferences info->http_pool and info->volume, so it has
		 * to be drained before either goes away. */
		destroy_workqueue(info->wb_wq);
		info->wb_wq = NULL;
	}
	if (info->http_pool) {
		if (info->http) {
			aios_http_pool_put(info->http_pool, info->http);
			info->http = NULL;
		}
		aios_http_pool_destroy(info->http_pool);
		info->http_pool = NULL;
	} else if (info->http) {
		aios_http_client_destroy(info->http);
		info->http = NULL;
	}
	dir_cache_free_all(info);
	memzero_explicit(info->cluster_key, sizeof(info->cluster_key));
	kfree(info);
	sb->s_fs_info = NULL;
}

/* syncfs(2): every directory's queued operations must be on the server. */
static int http_sync_fs(struct super_block *sb, int wait)
{
	if (!wait)
		return 0;
	return dir_lease_sync_all(AIOS_SB(sb), false);
}

static const struct super_operations aios_http_super_ops = {
	.statfs = http_statfs,
	.evict_inode = aios_evict_inode,
	.write_inode = aios_write_inode,
	.sync_fs = http_sync_fs,
	.put_super = http_put_super,
	.show_options = aios_show_options,
};

int aios_fill_super_http(struct super_block *sb, struct aios_sb_info *info)
{
	struct aios_http_pool *pool;
	struct aios_http_client *c;
	struct aios_inode_meta root = { 0 };
	struct inode *root_inode;
	int err;

	sb->s_magic = AIOSFS_MAGIC;
	sb->s_op = &aios_http_super_ops;
	sb->s_d_op = &aios_dentry_ops;
	sb->s_xattr = aios_xattr_handlers;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_blocksize = 4096;
	sb->s_blocksize_bits = 12;
	sb->s_time_gran = 1;
	sb->s_fs_info = info;
	info->backend = AIOS_BACKEND_HTTP;
	info->mount_id = -1;
	info->conn = NULL;
	if (!info->pool_size)
		info->pool_size = AIOSFS_HTTP_POOL_DEFAULT;
	if (!info->attr_ttl_ms)
		info->attr_ttl_ms = AIOS_DENTRY_TTL_MS;
	mutex_init(&info->http_mu);
	mutex_init(&info->dir_cache_mu);
	mutex_init(&info->ino_mu);
	INIT_LIST_HEAD(&info->leases);
	info->nleases = 0;
	info->dir_cache = kzalloc(sizeof(*info->dir_cache), GFP_KERNEL);
	if (!info->dir_cache)
		return -ENOMEM;

	/* One connection is held for namespace operations (info->http); the
	 * other pool_size serve the data path in parallel. */
	pool = aios_http_pool_create(info->endpoint, info->cluster_key,
				     info->app_label[0] ? info->app_label : NULL,
				     info->pool_size + 1, GFP_KERNEL);
	if (IS_ERR(pool)) {
		dir_cache_free_all(info);
		return PTR_ERR(pool);
	}
	aios_http_pool_set_timeout_ms(pool, 30000);
	/* The pool may have clamped the count; everything sized off pool_size
	 * (workqueue, fan-outs, show_options) must see what really exists. */
	info->pool_size = aios_http_pool_size(pool) - 1;
	if (info->principal[0]) {
		/* Trades the principal key for a ticket now: a wrong key fails the
		 * mount instead of every later I/O. */
		err = aios_http_pool_set_principal(pool, info->principal);
		if (err) {
			pr_err("aiosfs: authentication as principal %s failed: %d\n",
			       info->principal, err);
			aios_http_pool_destroy(pool);
			dir_cache_free_all(info);
			return err;
		}
	}
	info->http_pool = pool;
	/* max_active tracks the client pool: extra workers would only pile up
	 * blocked in aios_http_pool_get, one kernel stack each. */
	info->wb_wq = alloc_workqueue("aiosfs-wb", WQ_MEM_RECLAIM | WQ_UNBOUND,
				      info->pool_size);
	if (!info->wb_wq) {
		aios_http_pool_destroy(pool);
		info->http_pool = NULL;
		dir_cache_free_all(info);
		return -ENOMEM;
	}
	c = aios_http_pool_get(pool);
	if (!c) {
		err = -ENOMEM;
		goto fail;
	}
	info->http = c;

	/* Own bdi so read-ahead is sized to the stripe unit: a sequential
	 * reader then fetches whole chunks per round trip via ->readahead. */
	err = super_setup_bdi(sb);
	if (err)
		goto fail;
	{
		u64 su = info->stripe_unit ? info->stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
		unsigned long ra = (unsigned long)(su >> PAGE_SHIFT);

		sb->s_bdi->ra_pages = clamp_t(unsigned long, ra, VM_READAHEAD_PAGES,
					      AIOS_HTTP_WB_MAX_PAGES);
		sb->s_bdi->io_pages = sb->s_bdi->ra_pages;
	}

	err = ensure_super(info);
	if (err)
		goto fail;
	err = ensure_root(info);
	if (err)
		goto fail;
	err = load_inode(info, AIOS_HTTP_ROOT_INO, &root);
	if (err)
		goto fail;
	root_inode = aios_http_iget(sb, &root);
	if (IS_ERR(root_inode)) {
		err = PTR_ERR(root_inode);
		goto fail;
	}
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		err = -ENOMEM;
		goto fail;
	}
	aios_d_mark_fresh(sb->s_root);
	inode_meta_reset(&root);
	pr_info("aiosfs: mounted with backend=http (pool=%u actimeo=%ums)\n", info->pool_size,
		info->attr_ttl_ms);
	return 0;

fail:
	inode_meta_reset(&root);
	if (info->wb_wq) {
		/* Nothing can be queued yet, but destroy before the pool the work
		 * items reach into. */
		destroy_workqueue(info->wb_wq);
		info->wb_wq = NULL;
	}
	if (info->http && info->http_pool)
		aios_http_pool_put(info->http_pool, info->http);
	info->http = NULL;
	if (info->http_pool)
		aios_http_pool_destroy(info->http_pool);
	info->http_pool = NULL;
	dir_cache_free_all(info);
	return err;
}
