/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Exported API from aios_http.ko — in-kernel AIOS HTTP/1.1 + HMAC client
 * for AlmaLinux 9 / RHEL 9 (kernel 5.14).
 */
#ifndef AIOS_HTTP_API_H
#define AIOS_HTTP_API_H

#include <linux/types.h>

struct aios_http_client;
struct aios_http_pool;

struct aios_http_buf {
	void *data; /* kvmalloc'd; caller uses aios_http_buf_free() */
	size_t len;
};

/* Principal names: [A-Za-z0-9._-], at most 64 chars (+ NUL). */
#define AIOS_HTTP_PRINCIPAL_MAX 65

struct aios_http_client *aios_http_client_create(const char *endpoint /* HOST:PORT */,
						 const char *cluster_key, gfp_t gfp);
void aios_http_client_destroy(struct aios_http_client *c);
void aios_http_client_set_app_label(struct aios_http_client *c, const char *label);

/*
 * Ticket authentication (cephx / krb5 style; src/util/ticket.hpp). After this
 * call the key passed to aios_http_client_create is treated as the principal's
 * 64-hex key: the client proves possession to POST /auth/ticket, receives a
 * sealed ticket plus a per-session key, and signs every request with that
 * session key. The principal key is never sent. The ticket is renewed at its
 * half-life. Returns -EINVAL for a bad name or key, otherwise 0; the first
 * ticket is fetched lazily by the first request (or aios_http_client_login).
 */
int aios_http_client_set_principal(struct aios_http_client *c, const char *principal);
/* Fetch a ticket now (principal mode), so mount can fail fast on bad credentials. */
int aios_http_client_login(struct aios_http_client *c);

/* Socket send/recv timeout (default 30000). Applied to sk_sndtimeo/sk_rcvtimeo. */
void aios_http_client_set_timeout_ms(struct aios_http_client *c, unsigned int ms);
void aios_http_client_get_stats(struct aios_http_client *c, u64 *timeouts, u64 *reconnects);

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
/* Partial PUT via Content-Range (bytes start-end inclusive, '*' for total).
 * The server applies the patch (zero-filling any gap) and creates the object
 * when missing. -EOPNOTSUPP when the tip is erasure-coded or compressed and
 * the caller must rewrite the whole object instead. */
int aios_http_put_range(struct aios_http_client *c, const char *oid, u64 offset,
			const void *data, size_t len, u64 *cas_inout);
/* POST /o/{oid}/append: atomic append at tip size. size_out receives the new
 * object size. lock_token may be NULL. */
int aios_http_append(struct aios_http_client *c, const char *oid, const void *data, size_t len,
		     const char *lock_token, u64 *size_out);
int aios_http_delete(struct aios_http_client *c, const char *oid);

/* Range GET: bytes start-end inclusive. */
int aios_http_get_range(struct aios_http_client *c, const char *oid, u64 start, u64 end,
			struct aios_http_buf *body);

/* Object locks (POST/DELETE /o/{oid}/lock). token_out must be >= 128 bytes. */
int aios_http_lock_acquire(struct aios_http_client *c, const char *oid, int ttl_ms,
			   char *token_out, size_t token_len);
int aios_http_lock_release(struct aios_http_client *c, const char *oid, const char *token);

/* Cross-object transactions (/txn). cas_inout NULL = unconditional prepare. */
int aios_http_txn_begin(struct aios_http_client *c, char *txn_id_out, size_t txn_id_len);
int aios_http_txn_prepare_put(struct aios_http_client *c, const char *txn_id, const char *oid,
			      const void *body, size_t len, const char *lock_token,
			      u64 *cas_inout);
int aios_http_txn_prepare_delete(struct aios_http_client *c, const char *txn_id, const char *oid,
				 const char *lock_token);
int aios_http_txn_commit(struct aios_http_client *c, const char *txn_id);
int aios_http_txn_abort(struct aios_http_client *c, const char *txn_id);

/*
 * Shared client pool — create/get/put/destroy. aiosvd uses this; aiosfs may adopt later.
 * Clients are keep-alive TCP sockets with per-client mutex serialization.
 */
struct aios_http_pool *aios_http_pool_create(const char *endpoint, const char *cluster_key,
					     const char *app_label, unsigned int n, gfp_t gfp);
void aios_http_pool_destroy(struct aios_http_pool *p);
/* Principal mode for every client in the pool (see aios_http_client_set_principal).
 * Logs the first client in so a bad key fails here rather than on first I/O. */
int aios_http_pool_set_principal(struct aios_http_pool *p, const char *principal);
struct aios_http_client *aios_http_pool_get(struct aios_http_pool *p);
void aios_http_pool_put(struct aios_http_pool *p, struct aios_http_client *c);
void aios_http_pool_set_timeout_ms(struct aios_http_pool *p, unsigned int ms);
void aios_http_pool_get_stats(struct aios_http_pool *p, u64 *timeouts, u64 *reconnects);
unsigned int aios_http_pool_size(struct aios_http_pool *p);

#endif /* AIOS_HTTP_API_H */
