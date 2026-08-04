/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Exported API from aios_http.ko — in-kernel AIOS HTTP/1.1 + HMAC client
 * for AlmaLinux 9 / RHEL 9 (kernel 5.14).
 */
#ifndef AIOS_HTTP_API_H
#define AIOS_HTTP_API_H

#include <linux/types.h>

struct aios_http_client;

struct aios_http_buf {
	void *data; /* kvmalloc'd; caller uses aios_http_buf_free() */
	size_t len;
};

struct aios_http_client *aios_http_client_create(const char *endpoint /* HOST:PORT */,
						 const char *cluster_key, gfp_t gfp);
void aios_http_client_destroy(struct aios_http_client *c);
void aios_http_client_set_app_label(struct aios_http_client *c, const char *label);

void aios_http_buf_free(struct aios_http_buf *b);

/* Low-level request. path is e.g. "/o/foo". Follows up to 5× 307 redirects.
 * extra_hdrs: optional "Header: value\r\nHeader2: value\r\n" (may be NULL).
 * Returns 0 or -errno; *status_out is HTTP status on success. */
int aios_http_request(struct aios_http_client *c, const char *method, const char *path,
		      const char *extra_hdrs, const void *body, size_t body_len,
		      int *status_out, struct aios_http_buf *resp_body);

/* OID helpers (percent-encode '/'). oid_path_out receives "/o/{urlencoded}". */
int aios_http_oid_path(const char *oid, char *oid_path_out, size_t out_len);

int aios_http_get(struct aios_http_client *c, const char *oid, struct aios_http_buf *body,
		  u64 *cas_out /* optional */);
int aios_http_head(struct aios_http_client *c, const char *oid, u64 *size_out, u64 *cas_out);
int aios_http_put(struct aios_http_client *c, const char *oid, const void *body, size_t len,
		  const char *extra_hdrs, u64 *cas_inout /* NULL=unconditional; else CAS */);
int aios_http_delete(struct aios_http_client *c, const char *oid);

/* Range GET: bytes start-end inclusive. */
int aios_http_get_range(struct aios_http_client *c, const char *oid, u64 start, u64 end,
			struct aios_http_buf *body);

#endif /* AIOS_HTTP_API_H */
