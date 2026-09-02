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
#include <linux/pagemap.h>
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
#define AIOS_DIR_CACHE_TTL_MS 250
#define AIOS_DIRTY_FLUSH_BYTES (4ull * 1024 * 1024)
#define AIOS_DIRTY_FLUSH_MS 100

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif

#define AIOS_HTTP_DIR_RETRIES 8
#define AIOS_HTTP_TXN_ID_LEN 128

static char *extract_xattrs_object(const char *js);
static void http_clear_size_dirty(struct inode *inode, u64 size);

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
	if (*pp + 4 > end)
		return false;
	memcpy(v, *pp, 4);
	*pp += 4;
	return true;
}

static bool read_le64(const char **pp, const char *end, u64 *v)
{
	if (*pp + 8 > end)
		return false;
	memcpy(v, *pp, 8);
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
		char a1[64];
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
	unsigned int i, victim = 0;
	unsigned long oldest;

	if (!info->dir_cache || !dt->ents)
		return;
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
	if (dt->count) {
		copy = kmemdup(dt->ents, dt->count * sizeof(*dt->ents), GFP_KERNEL);
		if (!copy)
			return;
	}
	slot = &info->dir_cache->slots[victim];
	kfree(slot->ents);
	slot->ino = dt->ino;
	slot->ents = copy;
	slot->count = dt->count;
	slot->next_op = dt->next_op;
	slot->log_bytes = dt->log_bytes;
	slot->snapshot_op = dt->snapshot_op;
	slot->meta_cas = dt->meta_cas;
	slot->loaded = jiffies;
}

static bool dir_cache_lookup(struct aios_sb_info *info, struct aios_dir_table *dt)
{
	unsigned int i;

	if (!info->dir_cache)
		return false;
	for (i = 0; i < AIOS_DIR_CACHE_SLOTS; i++) {
		struct aios_dir_cache_slot *slot = &info->dir_cache->slots[i];

		if (!slot->ino || slot->ino != dt->ino)
			continue;
		if (!time_before(jiffies, slot->loaded + msecs_to_jiffies(AIOS_DIR_CACHE_TTL_MS)))
			continue;
		if (slot->count > AIOS_HTTP_MAX_DIR_ENTS)
			return false;
		memcpy(dt->ents, slot->ents, slot->count * sizeof(*slot->ents));
		dt->count = slot->count;
		dt->next_op = slot->next_op;
		dt->log_bytes = slot->log_bytes;
		dt->snapshot_op = slot->snapshot_op;
		dt->meta_cas = slot->meta_cas;
		return true;
	}
	return false;
}

static int dir_load(struct aios_sb_info *info, struct aios_dir_table *dt, bool allow_cache)
{
	struct aios_http_buf body = { 0 };
	int err;

	if (allow_cache && dir_cache_lookup(info, dt))
		return 0;

	dt->count = 0;
	dt->next_op = 1;
	dt->log_bytes = 0;
	dt->snapshot_op = 0;
	dt->meta_cas = 0;

	err = aios_http_get(info->http, dt->meta_oid, &body, &dt->meta_cas);
	if (err == -ENOENT) {
		if (allow_cache)
			dir_cache_publish(info, dt);
		return 0;
	}
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
		err = aios_http_get(info->http, dt->snap_oid, &body, NULL);
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

	if (dt->log_bytes == 0) {
		if (allow_cache)
			dir_cache_publish(info, dt);
		return 0;
	}
	err = aios_http_get_range(info->http, dt->log_oid, 0, dt->log_bytes - 1, &body);
	if (err == -ENOENT) {
		if (allow_cache)
			dir_cache_publish(info, dt);
		return 0;
	}
	if (err)
		return err;
	err = decode_and_apply_log(dt, body.data, body.len, dt->snapshot_op);
	aios_http_buf_free(&body);
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

static void release_held_locks(struct aios_sb_info *info, struct aios_held_lock *locks,
			       unsigned int n)
{
	while (n--) {
		if (locks[n].token[0])
			aios_http_lock_release(info->http, locks[n].oid, locks[n].token);
		locks[n].token[0] = '\0';
	}
}

static int acquire_sorted_locks(struct aios_sb_info *info, struct aios_held_lock *locks,
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
		err = aios_http_lock_acquire(info->http, locks[i].oid, 30000, locks[i].token,
					     sizeof(locks[i].token));
		if (err) {
			release_held_locks(info, locks, i);
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

static int txn_put_dir(struct aios_sb_info *info, const char *txn_id, struct aios_dir_table *dt,
		       struct aios_held_lock *locks, unsigned int nlocks)
{
	char *snap = NULL;
	char *meta = NULL;
	u64 meta_cas;
	int err;

	err = dir_plan_compact(dt, &snap, &meta);
	if (err)
		return err;
	err = aios_http_txn_prepare_put(info->http, txn_id, dt->snap_oid, snap, strlen(snap),
					token_for(locks, nlocks, dt->snap_oid), NULL);
	if (err)
		goto out;
	err = aios_http_txn_prepare_put(info->http, txn_id, dt->log_oid, "", 0,
					token_for(locks, nlocks, dt->log_oid), NULL);
	if (err)
		goto out;
	meta_cas = dt->meta_cas;
	err = aios_http_txn_prepare_put(info->http, txn_id, dt->meta_oid, meta, strlen(meta),
					token_for(locks, nlocks, dt->meta_oid), &meta_cas);
	if (!err)
		dt->meta_cas = meta_cas;
out:
	kfree(snap);
	kfree(meta);
	return err;
}

static int dir_find(struct aios_dir_table *dt, const char *name, u64 *ino_out);

/*
 * Commit one directory operation as a compact rewrite of the directory tip
 * (snapshot + empty log + meta) under the directory's object locks and a
 * /txn, the same protocol userspace libaios_posix uses. The table is reloaded
 * under the locks so a peer's append between the caller's load and here is
 * not lost, and the meta CAS conflict (-EAGAIN) is retried a few times.
 *
 * must_be_absent: LINK fails with -EEXIST if a0 already exists.
 * UNLINK / RENAME fail with -ENOENT if a0 vanished under the lock.
 */
static int dir_commit_op(struct aios_sb_info *info, struct aios_dir_table *dt, u32 op,
			 const char *a0, const char *a1, bool must_be_absent)
{
	struct aios_held_lock *locks;
	char *txn_id;
	unsigned int nlocks;
	int attempt;
	int err = -EAGAIN;

	locks = kcalloc(3, sizeof(*locks), GFP_KERNEL);
	txn_id = kzalloc(AIOS_HTTP_TXN_ID_LEN, GFP_KERNEL);
	if (!locks || !txn_id) {
		kfree(locks);
		kfree(txn_id);
		return -ENOMEM;
	}

	for (attempt = 0; attempt < AIOS_HTTP_DIR_RETRIES; attempt++) {
		nlocks = 3;
		strscpy(locks[0].oid, dt->meta_oid, sizeof(locks[0].oid));
		strscpy(locks[1].oid, dt->log_oid, sizeof(locks[1].oid));
		strscpy(locks[2].oid, dt->snap_oid, sizeof(locks[2].oid));
		locks[0].token[0] = locks[1].token[0] = locks[2].token[0] = '\0';
		txn_id[0] = '\0';

		err = acquire_sorted_locks(info, locks, &nlocks);
		if (err == -EAGAIN) {
			msleep(20);
			continue;
		}
		if (err)
			break;

		err = dir_load(info, dt, false);
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
		err = apply_dir_op(dt, op, a0, a1);
		if (err)
			goto unlock;

		err = aios_http_txn_begin(info->http, txn_id, AIOS_HTTP_TXN_ID_LEN);
		if (err)
			goto unlock;
		err = txn_put_dir(info, txn_id, dt, locks, nlocks);
		if (!err)
			err = aios_http_txn_commit(info->http, txn_id);
		if (err) {
			aios_http_txn_abort(info->http, txn_id);
			txn_id[0] = '\0';
			release_held_locks(info, locks, nlocks);
			if (err == -EAGAIN) {
				msleep(20);
				continue;
			}
			break;
		}
		dir_cache_publish(info, dt);
		release_held_locks(info, locks, nlocks);
		err = 0;
		break;

unlock:
		release_held_locks(info, locks, nlocks);
		break;
	}
	kfree(locks);
	kfree(txn_id);
	return err;
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

static int load_inode(struct aios_sb_info *info, u64 ino, struct aios_inode_meta *m)
{
	char oid[160];
	struct aios_http_buf body = { 0 };
	char *js;
	u64 cas = 0;
	int err;

	oid_ino(info->volume, ino, oid, sizeof(oid));
	err = aios_http_get(info->http, oid, &body, &cas);
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

static int store_inode(struct aios_sb_info *info, struct aios_inode_meta *m)
{
	char oid[160];
	char *js = NULL;
	size_t jslen = 0;
	int err;

	err = inode_to_json_full(info, m, &js, &jslen);
	if (err)
		return err;
	oid_ino(info->volume, m->ino, oid, sizeof(oid));
	err = aios_http_put(info->http, oid, js, jslen, NULL, &m->cas);
	kfree(js);
	return err;
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

/* Copy aux->symlink under extras_lock. Returns NULL if unset or on ENOMEM. */
static char *aux_symlink_dup(struct aios_inode_aux *aux)
{
	char *s = NULL;
	size_t cap = 0;

	for (;;) {
		size_t len;

		spin_lock(&aux->extras_lock);
		if (!aux->symlink) {
			spin_unlock(&aux->extras_lock);
			kfree(s);
			return NULL;
		}
		len = strlen(aux->symlink);
		if (s && len < cap) {
			memcpy(s, aux->symlink, len + 1);
			spin_unlock(&aux->extras_lock);
			return s;
		}
		spin_unlock(&aux->extras_lock);
		kfree(s);
		cap = len + 1;
		s = kmalloc(cap, GFP_KERNEL);
		if (!s)
			return NULL;
	}
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

static int alloc_ino(struct aios_sb_info *info, u64 *ino_out)
{
	char oid[160];
	int attempt;

	oid_super(info->volume, oid, sizeof(oid));
	for (attempt = 0; attempt < 16; attempt++) {
		struct aios_http_buf body = { 0 };
		char *js;
		char out[256];
		u64 cas = 0, next = 2, stripe = AIOS_HTTP_DEFAULT_STRIPE_UNIT;
		u32 width = AIOS_HTTP_DEFAULT_STRIPE_WIDTH;
		int err;

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

		*ino_out = next++;
		snprintf(out, sizeof(out),
			 "{\"aios_posix_super\":1,\"next_ino\":%llu,\"stripe_unit\":%llu,"
			 "\"stripe_width\":%u,\"uuid\":\"fs-%s\"}",
			 (unsigned long long)next, (unsigned long long)stripe, width,
			 info->volume);
		err = aios_http_put(info->http, oid, out, strlen(out), NULL, &cas);
		if (!err)
			return 0;
		if (err != -EAGAIN)
			return err;
	}
	return -EAGAIN;
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

/* Delete every chunk object of a regular file up to @size bytes. */
static void delete_file_chunks(struct aios_sb_info *info, u64 ino, u64 unit, u64 size)
{
	u64 chunks;
	u64 c;
	char oid[160];

	if (!unit)
		unit = AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	chunks = (size + unit - 1) / unit;
	for (c = 0; c < chunks; c++) {
		oid_chunk(info->volume, ino, c, oid, sizeof(oid));
		aios_http_delete(info->http, oid);
	}
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
	int err;

	err = load_inode(info, ino, &m);
	if (err == -ENOENT)
		return 0;
	if (err)
		return err;
	if (S_ISDIR(m.mode)) {
		inode_meta_reset(&m);
		delete_dir_objects(info, ino);
		return 0;
	}
	m.nlink = m.nlink ? m.nlink - 1 : 0;
	m.ctime_ns = now_ns();
	err = store_inode(info, &m);
	inode_meta_reset(&m);
	return err;
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

	if (!info || !info->http)
		return;
	mutex_lock(&info->http_mu);
	err = load_inode(info, inode->i_ino, &m);
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
	if (S_ISREG(inode->i_mode))
		delete_file_chunks(info, inode->i_ino, unit, size);
	oid_ino(info->volume, inode->i_ino, oid, sizeof(oid));
	aios_http_delete(info->http, oid);
out:
	inode_meta_reset(&m);
	mutex_unlock(&info->http_mu);
}

static int http_refresh(struct inode *inode)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m = { 0 };
	struct aios_kabi_stat st;
	int err;

	err = load_inode(info, inode->i_ino, &m);
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
	int err;

	if (dentry->d_name.len > AIOS_KABI_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(name, dentry->d_name.name, dentry->d_name.len);
	name[dentry->d_name.len] = '\0';
	if (strchr(name, '/'))
		return -EINVAL;

	mutex_lock(&info->http_mu);
	err = load_inode(info, dir->i_ino, &pmeta);
	if (err)
		goto out;
	if (!S_ISDIR(pmeta.mode)) {
		err = -ENOTDIR;
		goto out;
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
	pmeta.mtime_ns = pmeta.ctime_ns = ts;
	if (is_dir)
		pmeta.nlink += 1;
	err = store_inode(info, &pmeta);
	if (err)
		goto out_dt;
	inode = aios_http_iget(dir->i_sb, &m);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_dt;
	}
	d_instantiate(dentry, inode);
	aios_d_mark_fresh(dentry);
	if (is_dir)
		inc_nlink(dir);
	{
		struct aios_kabi_stat st;

		meta_to_stat(&pmeta, &st);
		aios_stat_to_inode(dir, &st);
	}
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
	struct aios_inode_meta m = { 0 };
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
	if (!err && S_ISDIR(m.mode)) {
		err = -EISDIR;
		goto out_dt;
	}
	err = dir_commit_op(info, &dt, AIOS_HTTP_OP_UNLINK, name, NULL, false);
	if (err)
		goto out_dt;
	aios_http_drop_link(info, child);
	drop_nlink(d_inode(dentry));
	d_drop(dentry);
out_dt:
	dir_table_free(&dt);
	inode_meta_reset(&m);
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

	err = dir_commit_op(info, &dt, AIOS_HTTP_OP_UNLINK, name, NULL, false);
	if (err)
		goto out_dt;
	err = load_inode(info, dir->i_ino, &pmeta);
	if (!err) {
		if (pmeta.nlink > 2)
			pmeta.nlink -= 1;
		pmeta.mtime_ns = pmeta.ctime_ns = now_ns();
		store_inode(info, &pmeta);
	}
	delete_dir_objects(info, child);
	clear_nlink(d_inode(dentry));
	drop_nlink(dir);
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

	err = acquire_sorted_locks(info, rc->locks, &nlocks);
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

	err = txn_put_dir(info, rc->txn_id, &rc->old_dir, rc->locks, nlocks);
	if (err)
		goto abort;
	err = txn_put_dir(info, rc->txn_id, &rc->new_dir, rc->locks, nlocks);
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
	release_held_locks(info, rc->locks, nlocks);
	rename_ctx_reset(rc);
	return 0;

abort:
	if (rc->txn_id[0])
		aios_http_txn_abort(info->http, rc->txn_id);
unlock:
	release_held_locks(info, rc->locks, nlocks);
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
	for (attempt = 0; attempt < AIOS_HTTP_DIR_RETRIES; attempt++) {
		err = http_rename_cross_dir_once(info, rc, old_parent, old_name, new_parent,
						 new_name, noreplace);
		if (err != -EAGAIN)
			break;
		msleep(20);
	}
	kfree(rc);
	return err;
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
	err = load_inode(info, dir->i_ino, &np);
	if (err)
		goto out;
	if (!S_ISDIR(np.mode)) {
		err = -ENOTDIR;
		goto out;
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
	np.mtime_ns = np.ctime_ns = ts;
	err = store_inode(info, &np);
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
	}
	aios_fillattr(mnt_userns, inode, stat);
	return 0;
}

static int truncate_file(struct aios_sb_info *info, struct aios_inode_aux *aux,
			 struct aios_inode_meta *m, u64 size)
{
	u64 unit = m->stripe_unit ? m->stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	char oid[160];
	int err;

	if (size < m->size) {
		u64 first_drop = (size + unit - 1) / unit;
		u64 old_chunks = (m->size + unit - 1) / unit;
		u64 c;

		for (c = first_drop; c < old_chunks; c++) {
			struct mutex *mu = aios_chunk_lock(aux, c);

			oid_chunk(info->volume, m->ino, c, oid, sizeof(oid));
			mutex_lock(mu);
			aios_http_delete(info->http, oid);
			mutex_unlock(mu);
		}
		if (size > 0) {
			u64 last = (size - 1) / unit;
			u64 keep = size - last * unit;
			struct mutex *mu = aios_chunk_lock(aux, last);
			struct aios_http_buf body = { 0 };

			oid_chunk(info->volume, m->ino, last, oid, sizeof(oid));
			mutex_lock(mu);
			err = aios_http_get(info->http, oid, &body, NULL);
			if (!err && body.len > keep)
				err = aios_http_put(info->http, oid, body.data, keep, NULL, NULL);
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
	return store_inode(info, m);
}

static int http_setattr(AIOS_IDMAP *mnt_userns, struct dentry *dentry,
			struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m = { 0 };
	struct aios_inode_aux *aux;
	int err;

	err = setattr_prepare(mnt_userns, dentry, attr);
	if (err)
		return err;
	aux = aios_inode_aux_get(inode, NULL);
	if (!aux)
		return -ENOMEM;

	mutex_lock(&info->http_mu);
	err = load_inode(info, inode->i_ino, &m);
	if (err)
		goto out;
	if (attr->ia_valid & ATTR_MODE)
		m.mode = (m.mode & S_IFMT) | (attr->ia_mode & 07777);
	if (attr->ia_valid & ATTR_UID)
		m.uid = aios_iattr_uid(mnt_userns, attr);
	if (attr->ia_valid & ATTR_GID)
		m.gid = aios_iattr_gid(mnt_userns, attr);
	if (attr->ia_valid & ATTR_SIZE) {
		err = truncate_file(info, aux, &m, attr->ia_size);
		if (err)
			goto out;
		truncate_setsize(inode, attr->ia_size);
	} else {
		if (attr->ia_valid & (ATTR_MODE | ATTR_UID | ATTR_GID | ATTR_MTIME | ATTR_ATIME)) {
			if (attr->ia_valid & ATTR_MTIME)
				m.mtime_ns = (u64)attr->ia_mtime.tv_sec * 1000000000ull +
					     attr->ia_mtime.tv_nsec;
			if (attr->ia_valid & ATTR_ATIME)
				m.atime_ns = (u64)attr->ia_atime.tv_sec * 1000000000ull +
					     attr->ia_atime.tv_nsec;
			m.ctime_ns = now_ns();
			err = store_inode(info, &m);
			if (err)
				goto out;
		}
	}
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
	inode_meta_reset(&m);
	mutex_unlock(&info->http_mu);
	return err;
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
	err = load_inode(info, dir->i_ino, &pmeta);
	if (err)
		goto out;
	if (!S_ISDIR(pmeta.mode)) {
		err = -ENOTDIR;
		goto out;
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
	pmeta.mtime_ns = pmeta.ctime_ns = ts;
	err = store_inode(info, &pmeta);
	if (err)
		goto out_dt;
	inode = aios_http_iget(dir->i_sb, &m);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_dt;
	}
	d_instantiate(dentry, inode);
	aios_d_mark_fresh(dentry);
	{
		struct aios_kabi_stat st;

		meta_to_stat(&pmeta, &st);
		aios_stat_to_inode(dir, &st);
	}
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

	/* pos 0,1 are . and ..; entries start at pos 2 */
	for (i = 0; i < dt.count; i++) {
		loff_t pos = (loff_t)i + 2;
		struct aios_inode_meta m = { 0 };
		unsigned char type = DT_UNKNOWN;

		if (ctx->pos > pos)
			continue;
		if (!load_inode(info, dt.ents[i].ino, &m)) {
			if (S_ISDIR(m.mode))
				type = DT_DIR;
			else if (S_ISREG(m.mode))
				type = DT_REG;
			else if (S_ISLNK(m.mode))
				type = DT_LNK;
		}
		inode_meta_reset(&m);
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

const struct file_operations aios_http_dir_ops = {
	.owner = THIS_MODULE,
	.iterate_shared = http_readdir,
	.llseek = generic_file_llseek,
};

int aios_http_io_read(struct inode *inode, loff_t pos, void *buf, size_t len, size_t *out_len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m = { 0 };
	struct aios_inode_aux *ii = inode->i_private;
	u64 unit, p, end, file_size;
	size_t written = 0;
	int err;

	if (out_len)
		*out_len = 0;
	if (!len)
		return 0;

	mutex_lock(&info->http_mu);
	err = load_inode(info, inode->i_ino, &m);
	if (err)
		goto out;
	if (!S_ISREG(m.mode)) {
		err = -EISDIR;
		goto out;
	}
	/* Chunks may already be written for data whose size bump is still
	 * pending (deferred size flush); trust the larger of the two. */
	file_size = max_t(u64, m.size, (u64)i_size_read(inode));
	if ((u64)pos >= file_size) {
		err = 0;
		goto out;
	}
	end = min_t(u64, (u64)pos + len, file_size);
	unit = m.stripe_unit ? m.stripe_unit :
			       (ii && ii->stripe_unit ? ii->stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT);
	p = (u64)pos;
	memset(buf, 0, len);
	while (p < end) {
		u64 chunk = p / unit;
		u64 chunk_off = p % unit;
		u64 chunk_end = min_t(u64, end, (chunk + 1) * unit);
		size_t n = (size_t)(chunk_end - p);
		char oid[160];
		struct aios_http_buf body = { 0 };

		oid_chunk(info->volume, m.ino, chunk, oid, sizeof(oid));
		err = aios_http_get(info->http, oid, &body, NULL);
		if (!err && chunk_off < body.len) {
			size_t avail = (size_t)(body.len - chunk_off);
			size_t take = min(n, avail);

			memcpy((char *)buf + written, (char *)body.data + chunk_off, take);
		} else if (err && err != -ENOENT) {
			aios_http_buf_free(&body);
			goto out;
		}
		aios_http_buf_free(&body);
		p += n;
		written += n;
		err = 0;
	}
	if (out_len)
		*out_len = written;
out:
	mutex_unlock(&info->http_mu);
	return err;
}

/*
 * Chunk RMW using an explicit client (no http_mu). The inode's stripe mutex
 * for each chunk is held across GET → patch → PUT; every chunk writer
 * (buffered writeback workers, writepage, O_DIRECT, punch, truncate) takes the
 * same lock so concurrent partial updates to one chunk cannot lose each other.
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
		struct aios_http_buf body = { 0 };
		char *nb;
		size_t nlen;

		oid_chunk(volume, ino, chunk, oid, sizeof(oid));
		mutex_lock(mu);
		err = aios_http_get(c, oid, &body, NULL);
		if (err && err != -ENOENT) {
			mutex_unlock(mu);
			return err;
		}
		nlen = max_t(size_t, body.len, chunk_off + n);
		nb = kvmalloc(nlen, GFP_KERNEL);
		if (!nb) {
			aios_http_buf_free(&body);
			mutex_unlock(mu);
			return -ENOMEM;
		}
		memset(nb, 0, nlen);
		if (body.len)
			memcpy(nb, body.data, body.len);
		aios_http_buf_free(&body);
		memcpy(nb + chunk_off, (char *)buf + done, n);
		err = aios_http_put(c, oid, nb, nlen, NULL, NULL);
		kvfree(nb);
		mutex_unlock(mu);
		if (err)
			return err;
		p += n;
		done += n;
	}
	return 0;
}

static void mark_http_size_dirty(struct inode *inode, u64 new_size, size_t wrote)
{
	struct aios_inode_aux *aux = inode->i_private;

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

int aios_http_io_write(struct inode *inode, loff_t pos, const void *buf, size_t len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m = { 0 };
	struct aios_inode_aux *aux;
	u64 unit;
	int err;

	if (!len)
		return 0;

	aux = aios_inode_aux_get(inode, NULL);
	if (!aux)
		return -ENOMEM;

	mutex_lock(&info->http_mu);
	err = load_inode(info, inode->i_ino, &m);
	if (err)
		goto out;
	if (!S_ISREG(m.mode)) {
		err = -EISDIR;
		goto out;
	}
	unit = m.stripe_unit ? m.stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	err = http_chunk_write(info->http, aux, info->volume, m.ino, unit, (u64)pos, buf, len);
	if (err)
		goto out;
	m.size = max_t(u64, m.size, (u64)pos + len);
	m.mtime_ns = m.ctime_ns = now_ns();
	attach_iinfo(inode, &m);
	mark_http_size_dirty(inode, m.size, len);
	if (http_should_flush_size(aux)) {
		err = store_inode(info, &m);
		if (!err)
			http_clear_size_dirty(inode, m.size);
	}
out:
	inode_meta_reset(&m);
	mutex_unlock(&info->http_mu);
	return err;
}

#define AIOS_HTTP_WB_MAX 64
/* Upper bound on collect/flush rounds per ->writepages call; each round that
 * makes no progress or fails terminates the loop earlier. */
#define AIOS_HTTP_WB_MAX_ROUNDS 4096

struct aios_http_wb_page {
	struct page *page;
	loff_t pos;
	size_t len;
	void *data;
};

struct aios_http_wb_chunk {
	struct work_struct work;
	struct aios_sb_info *info;
	struct aios_inode_aux *aux;
	u64 ino;
	u64 unit;
	u64 chunk;
	struct aios_http_wb_page *pages;
	unsigned int npages;
	int err;
	struct completion done;
};

static int aios_http_wb_cmp(const void *a, const void *b)
{
	const struct aios_http_wb_page *pa = a, *pb = b;

	if (pa->pos < pb->pos)
		return -1;
	if (pa->pos > pb->pos)
		return 1;
	return 0;
}

/*
 * Apply every page of one work item to its chunk with a single GET → patch →
 * PUT under the stripe lock. All pages in cw belong to cw->chunk.
 */
static int aios_http_wb_chunk_apply(struct aios_http_client *c, struct aios_http_wb_chunk *cw)
{
	struct mutex *mu = aios_chunk_lock(cw->aux, cw->chunk);
	struct aios_http_buf body = { 0 };
	u64 base = cw->chunk * cw->unit;
	size_t nlen = 0;
	char oid[160];
	char *nb;
	unsigned int i;
	int err;

	for (i = 0; i < cw->npages; i++) {
		u64 end;

		if (!cw->pages[i].len)
			continue;
		end = (u64)cw->pages[i].pos + cw->pages[i].len - base;
		if (end > nlen)
			nlen = (size_t)end;
	}
	if (!nlen)
		return 0;

	oid_chunk(cw->info->volume, cw->ino, cw->chunk, oid, sizeof(oid));
	mutex_lock(mu);
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
		if (!cw->pages[i].len)
			continue;
		memcpy(nb + ((u64)cw->pages[i].pos - base), cw->pages[i].data, cw->pages[i].len);
	}
	err = aios_http_put(c, oid, nb, nlen, NULL, NULL);
	kvfree(nb);
	mutex_unlock(mu);
	return err;
}

static void aios_http_wb_chunk_work(struct work_struct *work)
{
	struct aios_http_wb_chunk *cw = container_of(work, struct aios_http_wb_chunk, work);
	struct aios_http_client *c;
	unsigned int noio;
	int err;

	/* This is writeback: every allocation below, including the ones inside
	 * aios_http, must not be allowed to recurse into reclaim and wait on the
	 * very pages this item was queued to write out. The flag is per-task, so
	 * it has to be set here rather than inherited from the caller. */
	noio = memalloc_noio_save();
	c = aios_http_pool_get(cw->info->http_pool);
	if (!c) {
		memalloc_noio_restore(noio);
		cw->err = -ENOMEM;
		complete(&cw->done);
		return;
	}
	err = aios_http_wb_chunk_apply(c, cw);
	aios_http_pool_put(cw->info->http_pool, c);
	memalloc_noio_restore(noio);
	cw->err = err;
	complete(&cw->done);
}

#ifdef AIOS_HAS_FOLIO_AOPS
static int aios_http_wb_collect(struct folio *folio, struct writeback_control *wbc, void *data)
{
	struct page *page = folio_page(folio, 0);
#else
static int aios_http_wb_collect(struct page *page, struct writeback_control *wbc, void *data)
{
#endif
	struct aios_http_wb_page **pp = data;
	struct aios_http_wb_page *batch = *pp;
	struct inode *inode = page->mapping->host;
	loff_t pos = page_offset(page);
	loff_t i_size = i_size_read(inode);
	void *kaddr;
	void *copy;
	unsigned int n = 0;

	while (n < AIOS_HTTP_WB_MAX && batch[n].page)
		n++;
	if (n >= AIOS_HTTP_WB_MAX) {
		/* Batch full — leave dirty; the next round picks it up. */
		redirty_page_for_writepage(wbc, page);
		unlock_page(page);
		return 0;
	}

	copy = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!copy) {
		redirty_page_for_writepage(wbc, page);
		unlock_page(page);
		return -ENOMEM;
	}

	set_page_writeback(page);
	batch[n].page = page;
	batch[n].pos = pos;
	batch[n].len = 0;
	batch[n].data = copy;
	if (pos < i_size) {
		batch[n].len = min_t(loff_t, PAGE_SIZE, i_size - pos);
		kaddr = kmap(page);
		memcpy(batch[n].data, kaddr, batch[n].len);
		kunmap(page);
	}
	unlock_page(page);
	return 0;
}

/*
 * Write one collected batch (all pages are under writeback). On any failure
 * every page is redirtied so the data is not dropped, then writeback ends.
 */
static int aios_http_wb_flush_batch(struct address_space *mapping, struct writeback_control *wbc,
				    struct aios_inode_aux *aux, struct aios_http_wb_page *batch,
				    unsigned int n)
{
	struct inode *inode = mapping->host;
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m = { 0 };
	struct aios_http_wb_chunk *chunks = NULL;
	unsigned int nchunks = 0, i, c;
	u64 unit, max_end = 0;
	size_t wrote = 0;
	int err;

	sort(batch, n, sizeof(*batch), aios_http_wb_cmp, NULL);
	for (i = 0; i < n; i++) {
		if (batch[i].len) {
			max_end = max_t(u64, max_end, (u64)batch[i].pos + batch[i].len);
			wrote += batch[i].len;
		}
	}

	mutex_lock(&info->http_mu);
	err = load_inode(info, inode->i_ino, &m);
	unit = m.stripe_unit ? m.stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	inode_meta_reset(&m);
	mutex_unlock(&info->http_mu);
	if (err)
		goto finish;

	nchunks = 1;
	for (i = 1; i < n; i++) {
		if (batch[i].pos / unit != batch[i - 1].pos / unit)
			nchunks++;
	}
	chunks = kcalloc(nchunks, sizeof(*chunks), GFP_KERNEL);
	if (!chunks) {
		err = -ENOMEM;
		goto finish;
	}

	c = 0;
	for (i = 0; i < n; i++) {
		u64 ch = batch[i].pos / unit;

		if (i && ch == chunks[c].chunk) {
			chunks[c].npages++;
			continue;
		}
		if (i)
			c++;
		chunks[c].info = info;
		chunks[c].aux = aux;
		chunks[c].ino = inode->i_ino;
		chunks[c].unit = unit;
		chunks[c].chunk = ch;
		chunks[c].pages = &batch[i];
		chunks[c].npages = 1;
		init_completion(&chunks[c].done);
		INIT_WORK(&chunks[c].work, aios_http_wb_chunk_work);
	}

	for (i = 0; i < nchunks; i++)
		queue_work(info->wb_wq, &chunks[i].work);
	for (i = 0; i < nchunks; i++) {
		wait_for_completion(&chunks[i].done);
		if (chunks[i].err && !err)
			err = chunks[i].err;
	}

	if (!err && max_end) {
		mutex_lock(&info->http_mu);
		if (!load_inode(info, inode->i_ino, &m) && S_ISREG(m.mode)) {
			m.size = max_t(u64, m.size, max_end);
			m.mtime_ns = m.ctime_ns = now_ns();
			attach_iinfo(inode, &m);
			mark_http_size_dirty(inode, m.size, wrote);
			if (http_should_flush_size(aux)) {
				err = store_inode(info, &m);
				if (!err)
					http_clear_size_dirty(inode, m.size);
			}
		}
		inode_meta_reset(&m);
		mutex_unlock(&info->http_mu);
	}

finish:
	for (i = 0; i < n; i++) {
		if (err) {
			SetPageError(batch[i].page);
			mapping_set_error(mapping, err);
			redirty_page_for_writepage(wbc, batch[i].page);
		} else {
			ClearPageError(batch[i].page);
		}
		end_page_writeback(batch[i].page);
	}
	kfree(chunks);
	for (i = 0; i < n; i++)
		kfree(batch[i].data);
	return err;
}

/*
 * ->writepages is called once by do_writepages; for WB_SYNC_ALL (fsync,
 * sync) every dirty page in the range must be written before returning, so
 * collect/flush rounds continue until write_cache_pages finds nothing more.
 * For WB_SYNC_NONE the loop stops once wbc->nr_to_write is exhausted (which
 * write_cache_pages accounts). A failed round leaves its pages redirtied and
 * terminates the loop so an erroring server cannot spin us forever.
 */
static int aios_http_writepages_noio(struct address_space *mapping,
				     struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_aux *aux;
	struct aios_http_wb_page *batch;
	unsigned int round;
	int err = 0;

	if (!info->http_pool || !info->wb_wq)
		return -EINVAL;
	aux = aios_inode_aux_get(inode, NULL);
	if (!aux)
		return -ENOMEM;
	batch = kcalloc(AIOS_HTTP_WB_MAX, sizeof(*batch), GFP_KERNEL);
	if (!batch)
		return -ENOMEM;

	for (round = 0; round < AIOS_HTTP_WB_MAX_ROUNDS; round++) {
		unsigned int n = 0;
		int cerr;

		memset(batch, 0, AIOS_HTTP_WB_MAX * sizeof(*batch));
		cerr = write_cache_pages(mapping, wbc, aios_http_wb_collect, &batch);
		while (n < AIOS_HTTP_WB_MAX && batch[n].page)
			n++;
		if (!n) {
			err = cerr;
			break;
		}
		err = aios_http_wb_flush_batch(mapping, wbc, aux, batch, n);
		if (!err)
			err = cerr;
		if (err)
			break;
		/* A batch that did not fill up means write_cache_pages saw the
		 * whole range without us skipping anything. */
		if (n < AIOS_HTTP_WB_MAX)
			break;
		if (wbc->sync_mode == WB_SYNC_NONE && wbc->nr_to_write <= 0)
			break;
	}
	kfree(batch);
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
	struct aios_inode_meta m = { 0 };
	struct aios_inode_aux *aux;
	int err;

	aux = aios_inode_aux_get(inode, NULL);
	if (!aux)
		return -ENOMEM;

	mutex_lock(&info->http_mu);
	err = load_inode(info, inode->i_ino, &m);
	if (err)
		goto out;
	if (!S_ISREG(m.mode)) {
		err = -EISDIR;
		goto out;
	}
	if ((u64)size <= m.size) {
		err = 0;
		attach_iinfo(inode, &m);
		http_clear_size_dirty(inode, m.size);
		goto out;
	}
	err = truncate_file(info, aux, &m, (u64)size);
	if (!err) {
		attach_iinfo(inode, &m);
		http_clear_size_dirty(inode, m.size);
	}
out:
	inode_meta_reset(&m);
	mutex_unlock(&info->http_mu);
	return err;
}

/* Best-effort punch hole with KEEP_SIZE: zero overlapping chunk ranges. */
int aios_http_io_punch(struct inode *inode, loff_t offset, loff_t len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m = { 0 };
	struct aios_inode_aux *aux;
	u64 unit, start, end, p;
	int err;

	if (offset < 0 || len <= 0)
		return -EINVAL;
	aux = aios_inode_aux_get(inode, NULL);
	if (!aux)
		return -ENOMEM;

	mutex_lock(&info->http_mu);
	err = load_inode(info, inode->i_ino, &m);
	if (err)
		goto out;
	if (!S_ISREG(m.mode)) {
		err = -EISDIR;
		goto out;
	}
	if ((u64)offset >= m.size) {
		err = 0;
		goto out;
	}
	unit = m.stripe_unit ? m.stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	start = (u64)offset;
	end = min_t(u64, (u64)offset + (u64)len, m.size);
	p = start;
	while (p < end) {
		u64 chunk = p / unit;
		u64 chunk_off = p % unit;
		u64 chunk_end = min_t(u64, end, (chunk + 1) * unit);
		size_t n = (size_t)(chunk_end - p);
		struct mutex *mu = aios_chunk_lock(aux, chunk);
		char oid[160];
		struct aios_http_buf body = { 0 };
		char *nb;
		size_t nlen;

		oid_chunk(info->volume, m.ino, chunk, oid, sizeof(oid));
		mutex_lock(mu);
		if (chunk_off == 0 && n == unit) {
			/* Entire chunk punched — delete object. */
			err = aios_http_delete(info->http, oid);
			mutex_unlock(mu);
			if (err && err != -ENOENT)
				goto out;
			err = 0;
			p += n;
			continue;
		}
		err = aios_http_get(info->http, oid, &body, NULL);
		if (err == -ENOENT) {
			mutex_unlock(mu);
			err = 0;
			p += n;
			continue;
		}
		if (err) {
			mutex_unlock(mu);
			goto out;
		}
		nlen = body.len;
		if (chunk_off + n > nlen)
			nlen = chunk_off + n;
		nb = kvmalloc(nlen, GFP_KERNEL);
		if (!nb) {
			aios_http_buf_free(&body);
			mutex_unlock(mu);
			err = -ENOMEM;
			goto out;
		}
		memset(nb, 0, nlen);
		if (body.len)
			memcpy(nb, body.data, body.len);
		aios_http_buf_free(&body);
		memset(nb + chunk_off, 0, n);
		err = aios_http_put(info->http, oid, nb, nlen, NULL, NULL);
		kvfree(nb);
		mutex_unlock(mu);
		if (err)
			goto out;
		p += n;
	}
	m.ctime_ns = now_ns();
	err = store_inode(info, &m);
	if (!err)
		attach_iinfo(inode, &m);
out:
	inode_meta_reset(&m);
	mutex_unlock(&info->http_mu);
	return err;
}

static int http_load_xattrs(struct aios_sb_info *info, u64 ino, struct aios_xa_ent **ents,
			    unsigned int *n, u64 *cas_out, char **raw_js_out)
{
	char oid[160];
	struct aios_http_buf body = { 0 };
	char *js;
	char *xobj;
	u64 cas = 0;
	int err;

	oid_ino(info->volume, ino, oid, sizeof(oid));
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
	aios_http_buf_free(&body);
	xobj = extract_xattrs_object(js);
	err = parse_xattrs_object(xobj, ents, n);
	kfree(xobj);
	if (err) {
		kfree(js);
		return err;
	}
	if (cas_out)
		*cas_out = cas;
	if (raw_js_out)
		*raw_js_out = js;
	else
		kfree(js);
	return 0;
}

static int http_store_xattrs(struct aios_sb_info *info, u64 ino, struct aios_xa_ent *ents,
			     unsigned int n, u64 cas, const char *raw_js)
{
	struct aios_inode_meta m = { 0 };
	char *full = NULL;
	size_t flen = 0;
	char oid[160];
	int err;

	err = inode_from_json(raw_js, cas, &m);
	if (err)
		return err;
	inode_load_extras(raw_js, &m);
	kfree(m.xattrs_obj);
	m.xattrs_obj = NULL;
	m.ctime_ns = now_ns();
	if (n) {
		err = build_xattrs_object(ents, n, &m.xattrs_obj);
		if (err) {
			inode_meta_reset(&m);
			return err;
		}
	}
	m.extras_loaded = true;
	err = inode_to_json_full(info, &m, &full, &flen);
	if (err) {
		inode_meta_reset(&m);
		return err;
	}
	oid_ino(info->volume, ino, oid, sizeof(oid));
	err = aios_http_put(info->http, oid, full, flen, NULL, &cas);
	kfree(full);
	inode_meta_reset(&m);
	return err;
}

int aios_http_getxattr(struct inode *inode, const char *name, void *buf, size_t size)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_xa_ent *ents = NULL;
	unsigned int n = 0, i;
	int err;

	if (!name || !*name || strlen(name) > AIOS_KABI_NAME_MAX)
		return -EINVAL;

	mutex_lock(&info->http_mu);
	err = http_load_xattrs(info, inode->i_ino, &ents, &n, NULL, NULL);
	if (err)
		goto out;
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
out:
	mutex_unlock(&info->http_mu);
	return err;
}

int aios_http_setxattr(struct inode *inode, const char *name, const void *buf, size_t size,
		       int flags)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_xa_ent *ents = NULL;
	unsigned int n = 0, i;
	char *raw = NULL;
	u64 cas = 0;
	int err;
	bool present = false;

	if (!name || !*name || strlen(name) > AIOS_KABI_NAME_MAX)
		return -EINVAL;
	if (size > AIOS_HTTP_MAX_XATTR_VALUE)
		return -E2BIG;
	if (size && !buf)
		return -EINVAL;

	mutex_lock(&info->http_mu);
	err = http_load_xattrs(info, inode->i_ino, &ents, &n, &cas, &raw);
	if (err)
		goto out;
	if (!ents) {
		ents = kcalloc(AIOS_HTTP_MAX_XATTRS, sizeof(*ents), GFP_KERNEL);
		if (!ents) {
			err = -ENOMEM;
			goto out_raw;
		}
	}
	for (i = 0; i < n; i++) {
		if (!strcmp(ents[i].name, name)) {
			present = true;
			break;
		}
	}
	if ((flags & AIOS_KABI_XATTR_CREATE) && present) {
		err = -EEXIST;
		goto out_free;
	}
	if ((flags & AIOS_KABI_XATTR_REPLACE) && !present) {
		err = -ENODATA;
		goto out_free;
	}
	if (!present) {
		if (n >= AIOS_HTTP_MAX_XATTRS) {
			err = -ENOSPC;
			goto out_free;
		}
		i = n++;
		strscpy(ents[i].name, name, sizeof(ents[i].name));
		ents[i].value = NULL;
		ents[i].value_len = 0;
	}
	kfree(ents[i].value);
	ents[i].value = NULL;
	ents[i].value_len = 0;
	if (size) {
		ents[i].value = kmemdup(buf, size, GFP_KERNEL);
		if (!ents[i].value) {
			err = -ENOMEM;
			goto out_free;
		}
		ents[i].value_len = size;
	}
	err = http_store_xattrs(info, inode->i_ino, ents, n, cas, raw);
out_free:
	free_xa_ents(ents, n);
out_raw:
	kfree(raw);
out:
	mutex_unlock(&info->http_mu);
	return err;
}

int aios_http_listxattr(struct inode *inode, char *list, size_t size)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_xa_ent *ents = NULL;
	unsigned int n = 0, i;
	size_t need = 0, off = 0;
	int err;

	mutex_lock(&info->http_mu);
	err = http_load_xattrs(info, inode->i_ino, &ents, &n, NULL, NULL);
	if (err)
		goto out;
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
out:
	mutex_unlock(&info->http_mu);
	return err;
}

int aios_http_removexattr(struct inode *inode, const char *name)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_xa_ent *ents = NULL;
	unsigned int n = 0, i;
	char *raw = NULL;
	u64 cas = 0;
	int err;
	bool found = false;

	if (!name || !*name || strlen(name) > AIOS_KABI_NAME_MAX)
		return -EINVAL;

	mutex_lock(&info->http_mu);
	err = http_load_xattrs(info, inode->i_ino, &ents, &n, &cas, &raw);
	if (err)
		goto out;
	for (i = 0; i < n; i++) {
		if (!strcmp(ents[i].name, name)) {
			kfree(ents[i].value);
			ents[i] = ents[n - 1];
			ents[n - 1].value = NULL;
			n--;
			found = true;
			break;
		}
	}
	if (!found) {
		err = -ENODATA;
		goto out_free;
	}
	err = http_store_xattrs(info, inode->i_ino, ents, n, cas, raw);
out_free:
	free_xa_ents(ents, n);
	kfree(raw);
out:
	mutex_unlock(&info->http_mu);
	return err;
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

static const struct super_operations aios_http_super_ops = {
	.statfs = http_statfs,
	.evict_inode = aios_evict_inode,
	.write_inode = aios_write_inode,
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
	mutex_init(&info->http_mu);
	info->dir_cache = kzalloc(sizeof(*info->dir_cache), GFP_KERNEL);
	if (!info->dir_cache)
		return -ENOMEM;

	pool = aios_http_pool_create(info->endpoint, info->cluster_key,
				     info->app_label[0] ? info->app_label : NULL,
				     AIOSFS_HTTP_POOL_SIZE, GFP_KERNEL);
	if (IS_ERR(pool)) {
		dir_cache_free_all(info);
		return PTR_ERR(pool);
	}
	aios_http_pool_set_timeout_ms(pool, 30000);
	info->http_pool = pool;
	/* max_active tracks the client pool: extra workers would only pile up
	 * blocked in aios_http_pool_get, one kernel stack each. */
	info->wb_wq = alloc_workqueue("aiosfs-wb", WQ_MEM_RECLAIM | WQ_UNBOUND,
				      AIOSFS_HTTP_POOL_SIZE);
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
	pr_info("aiosfs: mounted with backend=http (pool=%u)\n", AIOSFS_HTTP_POOL_SIZE);
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
