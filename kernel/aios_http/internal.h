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
#define AIOS_HTTP_PATH_MAX 1100
#define AIOS_HTTP_LOC_MAX 1024
#define AIOS_HTTP_CANON_MAX 2048
#define AIOS_HTTP_TXN_PATH_MAX 1408
#define AIOS_HTTP_MAX_REDIRECTS 5
#define AIOS_HTTP_DEFAULT_TIMEOUT_MS 30000u
/* Sealed ticket blob ("t1." + base64) as issued by POST /auth/ticket. */
#define AIOS_HTTP_TICKET_MAX 1024
/* Auth header lines: date + content sha + Authorization carrying the ticket. */
#define AIOS_HTTP_AUTH_MAX (AIOS_HTTP_TICKET_MAX + 256)

struct aios_http_client {
	char host[256];
	char port[16];
	char endpoint[272]; /* host:port for Host header */
	/*
	 * Shared cluster key, or — when principal[0] is set — the principal's
	 * 64-hex key. The latter never goes on the wire: it proves possession
	 * once (aios_http_ensure_ticket) and derives the session key.
	 */
	char cluster_key[256];
	char principal[AIOS_HTTP_PRINCIPAL_MAX];
	char app_label[64];
	gfp_t gfp;
	unsigned int timeout_ms;
	/* Ticket state, owned by mu. session_key is 64 hex chars. */
	char ticket[AIOS_HTTP_TICKET_MAX];
	char session_key[65];
	s64 ticket_issued_ms;
	s64 ticket_expires_ms;
	bool in_ticket_exchange; /* build_auth emits no credential */
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
	char *authbuf; /* AIOS_HTTP_AUTH_MAX, owned by mu */
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

/* Build Authorization + date headers into auth_hdrs (caller buffer). Caller
 * holds c->mu; in principal mode this may first fetch or renew the ticket. */
int aios_http_build_auth(struct aios_http_client *c, const char *method,
			 const char *path, char *auth_hdrs, size_t auth_hdrs_len);

/* Principal mode: obtain a ticket now if none is held or the current one is
 * past its half-life. Caller holds c->mu. */
int aios_http_ensure_ticket(struct aios_http_client *c, bool force);

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
/* Top-level integer field ("key":123). -ENOENT when absent. */
int aios_http_json_s64(const char *js, size_t js_len, const char *key, s64 *out);

void aios_http_client_close_sock(struct aios_http_client *c);

#endif /* AIOS_HTTP_INTERNAL_H */
