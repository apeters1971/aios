// SPDX-License-Identifier: GPL-2.0
/*
 * In-kernel POSIX path: aiosfs VFS → aios_http.ko → AIOS cluster.
 * Compatible object layout with userspace libaios_posix (compact dir rewrites).
 */
#include "aiosfs.h"

#include "../aios_http/aios_http_api.h"

#include <linux/completion.h>
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

struct aios_iinfo {
	u64 cas;
	u64 stripe_unit;
	u32 stripe_width;
};

struct aios_dir_ent {
	char name[AIOS_KABI_NAME_MAX + 1];
	u64 ino;
};

struct aios_dir_table {
	struct aios_dir_ent *ents;
	unsigned int count;
	u64 next_op;
	u64 log_bytes;
	u64 snapshot_op;
	u64 meta_cas;
	char meta_oid[160];
	char log_oid[160];
	char snap_oid[160];
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

/* Parse {"entries":{"a":1,"b":2}} — names are unescaped (posix names). */
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
		const char *q;
		size_t nlen;
		u64 ino;

		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ','))
			p++;
		if (p >= end || *p == '}')
			break;
		if (*p != '"')
			return -EINVAL;
		p++;
		q = p;
		while (q < end && *q && *q != '"')
			q++;
		if (q >= end || *q != '"')
			return -EINVAL;
		nlen = q - p;
		if (nlen > AIOS_KABI_NAME_MAX)
			return -ENAMETOOLONG;
		if (dt->count >= AIOS_HTTP_MAX_DIR_ENTS)
			return -ENOSPC;
		memcpy(dt->ents[dt->count].name, p, nlen);
		dt->ents[dt->count].name[nlen] = '\0';
		p = q + 1;
		while (p < end && (*p == ' ' || *p == '\t' || *p == ':'))
			p++;
		if (kstrtou64(p, 10, &ino))
			return -EINVAL;
		dt->ents[dt->count].ino = ino;
		dt->count++;
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

static int dir_load(struct aios_sb_info *info, struct aios_dir_table *dt)
{
	struct aios_http_buf body = { 0 };
	int err;

	dt->count = 0;
	dt->next_op = 1;
	dt->log_bytes = 0;
	dt->snapshot_op = 0;
	dt->meta_cas = 0;

	err = aios_http_get(info->http, dt->meta_oid, &body, &dt->meta_cas);
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

	if (dt->log_bytes == 0)
		return 0;
	err = aios_http_get_range(info->http, dt->log_oid, 0, dt->log_bytes - 1, &body);
	if (err == -ENOENT)
		return 0;
	if (err)
		return err;
	err = decode_and_apply_log(dt, body.data, body.len, dt->snapshot_op);
	aios_http_buf_free(&body);
	return err;
}

/* Allocates *snap_out / *meta_out (caller kfree). Updates dt next_op/snapshot fields. */
static int dir_plan_compact(struct aios_dir_table *dt, char **snap_out, char **meta_out)
{
	char *snap = NULL;
	char *meta = NULL;
	size_t snap_cap;
	size_t pos;
	unsigned int i;
	u64 next = dt->next_op < 2 ? 2 : dt->next_op;
	u64 snap_op = next - 1;

	snap_cap = 32 + dt->count * (AIOS_KABI_NAME_MAX + 32);
	snap = kmalloc(snap_cap, GFP_KERNEL);
	meta = kmalloc(512, GFP_KERNEL);
	if (!snap || !meta) {
		kfree(snap);
		kfree(meta);
		return -ENOMEM;
	}
	pos = scnprintf(snap, snap_cap, "{\"entries\":{");
	for (i = 0; i < dt->count; i++) {
		pos += scnprintf(snap + pos, snap_cap - pos, "%s\"%s\":%llu",
				 i ? "," : "", dt->ents[i].name,
				 (unsigned long long)dt->ents[i].ino);
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
	return 0;
}

static int dir_store_compact(struct aios_sb_info *info, struct aios_dir_table *dt)
{
	char *snap = NULL;
	char *meta = NULL;
	u64 cas;
	int err;

	err = dir_plan_compact(dt, &snap, &meta);
	if (err)
		return err;
	err = aios_http_put(info->http, dt->snap_oid, snap, strlen(snap), NULL, NULL);
	if (err)
		goto out;
	err = aios_http_put(info->http, dt->log_oid, "", 0, NULL, NULL);
	if (err)
		goto out;
	cas = dt->meta_cas;
	err = aios_http_put(info->http, dt->meta_oid, meta, strlen(meta), NULL, &cas);
	if (!err)
		dt->meta_cas = cas;
out:
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

static int inode_from_json(const char *js, u64 cas, struct aios_inode_meta *m)
{
	memset(m, 0, sizeof(*m));
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

	*ents_out = NULL;
	*n_out = 0;
	if (!obj || obj[0] != '{')
		return 0;
	ents = kcalloc(AIOS_HTTP_MAX_XATTRS, sizeof(*ents), GFP_KERNEL);
	if (!ents)
		return -ENOMEM;
	p = obj + 1;
	end = obj + strlen(obj);
	while (p < end && *p) {
		const char *q;
		size_t nlen, vlen;
		char *b64 = NULL;
		u8 *raw = NULL;
		size_t raw_len = 0;
		int err;

		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ','))
			p++;
		if (p >= end || *p == '}')
			break;
		if (*p != '"' || n >= AIOS_HTTP_MAX_XATTRS) {
			free_xa_ents(ents, n);
			return n >= AIOS_HTTP_MAX_XATTRS ? -ENOSPC : -EINVAL;
		}
		p++;
		q = p;
		while (q < end && *q && *q != '"')
			q++;
		if (q >= end || *q != '"') {
			free_xa_ents(ents, n);
			return -EINVAL;
		}
		nlen = q - p;
		if (nlen == 0 || nlen > AIOS_KABI_NAME_MAX) {
			free_xa_ents(ents, n);
			return -EINVAL;
		}
		memcpy(ents[n].name, p, nlen);
		ents[n].name[nlen] = '\0';
		p = q + 1;
		while (p < end && (*p == ' ' || *p == '\t' || *p == ':'))
			p++;
		if (p >= end || *p != '"') {
			free_xa_ents(ents, n);
			return -EINVAL;
		}
		p++;
		q = p;
		while (q < end && *q && *q != '"')
			q++;
		if (q >= end || *q != '"') {
			free_xa_ents(ents, n);
			return -EINVAL;
		}
		vlen = q - p;
		b64 = kmalloc(vlen + 1, GFP_KERNEL);
		if (!b64) {
			free_xa_ents(ents, n);
			return -ENOMEM;
		}
		memcpy(b64, p, vlen);
		b64[vlen] = '\0';
		err = base64_decode(b64, vlen, &raw, &raw_len);
		kfree(b64);
		if (err) {
			free_xa_ents(ents, n);
			return err;
		}
		ents[n].value = raw;
		ents[n].value_len = raw_len;
		n++;
		p = q + 1;
	}
	*ents_out = ents;
	*n_out = n;
	return 0;
}

static int build_xattrs_object(struct aios_xa_ent *ents, unsigned int n, char **out)
{
	size_t cap = 2;
	char *s;
	size_t pos;
	unsigned int i;

	for (i = 0; i < n; i++)
		cap += strlen(ents[i].name) + ((ents[i].value_len + 2) / 3) * 4 + 8;
	s = kmalloc(cap + 16, GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	pos = scnprintf(s, cap + 16, "{");
	for (i = 0; i < n; i++) {
		char *b64 = NULL;
		size_t blen = 0;
		int err = base64_encode(ents[i].value, ents[i].value_len, &b64, &blen);

		if (err) {
			kfree(s);
			return err;
		}
		pos += scnprintf(s + pos, cap + 16 - pos, "%s\"%s\":\"%s\"", i ? "," : "",
				 ents[i].name, b64);
		kfree(b64);
	}
	scnprintf(s + pos, cap + 16 - pos, "}");
	*out = s;
	return 0;
}

/* Build full inode JSON, preserving xattrs from the cluster object when present. */
static int inode_to_json_full(struct aios_sb_info *info, struct aios_inode_meta *m, char **out,
			      size_t *out_len)
{
	char base[512];
	char oid[160];
	struct aios_http_buf body = { 0 };
	char *xattrs = NULL;
	char *js = NULL;
	int err;

	err = inode_to_json(m, base, sizeof(base));
	if (err)
		return err;
	oid_ino(info->volume, m->ino, oid, sizeof(oid));
	err = aios_http_get(info->http, oid, &body, NULL);
	if (!err && body.len) {
		char *tmp = kmalloc(body.len + 1, GFP_KERNEL);

		if (!tmp) {
			aios_http_buf_free(&body);
			return -ENOMEM;
		}
		memcpy(tmp, body.data, body.len);
		tmp[body.len] = '\0';
		xattrs = extract_xattrs_object(tmp);
		kfree(tmp);
	} else if (err == -ENOENT) {
		err = 0;
	}
	aios_http_buf_free(&body);
	if (err)
		return err;
	err = inode_json_merge_xattrs(base, xattrs, &js, out_len);
	kfree(xattrs);
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
		memset(m, 0, sizeof(*m));
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
	struct aios_iinfo *ii = inode->i_private;

	if (!ii) {
		ii = kzalloc(sizeof(*ii), GFP_KERNEL);
		inode->i_private = ii;
	}
	if (ii) {
		ii->cas = m->cas;
		ii->stripe_unit = m->stripe_unit;
		ii->stripe_width = m->stripe_width;
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
	struct aios_inode_meta root;
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
		err = store_inode(info, &root);
	}
	return err;
}

static int drop_nlink(struct aios_sb_info *info, u64 ino)
{
	struct aios_inode_meta m;
	int err;

	err = load_inode(info, ino, &m);
	if (err == -ENOENT)
		return 0;
	if (err)
		return err;
	if (m.nlink > 1) {
		m.nlink -= 1;
		m.ctime_ns = now_ns();
		return store_inode(info, &m);
	}
	if (S_ISREG(m.mode) && m.size) {
		u64 unit = m.stripe_unit ? m.stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
		u64 chunks = (m.size + unit - 1) / unit;
		u64 c;
		char oid[160];

		for (c = 0; c < chunks; c++) {
			oid_chunk(info->volume, ino, c, oid, sizeof(oid));
			aios_http_delete(info->http, oid);
		}
	}
	{
		char oid[160];

		oid_ino(info->volume, ino, oid, sizeof(oid));
		aios_http_delete(info->http, oid);
	}
	return 0;
}

static int http_refresh(struct inode *inode)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m;
	struct aios_kabi_stat st;
	int err;

	err = load_inode(info, inode->i_ino, &m);
	if (err)
		return err;
	meta_to_stat(&m, &st);
	aios_stat_to_inode(inode, &st);
	attach_iinfo(inode, &m);
	return 0;
}

static struct dentry *http_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_dir_table dt;
	struct aios_inode_meta m;
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
	err = dir_load(info, &dt);
	if (err)
		goto out_dt;
	err = dir_find(&dt, name, &child);
	if (err) {
		d_add(dentry, NULL);
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
out:
	mutex_unlock(&info->http_mu);
	if (err)
		return ERR_PTR(err);
	if (!inode)
		return NULL;
	return d_splice_alias(inode, dentry);
}

static int http_create_common(struct inode *dir, struct dentry *dentry, umode_t mode, bool is_dir)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_dir_table dt;
	struct aios_inode_meta pmeta, m;
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
	err = dir_load(info, &dt);
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
	m.uid = info->uid;
	m.gid = info->gid;
	m.atime_ns = m.mtime_ns = m.ctime_ns = ts;
	m.stripe_unit = info->stripe_unit ? info->stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	m.stripe_width = info->stripe_width ? info->stripe_width : AIOS_HTTP_DEFAULT_STRIPE_WIDTH;
	m.cas = 0;
	err = store_inode(info, &m);
	if (err)
		goto out_dt;
	snprintf(inos, sizeof(inos), "%llu", (unsigned long long)ino);
	err = apply_dir_op(&dt, AIOS_HTTP_OP_LINK, name, inos);
	if (err)
		goto out_dt;
	err = dir_store_compact(info, &dt);
	if (err)
		goto out_dt;
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
	mutex_unlock(&info->http_mu);
	return err;
}

static int http_create(struct user_namespace *mnt_userns, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl)
{
	return http_create_common(dir, dentry, mode, false);
}

static int http_mkdir(struct user_namespace *mnt_userns, struct inode *dir,
		      struct dentry *dentry, umode_t mode)
{
	return http_create_common(dir, dentry, mode, true);
}

static int http_unlink(struct inode *dir, struct dentry *dentry)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_dir_table dt;
	struct aios_inode_meta m;
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
	err = dir_load(info, &dt);
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
	err = apply_dir_op(&dt, AIOS_HTTP_OP_UNLINK, name, NULL);
	if (err)
		goto out_dt;
	err = dir_store_compact(info, &dt);
	if (err)
		goto out_dt;
	drop_nlink(info, child);
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
	struct aios_inode_meta m, pmeta;
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
	err = dir_load(info, &dt);
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
	err = dir_load(info, &child_dt);
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

	err = apply_dir_op(&dt, AIOS_HTTP_OP_UNLINK, name, NULL);
	if (err)
		goto out_dt;
	err = dir_store_compact(info, &dt);
	if (err)
		goto out_dt;
	err = load_inode(info, dir->i_ino, &pmeta);
	if (!err) {
		if (pmeta.nlink > 2)
			pmeta.nlink -= 1;
		pmeta.mtime_ns = pmeta.ctime_ns = now_ns();
		store_inode(info, &pmeta);
	}
	{
		char oid[160];

		oid_ino(info->volume, child, oid, sizeof(oid));
		aios_http_delete(info->http, oid);
		oid_dir_meta(info->volume, child, oid, sizeof(oid));
		aios_http_delete(info->http, oid);
		oid_dir_log(info->volume, child, oid, sizeof(oid));
		aios_http_delete(info->http, oid);
		oid_dir_snap(info->volume, child, oid, sizeof(oid));
		aios_http_delete(info->http, oid);
	}
	clear_nlink(d_inode(dentry));
	drop_nlink(dir);
	d_drop(dentry);
	err = 0;
out_dt:
	dir_table_free(&dt);
out:
	mutex_unlock(&info->http_mu);
	return err;
}

static int http_rename_same_dir(struct aios_sb_info *info, u64 parent, const char *old_name,
				const char *new_name)
{
	struct aios_dir_table dt;
	int err;

	err = dir_table_init(&dt, info->volume, parent);
	if (err)
		return err;
	err = dir_load(info, &dt);
	if (err)
		goto out;
	if (dir_find(&dt, old_name, NULL)) {
		err = -ENOENT;
		goto out;
	}
	err = apply_dir_op(&dt, AIOS_HTTP_OP_RENAME, old_name, new_name);
	if (err)
		goto out;
	err = dir_store_compact(info, &dt);
out:
	dir_table_free(&dt);
	return err;
}

static int http_rename_cross_dir(struct aios_sb_info *info, u64 old_parent, const char *old_name,
				 u64 new_parent, const char *new_name)
{
	int attempt;
	int err = -EAGAIN;

	for (attempt = 0; attempt < 8; attempt++) {
		struct aios_dir_table old_dir, new_dir;
		struct aios_inode_meta moved, victim, old_p, new_p;
		struct aios_held_lock locks[4];
		char txn_id[128];
		char oid[160];
		u64 ino, victim_ino = 0;
		u64 ts;
		u64 old_cas, new_cas, victim_cas = 0;
		u64 victim_size = 0, victim_unit = AIOS_HTTP_DEFAULT_STRIPE_UNIT;
		bool delete_victim = false;
		bool victim_exists = false;
		unsigned int nlocks = 4;

		memset(&victim, 0, sizeof(victim));
		err = dir_table_init(&old_dir, info->volume, old_parent);
		if (err)
			return err;
		err = dir_table_init(&new_dir, info->volume, new_parent);
		if (err) {
			dir_table_free(&old_dir);
			return err;
		}
		err = dir_load(info, &old_dir);
		if (err)
			goto next;
		err = dir_load(info, &new_dir);
		if (err)
			goto next;

		err = dir_find(&old_dir, old_name, &ino);
		if (err)
			goto next;
		if (ino == new_parent) {
			err = -EINVAL;
			goto next;
		}
		err = load_inode(info, ino, &moved);
		if (err)
			goto next;

		if (!dir_find(&new_dir, new_name, &victim_ino)) {
			if (victim_ino != ino) {
				err = load_inode(info, victim_ino, &victim);
				if (err && err != -ENOENT)
					goto next;
				victim_exists = !err && victim.exists;
				err = 0;
				if (victim_exists && S_ISDIR(victim.mode)) {
					err = -EISDIR;
					goto next;
				}
				if (S_ISDIR(moved.mode) && victim_exists && S_ISREG(victim.mode)) {
					err = -ENOTDIR;
					goto next;
				}
			}
		} else {
			victim_ino = 0;
		}

		err = load_inode(info, old_parent, &old_p);
		if (err)
			goto next;
		err = load_inode(info, new_parent, &new_p);
		if (err)
			goto next;
		if (!S_ISDIR(old_p.mode) || !S_ISDIR(new_p.mode)) {
			err = -ENOTDIR;
			goto next;
		}

		strscpy(locks[0].oid, old_dir.meta_oid, sizeof(locks[0].oid));
		strscpy(locks[1].oid, old_dir.log_oid, sizeof(locks[1].oid));
		strscpy(locks[2].oid, new_dir.meta_oid, sizeof(locks[2].oid));
		strscpy(locks[3].oid, new_dir.log_oid, sizeof(locks[3].oid));
		locks[0].token[0] = locks[1].token[0] = locks[2].token[0] =
			locks[3].token[0] = '\0';

		err = acquire_sorted_locks(info, locks, &nlocks);
		if (err == -EAGAIN) {
			dir_table_free(&old_dir);
			dir_table_free(&new_dir);
			msleep(20);
			continue;
		}
		if (err)
			goto next;

		/* Reload under locks. */
		err = dir_load(info, &old_dir);
		if (err)
			goto unlock;
		err = dir_load(info, &new_dir);
		if (err)
			goto unlock;
		{
			u64 cur_ino = 0;

			err = dir_find(&old_dir, old_name, &cur_ino);
			if (err)
				goto unlock;
			if (cur_ino != ino) {
				release_held_locks(info, locks, nlocks);
				dir_table_free(&old_dir);
				dir_table_free(&new_dir);
				continue;
			}
		}
		{
			u64 cur_victim = 0;
			int fe = dir_find(&new_dir, new_name, &cur_victim);

			if (!fe) {
				if (cur_victim != victim_ino && cur_victim != ino) {
					release_held_locks(info, locks, nlocks);
					dir_table_free(&old_dir);
					dir_table_free(&new_dir);
					continue;
				}
			} else if (victim_ino && victim_ino != ino) {
				release_held_locks(info, locks, nlocks);
				dir_table_free(&old_dir);
				dir_table_free(&new_dir);
				continue;
			}
		}

		err = load_inode(info, old_parent, &old_p);
		if (err)
			goto unlock;
		err = load_inode(info, new_parent, &new_p);
		if (err)
			goto unlock;
		err = load_inode(info, ino, &moved);
		if (err)
			goto unlock;
		if (victim_ino && victim_ino != ino) {
			err = load_inode(info, victim_ino, &victim);
			victim_exists = !err && victim.exists;
			if (err && err != -ENOENT)
				goto unlock;
			err = 0;
		}

		apply_dir_op(&old_dir, AIOS_HTTP_OP_UNLINK, old_name, NULL);
		if (victim_ino && victim_ino != ino)
			apply_dir_op(&new_dir, AIOS_HTTP_OP_UNLINK, new_name, NULL);
		{
			char inos[32];

			snprintf(inos, sizeof(inos), "%llu", (unsigned long long)ino);
			apply_dir_op(&new_dir, AIOS_HTTP_OP_LINK, new_name, inos);
		}

		ts = now_ns();
		old_p.mtime_ns = old_p.ctime_ns = ts;
		new_p.mtime_ns = new_p.ctime_ns = ts;
		if (S_ISDIR(moved.mode)) {
			if (old_p.nlink > 2)
				old_p.nlink -= 1;
			new_p.nlink += 1;
		}

		txn_id[0] = '\0';
		err = aios_http_txn_begin(info->http, txn_id, sizeof(txn_id));
		if (err)
			goto unlock;

		err = txn_put_dir(info, txn_id, &old_dir, locks, nlocks);
		if (err)
			goto abort;
		err = txn_put_dir(info, txn_id, &new_dir, locks, nlocks);
		if (err)
			goto abort;

		{
			char *full = NULL;
			size_t flen = 0;

			err = inode_to_json_full(info, &old_p, &full, &flen);
			if (err)
				goto abort;
			oid_ino(info->volume, old_parent, oid, sizeof(oid));
			old_cas = old_p.cas;
			err = aios_http_txn_prepare_put(info->http, txn_id, oid, full, flen, NULL,
							&old_cas);
			kfree(full);
			if (err)
				goto abort;

			err = inode_to_json_full(info, &new_p, &full, &flen);
			if (err)
				goto abort;
			oid_ino(info->volume, new_parent, oid, sizeof(oid));
			new_cas = new_p.cas;
			err = aios_http_txn_prepare_put(info->http, txn_id, oid, full, flen, NULL,
							&new_cas);
			kfree(full);
			if (err)
				goto abort;
		}

		delete_victim = false;
		if (victim_ino && victim_ino != ino && victim_exists) {
			oid_ino(info->volume, victim_ino, oid, sizeof(oid));
			if (victim.nlink > 1) {
				char *full = NULL;
				size_t flen = 0;

				victim.nlink -= 1;
				victim.ctime_ns = ts;
				err = inode_to_json_full(info, &victim, &full, &flen);
				if (err)
					goto abort;
				victim_cas = victim.cas;
				err = aios_http_txn_prepare_put(info->http, txn_id, oid, full, flen,
								NULL, &victim_cas);
				kfree(full);
				if (err)
					goto abort;
			} else {
				if (S_ISREG(victim.mode)) {
					victim_size = victim.size;
					victim_unit = victim.stripe_unit ? victim.stripe_unit :
									  AIOS_HTTP_DEFAULT_STRIPE_UNIT;
				}
				err = aios_http_txn_prepare_delete(info->http, txn_id, oid, NULL);
				if (err)
					goto abort;
				delete_victim = true;
			}
		}

		err = aios_http_txn_commit(info->http, txn_id);
		if (err)
			goto abort;
		txn_id[0] = '\0';

		release_held_locks(info, locks, nlocks);
		dir_table_free(&old_dir);
		dir_table_free(&new_dir);

		if (delete_victim && victim_ino && victim_size) {
			u64 chunks = (victim_size + victim_unit - 1) / victim_unit;
			u64 c;

			for (c = 0; c < chunks; c++) {
				oid_chunk(info->volume, victim_ino, c, oid, sizeof(oid));
				aios_http_delete(info->http, oid);
			}
		}
		return 0;

abort:
		if (txn_id[0])
			aios_http_txn_abort(info->http, txn_id);
unlock:
		release_held_locks(info, locks, nlocks);
next:
		dir_table_free(&old_dir);
		dir_table_free(&new_dir);
		if (err == -ENOENT || err == -EINVAL || err == -EISDIR || err == -ENOTDIR)
			return err;
		if (err == -EAGAIN) {
			msleep(20);
			continue;
		}
		if (err)
			return err;
	}
	return -EAGAIN;
}

static int http_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct aios_sb_info *info = AIOS_SB(dir->i_sb);
	struct aios_dir_table new_dt;
	struct aios_inode_meta m, np;
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
	err = dir_load(info, &new_dt);
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
	err = apply_dir_op(&new_dt, AIOS_HTTP_OP_LINK, new_name, inos);
	if (err)
		goto out_dt;
	err = dir_store_compact(info, &new_dt);
	if (err)
		goto out_dt;
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
out_dt:
	dir_table_free(&new_dt);
out:
	mutex_unlock(&info->http_mu);
	return err;
}

static int http_rename(struct user_namespace *mnt_userns, struct inode *old_dir,
		       struct dentry *old_dentry, struct inode *new_dir,
		       struct dentry *new_dentry, unsigned int flags)
{
	struct aios_sb_info *info = AIOS_SB(old_dir->i_sb);
	char old_name[AIOS_KABI_NAME_MAX + 1];
	char new_name[AIOS_KABI_NAME_MAX + 1];
	int err;

	if (flags)
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
		err = http_rename_same_dir(info, old_dir->i_ino, old_name, new_name);
	else
		err = http_rename_cross_dir(info, old_dir->i_ino, old_name, new_dir->i_ino,
					    new_name);
	mutex_unlock(&info->http_mu);
	return err;
}

static int http_getattr(struct user_namespace *mnt_userns, const struct path *path,
			struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	int err = http_refresh(inode);

	if (err)
		return err;
	generic_fillattr(inode, stat);
	return 0;
}

static int truncate_file(struct aios_sb_info *info, struct aios_inode_meta *m, u64 size)
{
	u64 unit = m->stripe_unit ? m->stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	char oid[160];
	int err;

	if (size < m->size) {
		u64 first_drop = (size + unit - 1) / unit;
		u64 old_chunks = (m->size + unit - 1) / unit;
		u64 c;

		for (c = first_drop; c < old_chunks; c++) {
			oid_chunk(info->volume, m->ino, c, oid, sizeof(oid));
			aios_http_delete(info->http, oid);
		}
		if (size > 0) {
			u64 last = (size - 1) / unit;
			u64 keep = size - last * unit;
			struct aios_http_buf body = { 0 };

			oid_chunk(info->volume, m->ino, last, oid, sizeof(oid));
			err = aios_http_get(info->http, oid, &body, NULL);
			if (!err && body.len > keep) {
				err = aios_http_put(info->http, oid, body.data, keep, NULL, NULL);
				aios_http_buf_free(&body);
				if (err)
					return err;
			} else {
				aios_http_buf_free(&body);
				if (err && err != -ENOENT)
					return err;
			}
		}
	}
	m->size = size;
	m->mtime_ns = m->ctime_ns = now_ns();
	return store_inode(info, m);
}

static int http_setattr(struct user_namespace *mnt_userns, struct dentry *dentry,
			struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m;
	int err;

	err = setattr_prepare(mnt_userns, dentry, attr);
	if (err)
		return err;

	mutex_lock(&info->http_mu);
	err = load_inode(info, inode->i_ino, &m);
	if (err)
		goto out;
	if (attr->ia_valid & ATTR_MODE)
		m.mode = (m.mode & S_IFMT) | (attr->ia_mode & 07777);
	if (attr->ia_valid & ATTR_UID)
		m.uid = from_kuid(mnt_userns, attr->ia_uid);
	if (attr->ia_valid & ATTR_GID)
		m.gid = from_kgid(mnt_userns, attr->ia_gid);
	if (attr->ia_valid & ATTR_SIZE) {
		err = truncate_file(info, &m, attr->ia_size);
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
	setattr_copy(mnt_userns, inode, attr);
	mark_inode_dirty(inode);
out:
	mutex_unlock(&info->http_mu);
	return err;
}

const struct inode_operations aios_http_dir_inode_ops = {
	.lookup = http_lookup,
	.create = http_create,
	.mkdir = http_mkdir,
	.unlink = http_unlink,
	.rmdir = http_rmdir,
	.rename = http_rename,
	.link = http_link,
	.getattr = http_getattr,
	.setattr = http_setattr,
	.listxattr = aios_listxattr,
};

const struct inode_operations aios_http_file_inode_ops = {
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
	err = dir_load(info, &dt);
	if (err)
		goto out_dt;

	/* pos 0,1 are . and ..; entries start at pos 2 */
	for (i = 0; i < dt.count; i++) {
		loff_t pos = (loff_t)i + 2;
		struct aios_inode_meta m;
		unsigned char type = DT_UNKNOWN;

		if (ctx->pos > pos)
			continue;
		ctx->pos = pos;
		if (!load_inode(info, dt.ents[i].ino, &m)) {
			if (S_ISDIR(m.mode))
				type = DT_DIR;
			else if (S_ISREG(m.mode))
				type = DT_REG;
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

const struct file_operations aios_http_dir_ops = {
	.owner = THIS_MODULE,
	.iterate_shared = http_readdir,
	.llseek = generic_file_llseek,
};

int aios_http_io_read(struct inode *inode, loff_t pos, void *buf, size_t len, size_t *out_len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m;
	struct aios_iinfo *ii = inode->i_private;
	u64 unit, p, end;
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
	if ((u64)pos >= m.size) {
		err = 0;
		goto out;
	}
	end = min_t(u64, (u64)pos + len, m.size);
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

/* Chunk RMW using an explicit client (no http_mu; caller serializes per-chunk). */
static int http_chunk_write(struct aios_http_client *c, const char *volume, u64 ino, u64 unit,
			    u64 pos, const void *buf, size_t len)
{
	u64 p = pos;
	size_t done = 0;
	int err = 0;

	while (done < len) {
		u64 chunk = p / unit;
		u64 chunk_off = p % unit;
		size_t n = min_t(size_t, (size_t)(unit - chunk_off), len - done);
		char oid[160];
		struct aios_http_buf body = { 0 };
		char *nb;
		size_t nlen;

		oid_chunk(volume, ino, chunk, oid, sizeof(oid));
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
		memcpy(nb + chunk_off, (char *)buf + done, n);
		err = aios_http_put(c, oid, nb, nlen, NULL, NULL);
		kvfree(nb);
		if (err)
			return err;
		p += n;
		done += n;
	}
	return 0;
}

int aios_http_io_write(struct inode *inode, loff_t pos, const void *buf, size_t len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m;
	u64 unit;
	int err;

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
	unit = m.stripe_unit ? m.stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	err = http_chunk_write(info->http, info->volume, m.ino, unit, (u64)pos, buf, len);
	if (err)
		goto out;
	m.size = max_t(u64, m.size, (u64)pos + len);
	m.mtime_ns = m.ctime_ns = now_ns();
	err = store_inode(info, &m);
	if (!err)
		attach_iinfo(inode, &m);
out:
	mutex_unlock(&info->http_mu);
	return err;
}

#define AIOS_HTTP_WB_MAX 64

struct aios_http_wb_page {
	struct page *page;
	loff_t pos;
	size_t len;
	void *data;
};

struct aios_http_wb_chunk {
	struct work_struct work;
	struct aios_sb_info *info;
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

static void aios_http_wb_chunk_work(struct work_struct *work)
{
	struct aios_http_wb_chunk *cw = container_of(work, struct aios_http_wb_chunk, work);
	struct aios_http_client *c;
	unsigned int i;
	unsigned int noio;
	int err = 0;

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
	for (i = 0; i < cw->npages && !err; i++) {
		err = http_chunk_write(c, cw->info->volume, cw->ino, cw->unit, (u64)cw->pages[i].pos,
				       cw->pages[i].data, cw->pages[i].len);
	}
	aios_http_pool_put(cw->info->http_pool, c);
	memalloc_noio_restore(noio);
	cw->err = err;
	complete(&cw->done);
}

static int aios_http_wb_collect(struct page *page, struct writeback_control *wbc, void *data)
{
	struct aios_http_wb_page **pp = data;
	struct aios_http_wb_page *batch = *pp;
	struct inode *inode = page->mapping->host;
	loff_t pos = page_offset(page);
	loff_t i_size = i_size_read(inode);
	void *kaddr;
	unsigned int n = 0;

	while (n < AIOS_HTTP_WB_MAX && batch[n].page)
		n++;
	if (n >= AIOS_HTTP_WB_MAX) {
		/* Batch full — skip; write_cache_pages will revisit dirty pages. */
		redirty_page_for_writepage(wbc, page);
		unlock_page(page);
		return 0;
	}

	set_page_writeback(page);
	batch[n].page = page;
	batch[n].pos = pos;
	batch[n].len = 0;
	batch[n].data = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!batch[n].data) {
		end_page_writeback(page);
		unlock_page(page);
		return -ENOMEM;
	}
	if (pos < i_size) {
		batch[n].len = min_t(loff_t, PAGE_SIZE, i_size - pos);
		kaddr = kmap(page);
		memcpy(batch[n].data, kaddr, batch[n].len);
		kunmap(page);
	}
	unlock_page(page);
	return 0;
}

static int aios_http_writepages_noio(struct address_space *mapping,
				     struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_http_wb_page *batch;
	struct aios_inode_meta m;
	struct aios_http_wb_chunk *chunks = NULL;
	unsigned int n = 0, nchunks = 0, i, c;
	u64 unit, max_end = 0;
	int err;

	if (!info->http_pool || !info->wb_wq)
		return -EINVAL;

	batch = kcalloc(AIOS_HTTP_WB_MAX, sizeof(*batch), GFP_KERNEL);
	if (!batch)
		return -ENOMEM;

	err = write_cache_pages(mapping, wbc, aios_http_wb_collect, &batch);
	if (err)
		goto out_pages;

	for (i = 0; i < AIOS_HTTP_WB_MAX; i++) {
		if (!batch[i].page)
			break;
		n++;
		if (batch[i].len)
			max_end = max_t(u64, max_end, (u64)batch[i].pos + batch[i].len);
	}
	if (!n) {
		kfree(batch);
		return 0;
	}

	sort(batch, n, sizeof(*batch), aios_http_wb_cmp, NULL);

	mutex_lock(&info->http_mu);
	err = load_inode(info, inode->i_ino, &m);
	if (err) {
		mutex_unlock(&info->http_mu);
		goto out_pages;
	}
	unit = m.stripe_unit ? m.stripe_unit : AIOS_HTTP_DEFAULT_STRIPE_UNIT;
	mutex_unlock(&info->http_mu);

	/* Count distinct chunks. */
	nchunks = 1;
	for (i = 1; i < n; i++) {
		if (batch[i].pos / unit != batch[i - 1].pos / unit)
			nchunks++;
	}
	chunks = kcalloc(nchunks, sizeof(*chunks), GFP_KERNEL);
	if (!chunks) {
		err = -ENOMEM;
		goto out_pages;
	}

	c = 0;
	chunks[0].info = info;
	chunks[0].ino = inode->i_ino;
	chunks[0].unit = unit;
	chunks[0].chunk = batch[0].pos / unit;
	chunks[0].pages = &batch[0];
	chunks[0].npages = 1;
	init_completion(&chunks[0].done);
	INIT_WORK(&chunks[0].work, aios_http_wb_chunk_work);
	for (i = 1; i < n; i++) {
		u64 ch = batch[i].pos / unit;

		if (ch == chunks[c].chunk) {
			chunks[c].npages++;
		} else {
			c++;
			chunks[c].info = info;
			chunks[c].ino = inode->i_ino;
			chunks[c].unit = unit;
			chunks[c].chunk = ch;
			chunks[c].pages = &batch[i];
			chunks[c].npages = 1;
			init_completion(&chunks[c].done);
			INIT_WORK(&chunks[c].work, aios_http_wb_chunk_work);
		}
	}

	for (i = 0; i < nchunks; i++)
		queue_work(info->wb_wq, &chunks[i].work);
	err = 0;
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
			err = store_inode(info, &m);
			if (!err)
				attach_iinfo(inode, &m);
		}
		mutex_unlock(&info->http_mu);
	}

	for (i = 0; i < n; i++) {
		if (err) {
			SetPageError(batch[i].page);
			mapping_set_error(mapping, err);
			set_page_dirty(batch[i].page);
		} else {
			ClearPageError(batch[i].page);
		}
		end_page_writeback(batch[i].page);
	}

	kfree(chunks);
	for (i = 0; i < n; i++)
		kfree(batch[i].data);
	kfree(batch);
	return err;

out_pages:
	for (i = 0; i < AIOS_HTTP_WB_MAX; i++) {
		if (!batch[i].page)
			break;
		SetPageError(batch[i].page);
		mapping_set_error(mapping, err ? err : -EIO);
		end_page_writeback(batch[i].page);
		kfree(batch[i].data);
	}
	kfree(batch);
	kfree(chunks);
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

int aios_http_io_set_size(struct inode *inode, loff_t size)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m;
	int err;

	mutex_lock(&info->http_mu);
	err = load_inode(info, inode->i_ino, &m);
	if (err)
		goto out;
	if (!S_ISREG(m.mode)) {
		err = -EISDIR;
		goto out;
	}
	if ((u64)size == m.size) {
		err = 0;
		goto out;
	}
	err = truncate_file(info, &m, (u64)size);
	if (!err)
		attach_iinfo(inode, &m);
out:
	mutex_unlock(&info->http_mu);
	return err;
}

/* Best-effort punch hole with KEEP_SIZE: zero overlapping chunk ranges. */
int aios_http_io_punch(struct inode *inode, loff_t offset, loff_t len)
{
	struct aios_sb_info *info = AIOS_SB(inode->i_sb);
	struct aios_inode_meta m;
	u64 unit, start, end, p;
	int err;

	if (offset < 0 || len <= 0)
		return -EINVAL;

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
		char oid[160];
		struct aios_http_buf body = { 0 };
		char *nb;
		size_t nlen;

		oid_chunk(info->volume, m.ino, chunk, oid, sizeof(oid));
		if (chunk_off == 0 && n == unit) {
			/* Entire chunk punched — delete object. */
			err = aios_http_delete(info->http, oid);
			if (err && err != -ENOENT)
				goto out;
			err = 0;
			p += n;
			continue;
		}
		err = aios_http_get(info->http, oid, &body, NULL);
		if (err == -ENOENT) {
			err = 0;
			p += n;
			continue;
		}
		if (err)
			goto out;
		nlen = body.len;
		if (chunk_off + n > nlen)
			nlen = chunk_off + n;
		nb = kvmalloc(nlen, GFP_KERNEL);
		if (!nb) {
			aios_http_buf_free(&body);
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
		if (err)
			goto out;
		p += n;
	}
	m.ctime_ns = now_ns();
	err = store_inode(info, &m);
	if (!err)
		attach_iinfo(inode, &m);
out:
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
	struct aios_inode_meta m;
	char base[512];
	char *xobj = NULL;
	char *full = NULL;
	size_t flen = 0;
	char oid[160];
	int err;

	err = inode_from_json(raw_js, cas, &m);
	if (err)
		return err;
	m.ctime_ns = now_ns();
	err = inode_to_json(&m, base, sizeof(base));
	if (err)
		return err;
	if (n) {
		err = build_xattrs_object(ents, n, &xobj);
		if (err)
			return err;
	}
	err = inode_json_merge_xattrs(base, xobj, &full, &flen);
	kfree(xobj);
	if (err)
		return err;
	oid_ino(info->volume, ino, oid, sizeof(oid));
	err = aios_http_put(info->http, oid, full, flen, NULL, &cas);
	kfree(full);
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
	buf->f_type = AIOSFS_MAGIC;
	buf->f_bsize = 4096;
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
	kfree(info);
	sb->s_fs_info = NULL;
}

static const struct super_operations aios_http_super_ops = {
	.statfs = http_statfs,
	.drop_inode = generic_delete_inode,
	.evict_inode = aios_evict_inode,
	.write_inode = aios_write_inode,
	.put_super = http_put_super,
	.show_options = aios_show_options,
};

int aios_fill_super_http(struct super_block *sb, struct aios_sb_info *info)
{
	struct aios_http_pool *pool;
	struct aios_http_client *c;
	struct aios_inode_meta root;
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

	pool = aios_http_pool_create(info->endpoint, info->cluster_key,
				     info->app_label[0] ? info->app_label : NULL,
				     AIOSFS_HTTP_POOL_SIZE, GFP_KERNEL);
	if (IS_ERR(pool))
		return PTR_ERR(pool);
	aios_http_pool_set_timeout_ms(pool, 30000);
	info->http_pool = pool;
	/* max_active tracks the client pool: extra workers would only pile up
	 * blocked in aios_http_pool_get, one kernel stack each. */
	info->wb_wq = alloc_workqueue("aiosfs-wb", WQ_MEM_RECLAIM | WQ_UNBOUND,
				      AIOSFS_HTTP_POOL_SIZE);
	if (!info->wb_wq) {
		aios_http_pool_destroy(pool);
		info->http_pool = NULL;
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
	pr_info("aiosfs: mounted with backend=http (pool=%u)\n", AIOSFS_HTTP_POOL_SIZE);
	return 0;

fail:
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
	return err;
}
