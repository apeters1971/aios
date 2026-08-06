/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AIOS_HTTP_INTERNAL_H
#define AIOS_HTTP_INTERNAL_H

#include "aios_http_api.h"

#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define AIOS_HTTP_MAX_BODY (16u * 1024u * 1024u)
#define AIOS_HTTP_MAX_HDR (16u * 1024u)
#define AIOS_HTTP_MAX_REDIRECTS 5
#define AIOS_HTTP_DEFAULT_TIMEOUT_MS 30000u

struct aios_http_client {
	char host[256];
	char port[16];
	char endpoint[272]; /* host:port for Host header */
	char cluster_key[256];
	char app_label[64];
	gfp_t gfp;
	unsigned int timeout_ms;
	struct socket *sock; /* keep-alive TCP; NULL if disconnected */
	atomic64_t timeouts;
	atomic64_t reconnects;
	struct mutex mu; /* serialize TCP transactions on this client */
	/* Request/response header scratch, both AIOS_HTTP_MAX_HDR, owned by mu.
	 * A header buffer is the size of a whole kernel stack, so it can be neither
	 * automatic nor per-request: keeping them here also spares the I/O path two
	 * trips through the allocator per request. Exactly AIOS_HTTP_MAX_HDR so each
	 * lands on a kmalloc bucket boundary rather than rounding up to the next. */
	char *reqbuf;
	char *hdrbuf;
};

struct aios_http_pool {
	struct aios_http_client **clients;
	unsigned int n;
	unsigned long busy; /* bitmask; n <= BITS_PER_LONG */
	spinlock_t lock;
	struct semaphore sem;
};

int aios_http_hmac_sha256_hex(const char *key, size_t key_len, const char *data,
			      size_t data_len, char *hex_out /* 65 bytes */);

/* Build Authorization + date headers into auth_hdrs (caller buffer). */
int aios_http_build_auth(const struct aios_http_client *c, const char *method,
			 const char *path, char *auth_hdrs, size_t auth_hdrs_len);

int aios_http_tcp_request(struct aios_http_client *c, const char *method, const char *path,
			  const char *extra_hdrs, const void *body, size_t body_len,
			  int *status_out, char *location_out, size_t location_len,
			  struct aios_http_buf *resp_body, char *resp_hdrs,
			  size_t resp_hdrs_len);

int aios_http_header_get(const char *hdrs, const char *name, char *out, size_t out_len);
u64 aios_http_attr_u64(const char *hdrs, const char *attr_name);

/* Returns 1 if path-only, 0 if host/port/path filled, -errno on error. */
int aios_http_parse_location(const char *loc, char *host, size_t host_len, char *port,
			     size_t port_len, char *path, size_t path_len);

int aios_http_encode_oid(const char *oid, char *out, size_t out_len);
int aios_http_map_status(int status);

/* Build aios.posix.cas precondition headers. new_cas_out receives expected+1. */
int aios_http_fill_posix_cas(struct aios_http_client *c, const char *oid, u64 expected_cas,
			     char *out, size_t out_len, u64 *new_cas_out);

int aios_http_json_string(const char *js, size_t js_len, const char *key, char *out,
			  size_t out_len);

void aios_http_client_close_sock(struct aios_http_client *c);

#endif /* AIOS_HTTP_INTERNAL_H */
