// SPDX-License-Identifier: GPL-2.0
#include "internal.h"

#include <linux/ctype.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

void aios_http_buf_free(struct aios_http_buf *b)
{
	if (!b)
		return;
	kvfree(b->data);
	b->data = NULL;
	b->len = 0;
}
EXPORT_SYMBOL_GPL(aios_http_buf_free);

struct aios_http_client *aios_http_client_create(const char *endpoint, const char *cluster_key,
						 gfp_t gfp)
{
	struct aios_http_client *c;
	const char *colon;

	if (!endpoint || !cluster_key || !*endpoint || !*cluster_key)
		return ERR_PTR(-EINVAL);

	c = kzalloc(sizeof(*c), gfp);
	if (!c)
		return ERR_PTR(-ENOMEM);

	colon = strrchr(endpoint, ':');
	if (!colon || colon == endpoint || !*(colon + 1)) {
		kfree(c);
		return ERR_PTR(-EINVAL);
	}
	if ((size_t)(colon - endpoint) >= sizeof(c->host)) {
		kfree(c);
		return ERR_PTR(-EINVAL);
	}
	memcpy(c->host, endpoint, colon - endpoint);
	c->host[colon - endpoint] = '\0';
	strscpy(c->port, colon + 1, sizeof(c->port));
	strscpy(c->endpoint, endpoint, sizeof(c->endpoint));
	strscpy(c->cluster_key, cluster_key, sizeof(c->cluster_key));
	c->gfp = gfp;
	mutex_init(&c->mu);
	return c;
}
EXPORT_SYMBOL_GPL(aios_http_client_create);

void aios_http_client_destroy(struct aios_http_client *c)
{
	if (!c)
		return;
	memzero_explicit(c->cluster_key, sizeof(c->cluster_key));
	kfree(c);
}
EXPORT_SYMBOL_GPL(aios_http_client_destroy);

void aios_http_client_set_app_label(struct aios_http_client *c, const char *label)
{
	if (!c)
		return;
	if (!label)
		c->app_label[0] = '\0';
	else
		strscpy(c->app_label, label, sizeof(c->app_label));
}
EXPORT_SYMBOL_GPL(aios_http_client_set_app_label);

int aios_http_oid_path(const char *oid, char *oid_path_out, size_t out_len)
{
	static const char *hex = "0123456789ABCDEF";
	char enc[1024];
	size_t ei = 0;
	size_t i;

	if (!oid || !oid_path_out)
		return -EINVAL;
	for (i = 0; oid[i]; i++) {
		unsigned char ch = (unsigned char)oid[i];

		if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
			if (ei + 1 >= sizeof(enc))
				return -ENAMETOOLONG;
			enc[ei++] = ch;
		} else {
			if (ei + 3 >= sizeof(enc))
				return -ENAMETOOLONG;
			enc[ei++] = '%';
			enc[ei++] = hex[ch >> 4];
			enc[ei++] = hex[ch & 0xf];
		}
	}
	enc[ei] = '\0';
	if (snprintf(oid_path_out, out_len, "/o/%s", enc) >= (int)out_len)
		return -ENAMETOOLONG;
	return 0;
}
EXPORT_SYMBOL_GPL(aios_http_oid_path);

static int map_http_status(int status)
{
	if (status == 200 || status == 201 || status == 204 || status == 206)
		return 0;
	if (status == 404)
		return -ENOENT;
	if (status == 412 || status == 409)
		return -EAGAIN;
	if (status == 413)
		return -EFBIG;
	if (status == 400)
		return -EINVAL;
	return -EIO;
}

static int apply_redirect(struct aios_http_client *c, const char *location, char *cur_path,
			  size_t cur_path_len)
{
	char host[256], port[16], path[1024];
	int pr;

	pr = aios_http_parse_location(location, host, sizeof(host), port, sizeof(port), path,
				      sizeof(path));
	if (pr < 0)
		return pr;
	if (pr == 1) {
		strscpy(cur_path, path, cur_path_len);
		return 0;
	}
	strscpy(c->host, host, sizeof(c->host));
	strscpy(c->port, port, sizeof(c->port));
	snprintf(c->endpoint, sizeof(c->endpoint), "%s:%s", c->host, c->port);
	strscpy(cur_path, path, cur_path_len);
	return 0;
}

int aios_http_request(struct aios_http_client *c, const char *method, const char *path,
		      const char *extra_hdrs, const void *body, size_t body_len,
		      int *status_out, struct aios_http_buf *resp_body)
{
	char cur_path[1024];
	char location[1024];
	char hdrs[AIOS_HTTP_MAX_HDR];
	int hop;
	int err = 0;

	if (!c || !method || !path || !status_out)
		return -EINVAL;

	strscpy(cur_path, path, sizeof(cur_path));
	mutex_lock(&c->mu);
	for (hop = 0; hop <= AIOS_HTTP_MAX_REDIRECTS; hop++) {
		int status = 0;

		if (resp_body)
			aios_http_buf_free(resp_body);
		err = aios_http_tcp_request(c, method, cur_path, extra_hdrs, body, body_len,
					    &status, location, sizeof(location), resp_body,
					    hdrs, sizeof(hdrs));
		if (err)
			break;
		*status_out = status;
		if (status == 307 || status == 301 || status == 302) {
			if (!location[0]) {
				err = -EIO;
				break;
			}
			err = apply_redirect(c, location, cur_path, sizeof(cur_path));
			if (err)
				break;
			continue;
		}
		err = 0;
		break;
	}
	mutex_unlock(&c->mu);
	return err;
}
EXPORT_SYMBOL_GPL(aios_http_request);

/* Internal request that also returns response headers (for CAS attrs). */
static int request_with_hdrs(struct aios_http_client *c, const char *method, const char *path,
			     const char *extra_hdrs, const void *body, size_t body_len,
			     int *status_out, struct aios_http_buf *resp_body, char *hdrs,
			     size_t hdrs_len)
{
	char cur_path[1024];
	char location[1024];
	int hop;
	int err = 0;

	strscpy(cur_path, path, sizeof(cur_path));
	mutex_lock(&c->mu);
	for (hop = 0; hop <= AIOS_HTTP_MAX_REDIRECTS; hop++) {
		int status = 0;

		if (resp_body)
			aios_http_buf_free(resp_body);
		err = aios_http_tcp_request(c, method, cur_path, extra_hdrs, body, body_len,
					    &status, location, sizeof(location), resp_body, hdrs,
					    hdrs_len);
		if (err)
			break;
		*status_out = status;
		if (status == 307 || status == 301 || status == 302) {
			if (!location[0]) {
				err = -EIO;
				break;
			}
			err = apply_redirect(c, location, cur_path, sizeof(cur_path));
			if (err)
				break;
			continue;
		}
		err = 0;
		break;
	}
	mutex_unlock(&c->mu);
	return err;
}

int aios_http_get(struct aios_http_client *c, const char *oid, struct aios_http_buf *body,
		  u64 *cas_out)
{
	char path[1100];
	char hdrs[AIOS_HTTP_MAX_HDR];
	int status = 0;
	int err;

	if (!body)
		return -EINVAL;
	err = aios_http_oid_path(oid, path, sizeof(path));
	if (err)
		return err;
	err = request_with_hdrs(c, "GET", path, NULL, NULL, 0, &status, body, hdrs, sizeof(hdrs));
	if (err)
		return err;
	if (status == 404) {
		aios_http_buf_free(body);
		return -ENOENT;
	}
	err = map_http_status(status);
	if (err)
		return err;
	if (cas_out)
		*cas_out = aios_http_attr_u64(hdrs, "aios.posix.cas");
	return 0;
}
EXPORT_SYMBOL_GPL(aios_http_get);

int aios_http_head(struct aios_http_client *c, const char *oid, u64 *size_out, u64 *cas_out)
{
	char path[1100];
	char hdrs[AIOS_HTTP_MAX_HDR];
	char sz[32];
	int status = 0;
	int err;

	err = aios_http_oid_path(oid, path, sizeof(path));
	if (err)
		return err;
	err = request_with_hdrs(c, "HEAD", path, NULL, NULL, 0, &status, NULL, hdrs,
				sizeof(hdrs));
	if (err)
		return err;
	if (status == 404)
		return -ENOENT;
	err = map_http_status(status);
	if (err)
		return err;
	if (size_out) {
		*size_out = 0;
		if (!aios_http_header_get(hdrs, "x-aios-size", sz, sizeof(sz)))
			kstrtou64(sz, 10, size_out);
		else if (!aios_http_header_get(hdrs, "Content-Length", sz, sizeof(sz)))
			kstrtou64(sz, 10, size_out);
	}
	if (cas_out)
		*cas_out = aios_http_attr_u64(hdrs, "aios.posix.cas");
	return 0;
}
EXPORT_SYMBOL_GPL(aios_http_head);

int aios_http_get_range(struct aios_http_client *c, const char *oid, u64 start, u64 end,
			struct aios_http_buf *body)
{
	char path[1100];
	char extra[96];
	int status = 0;
	int err;

	if (!body || end < start)
		return -EINVAL;
	err = aios_http_oid_path(oid, path, sizeof(path));
	if (err)
		return err;
	snprintf(extra, sizeof(extra), "Range: bytes=%llu-%llu\r\n",
		 (unsigned long long)start, (unsigned long long)end);
	err = aios_http_request(c, "GET", path, extra, NULL, 0, &status, body);
	if (err)
		return err;
	if (status == 404) {
		aios_http_buf_free(body);
		return -ENOENT;
	}
	if (status == 416)
		return -ERANGE;
	return map_http_status(status);
}
EXPORT_SYMBOL_GPL(aios_http_get_range);

int aios_http_put(struct aios_http_client *c, const char *oid, const void *body, size_t len,
		  const char *extra_hdrs, u64 *cas_inout)
{
	char path[1100];
	char cas_hdrs[512];
	char combined[768];
	char hdrs[AIOS_HTTP_MAX_HDR];
	int status = 0;
	int err;
	u64 new_cas = 0;

	err = aios_http_oid_path(oid, path, sizeof(path));
	if (err)
		return err;

	cas_hdrs[0] = '\0';
	if (cas_inout) {
		new_cas = *cas_inout + 1;
		if (*cas_inout == 0) {
			u64 existing_cas = 0;
			int he = aios_http_head(c, oid, NULL, &existing_cas);

			if (he == -ENOENT) {
				snprintf(cas_hdrs, sizeof(cas_hdrs),
					 "If-None-Match: *\r\n"
					 "x-aios-attr-aios.posix.cas: %llu\r\n",
					 (unsigned long long)new_cas);
			} else if (he == 0 && existing_cas == 0) {
				snprintf(cas_hdrs, sizeof(cas_hdrs),
					 "If-Match: *\r\n"
					 "x-aios-if-attr-absent: aios.posix.cas\r\n"
					 "x-aios-attr-aios.posix.cas: %llu\r\n",
					 (unsigned long long)new_cas);
			} else if (he == 0) {
				return -EAGAIN;
			} else {
				return he;
			}
		} else {
			snprintf(cas_hdrs, sizeof(cas_hdrs),
				 "If-Match: *\r\n"
				 "x-aios-if-attr-eq: aios.posix.cas=%llu\r\n"
				 "x-aios-attr-aios.posix.cas: %llu\r\n",
				 (unsigned long long)*cas_inout, (unsigned long long)new_cas);
		}
	}

	if (extra_hdrs && extra_hdrs[0])
		snprintf(combined, sizeof(combined), "%s%s", cas_hdrs, extra_hdrs);
	else
		strscpy(combined, cas_hdrs, sizeof(combined));

	{
		char ctype[64] = "Content-Type: application/octet-stream\r\n";
		char all[832];

		snprintf(all, sizeof(all), "%s%s", ctype, combined);
		err = request_with_hdrs(c, "PUT", path, all, body, len, &status, NULL, hdrs,
					sizeof(hdrs));
	}
	if (err)
		return err;
	err = map_http_status(status);
	if (err)
		return err;
	if (cas_inout)
		*cas_inout = new_cas;
	return 0;
}
EXPORT_SYMBOL_GPL(aios_http_put);

int aios_http_delete(struct aios_http_client *c, const char *oid)
{
	char path[1100];
	int status = 0;
	int err;

	err = aios_http_oid_path(oid, path, sizeof(path));
	if (err)
		return err;
	err = aios_http_request(c, "DELETE", path, NULL, NULL, 0, &status, NULL);
	if (err)
		return err;
	if (status == 404)
		return -ENOENT;
	return map_http_status(status);
}
EXPORT_SYMBOL_GPL(aios_http_delete);
