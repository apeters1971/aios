// SPDX-License-Identifier: GPL-2.0
#include "internal.h"

#include <crypto/hash.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>

int aios_http_hmac_sha256_hex(const char *key, size_t key_len, const char *data,
			      size_t data_len, char *hex_out)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	u8 digest[32];
	int err;
	int i;

	if (!key || !data || !hex_out)
		return -EINVAL;

	tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	err = crypto_shash_setkey(tfm, key, key_len);
	if (err)
		goto out_tfm;

	desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		err = -ENOMEM;
		goto out_tfm;
	}
	desc->tfm = tfm;
	err = crypto_shash_digest(desc, data, data_len, digest);
	kfree(desc);
	if (err)
		goto out_tfm;

	for (i = 0; i < 32; i++)
		sprintf(hex_out + i * 2, "%02x", digest[i]);
	hex_out[64] = '\0';

out_tfm:
	crypto_free_shash(tfm);
	return err;
}

int aios_http_sha256_hex(struct aios_http_client *c, const void *body, size_t body_len,
			 char *hex_out)
{
	u8 digest[32];
	int err;
	int i;

	if (!c || !hex_out || (body_len && !body))
		return -EINVAL;

	if (!c->sha256) {
		struct crypto_shash *tfm = crypto_alloc_shash("sha256", 0, 0);

		if (IS_ERR(tfm))
			return PTR_ERR(tfm);
		c->sha256 = tfm;
	}
	{
		SHASH_DESC_ON_STACK(desc, c->sha256);

		desc->tfm = c->sha256;
		err = crypto_shash_digest(desc, body_len ? body : (const u8 *)"", body_len,
					  digest);
		shash_desc_zero(desc);
	}
	if (err)
		return err;

	for (i = 0; i < 32; i++)
		sprintf(hex_out + i * 2, "%02x", digest[i]);
	hex_out[64] = '\0';
	return 0;
}

static void random_hex(char *out, size_t bytes)
{
	static const char hex[] = "0123456789abcdef";
	u8 raw[32];
	size_t i;

	if (bytes > sizeof(raw))
		bytes = sizeof(raw);
	get_random_bytes(raw, bytes);
	for (i = 0; i < bytes; i++) {
		out[i * 2] = hex[raw[i] >> 4];
		out[i * 2 + 1] = hex[raw[i] & 0xf];
	}
	out[bytes * 2] = '\0';
}

static bool const_time_eq64(const char *a, const char *b)
{
	unsigned char diff = 0;
	int i;

	for (i = 0; i < 64; i++)
		diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
	return diff == 0;
}

/*
 * Ticket grant, one round trip (mirrors src/util/ticket.cpp):
 *
 *   -> POST /auth/ticket {"principal","ts","nonce","proof"}
 *        proof = HMAC(principal_key, "aios-ticket-req-v1\n" principal "\n" ts "\n" nonce)
 *   <- {"ticket","server_nonce","expires_ms","server_proof"}
 *        session_key  = HMAC_hex(principal_key, "aios-session-v1\n" nonce "\n" server_nonce)
 *        server_proof = HMAC(session_key, "aios-ticket-reply-v1\n" nonce "\n" ticket "\n"
 *                                          server_nonce "\n" expires_ms)
 *
 * Only HMAC-SHA256 is needed on this side; the ticket itself is opaque.
 * Caller holds c->mu. Runs a nested tcp request with in_ticket_exchange set so
 * build_auth emits no credential for it.
 */
static int ticket_exchange(struct aios_http_client *c)
{
	char nonce[33];
	char proof[65];
	char server_nonce[65];
	char server_proof[65];
	char expect[65];
	char *canon = NULL;
	char *body = NULL;
	char *ticket = NULL;
	struct aios_http_buf resp = { 0 };
	s64 now_ms;
	s64 expires_ms = 0;
	int status = 0;
	int n;
	int err;

	now_ms = ktime_to_ms(ktime_get_real());
	random_hex(nonce, 16);

	canon = kmalloc(AIOS_HTTP_CANON_MAX + AIOS_HTTP_TICKET_MAX, c->gfp);
	body = kmalloc(512, c->gfp);
	ticket = kmalloc(AIOS_HTTP_TICKET_MAX, c->gfp);
	if (!canon || !body || !ticket) {
		err = -ENOMEM;
		goto out;
	}

	n = snprintf(canon, AIOS_HTTP_CANON_MAX, "aios-ticket-req-v1\n%s\n%lld\n%s",
		     c->principal, (long long)now_ms, nonce);
	if (n < 0 || n >= (int)AIOS_HTTP_CANON_MAX) {
		err = -EOVERFLOW;
		goto out;
	}
	err = aios_http_hmac_sha256_hex(c->cluster_key, strlen(c->cluster_key), canon, n, proof);
	if (err)
		goto out;

	n = snprintf(body, 512, "{\"principal\":\"%s\",\"ts\":%lld,\"nonce\":\"%s\",\"proof\":\"%s\"}",
		     c->principal, (long long)now_ms, nonce, proof);
	if (n < 0 || n >= 512) {
		err = -EOVERFLOW;
		goto out;
	}

	c->in_ticket_exchange = true;
	err = aios_http_tcp_request(c, "POST", "/auth/ticket",
				    "Content-Type: application/json\r\n", body, n, &status, NULL,
				    0, &resp, NULL, 0);
	c->in_ticket_exchange = false;
	if (err)
		goto out;
	if (status != 200) {
		pr_warn("aios_http: ticket grant for %s refused: HTTP %d\n", c->principal,
			status);
		err = status == 429 ? -EBUSY : -EACCES;
		goto out;
	}
	if (!resp.data || !resp.len) {
		err = -EIO;
		goto out;
	}

	err = aios_http_json_string(resp.data, resp.len, "ticket", ticket, AIOS_HTTP_TICKET_MAX);
	if (!err)
		err = aios_http_json_string(resp.data, resp.len, "server_nonce", server_nonce,
					    sizeof(server_nonce));
	if (!err)
		err = aios_http_json_string(resp.data, resp.len, "server_proof", server_proof,
					    sizeof(server_proof));
	if (!err)
		err = aios_http_json_s64(resp.data, resp.len, "expires_ms", &expires_ms);
	if (err) {
		pr_warn("aios_http: malformed ticket reply for %s (%d)\n", c->principal, err);
		err = -EIO;
		goto out;
	}
	if (strncmp(ticket, "t1.", 3) != 0 || strlen(server_proof) != 64 || expires_ms <= now_ms) {
		err = -EIO;
		goto out;
	}

	/* session_key = HMAC_hex(principal_key, "aios-session-v1\n" nonce "\n" server_nonce) */
	n = snprintf(canon, AIOS_HTTP_CANON_MAX, "aios-session-v1\n%s\n%s", nonce, server_nonce);
	if (n < 0 || n >= (int)AIOS_HTTP_CANON_MAX) {
		err = -EOVERFLOW;
		goto out;
	}
	err = aios_http_hmac_sha256_hex(c->cluster_key, strlen(c->cluster_key), canon, n, expect);
	if (err)
		goto out;
	/* expect now holds the candidate session key; check the server's proof with it. */
	n = snprintf(canon, AIOS_HTTP_CANON_MAX + AIOS_HTTP_TICKET_MAX,
		     "aios-ticket-reply-v1\n%s\n%s\n%s\n%lld", nonce, ticket, server_nonce,
		     (long long)expires_ms);
	if (n < 0 || n >= (int)(AIOS_HTTP_CANON_MAX + AIOS_HTTP_TICKET_MAX)) {
		err = -EOVERFLOW;
		goto out;
	}
	{
		char check[65];

		err = aios_http_hmac_sha256_hex(expect, 64, canon, n, check);
		if (err)
			goto out;
		if (!const_time_eq64(check, server_proof)) {
			/* Wrong key on our side, or someone answering in the cluster's name. */
			pr_warn("aios_http: ticket reply for %s failed server proof\n",
				c->principal);
			err = -EACCES;
			goto out;
		}
	}

	strscpy(c->ticket, ticket, sizeof(c->ticket));
	memcpy(c->session_key, expect, 64);
	c->session_key[64] = '\0';
	c->ticket_issued_ms = now_ms;
	c->ticket_expires_ms = expires_ms;
	err = 0;

out:
	aios_http_buf_free(&resp);
	kfree(canon);
	kfree(body);
	kfree(ticket);
	memzero_explicit(expect, sizeof(expect));
	return err;
}

int aios_http_ensure_ticket(struct aios_http_client *c, bool force)
{
	s64 now_ms;
	s64 half;
	int err;

	if (!c || !c->principal[0])
		return 0;
	now_ms = ktime_to_ms(ktime_get_real());
	if (!force && c->ticket[0]) {
		half = c->ticket_issued_ms + (c->ticket_expires_ms - c->ticket_issued_ms) / 2;
		if (now_ms < half)
			return 0;
	}
	err = ticket_exchange(c);
	/*
	 * A failed renewal is not fatal while the old ticket is still valid
	 * (the node may be briefly unreachable); the next request retries.
	 */
	if (err && !force && c->ticket[0] && now_ms < c->ticket_expires_ms)
		return 0;
	return err;
}

int aios_http_build_auth(struct aios_http_client *c, const char *method,
			 const char *path, const void *body, size_t body_len,
			 char *auth_hdrs, size_t auth_hdrs_len)
{
	/* Canonical: method\npath\ndate\nSignedHeaders:\nSignedHeaders\n<body sha256 hex> */
	char date[32];
	char *canon;
	char sig[65];
	char body_sha[65];
	const char *key;
	const char *credential;
	s64 ms;
	int n;
	int err;

	if (!c || !method || !path || !auth_hdrs)
		return -EINVAL;

	/* The grant request carries its proof in the body; no credential yet. */
	if (c->in_ticket_exchange) {
		if (auth_hdrs_len)
			auth_hdrs[0] = '\0';
		return 0;
	}

	if (c->principal[0]) {
		err = aios_http_ensure_ticket(c, false);
		if (err)
			return err;
		key = c->session_key;
		credential = c->ticket;
	} else {
		key = c->cluster_key;
		credential = "stl";
	}

	/*
	 * Sign the body's digest rather than UNSIGNED-PAYLOAD: the server checks
	 * the received bytes against it, so an on-path party cannot swap the
	 * payload of an otherwise valid request. Costs one SHA-256 pass per PUT.
	 */
	err = aios_http_sha256_hex(c, body, body_len, body_sha);
	if (err)
		return err;

	ms = ktime_to_ms(ktime_get_real());
	snprintf(date, sizeof(date), "%lld", (long long)ms);

	canon = kmalloc(AIOS_HTTP_CANON_MAX, c->gfp);
	if (!canon)
		return -ENOMEM;

	/*
	 * Must match src/http/http_auth.cpp http_canonical(). SignedHeaders is
	 * split on commas only, so "x-aios-content-sha256;x-aios-date" is one
	 * header name and that line's value is empty. Expanding into two
	 * name:value lines (AWS-style) produces a 401 bad signature.
	 */
	n = snprintf(canon, AIOS_HTTP_CANON_MAX,
		     "%s\n%s\n%s\n"
		     "x-aios-content-sha256;x-aios-date:\n"
		     "x-aios-content-sha256;x-aios-date\n"
		     "%s",
		     method, path, date, body_sha);
	if (n < 0 || n >= (int)AIOS_HTTP_CANON_MAX) {
		kfree(canon);
		return -EOVERFLOW;
	}

	err = aios_http_hmac_sha256_hex(key, strlen(key), canon, strlen(canon), sig);
	kfree(canon);
	if (err)
		return err;

	n = snprintf(auth_hdrs, auth_hdrs_len,
		     "x-aios-date: %s\r\n"
		     "x-aios-content-sha256: %s\r\n"
		     "Authorization: AIOS-HMAC-SHA256 Credential=%s, "
		     "SignedHeaders=x-aios-content-sha256;x-aios-date, Signature=%s\r\n",
		     date, body_sha, credential, sig);
	if (n < 0 || n >= (int)auth_hdrs_len)
		return -EOVERFLOW;
	return 0;
}
