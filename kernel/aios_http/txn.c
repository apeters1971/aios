// SPDX-License-Identifier: GPL-2.0
/*
 * Object locks + cross-object /txn API for aios_http.ko
 */
#include "internal.h"

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

int aios_http_lock_acquire(struct aios_http_client *c, const char *oid, int ttl_ms,
			   char *token_out, size_t token_len)
{
	struct {
		char enc[1024];
		char path[AIOS_HTTP_PATH_MAX];
		char extra[64];
	} *b;
	struct aios_http_buf body = { 0 };
	int status = 0;
	int err;

	if (!c || !token_out || token_len < 8)
		return -EINVAL;
	b = kmalloc(sizeof(*b), c->gfp);
	if (!b)
		return -ENOMEM;
	err = aios_http_encode_oid(oid, b->enc, sizeof(b->enc));
	if (err)
		goto out;
	if (snprintf(b->path, sizeof(b->path), "/o/%s/lock", b->enc) >= (int)sizeof(b->path)) {
		err = -ENAMETOOLONG;
		goto out;
	}
	snprintf(b->extra, sizeof(b->extra), "x-aios-lock-ttl-ms: %d\r\n",
		 ttl_ms > 0 ? ttl_ms : 30000);

	err = aios_http_request(c, "POST", b->path, b->extra, NULL, 0, &status, &body);
	if (err)
		goto out;
	if (status != 201) {
		aios_http_buf_free(&body);
		err = aios_http_map_status(status);
		goto out;
	}
	err = aios_http_json_string(body.data, body.len, "token", token_out, token_len);
	aios_http_buf_free(&body);
out:
	kfree(b);
	return err;
}
EXPORT_SYMBOL_GPL(aios_http_lock_acquire);

int aios_http_lock_release(struct aios_http_client *c, const char *oid, const char *token)
{
	struct {
		char enc[1024];
		char path[AIOS_HTTP_PATH_MAX];
		char extra[192];
	} *b;
	int status = 0;
	int err;

	if (!c || !token || !*token)
		return -EINVAL;
	b = kmalloc(sizeof(*b), c->gfp);
	if (!b)
		return -ENOMEM;
	err = aios_http_encode_oid(oid, b->enc, sizeof(b->enc));
	if (err)
		goto out;
	if (snprintf(b->path, sizeof(b->path), "/o/%s/lock", b->enc) >= (int)sizeof(b->path)) {
		err = -ENAMETOOLONG;
		goto out;
	}
	snprintf(b->extra, sizeof(b->extra), "x-aios-lock-token: %s\r\n", token);
	err = aios_http_request(c, "DELETE", b->path, b->extra, NULL, 0, &status, NULL);
	if (!err)
		err = aios_http_map_status(status);
out:
	kfree(b);
	return err;
}
EXPORT_SYMBOL_GPL(aios_http_lock_release);

int aios_http_txn_begin(struct aios_http_client *c, char *txn_id_out, size_t txn_id_len)
{
	struct aios_http_buf body = { 0 };
	int status = 0;
	int err;

	if (!txn_id_out || txn_id_len < 8)
		return -EINVAL;
	err = aios_http_request(c, "POST", "/txn", NULL, NULL, 0, &status, &body);
	if (err)
		return err;
	if (status != 201 && status != 200) {
		aios_http_buf_free(&body);
		return aios_http_map_status(status);
	}
	err = aios_http_json_string(body.data, body.len, "txn_id", txn_id_out, txn_id_len);
	aios_http_buf_free(&body);
	return err;
}
EXPORT_SYMBOL_GPL(aios_http_txn_begin);

static int txn_oid_path(const char *txn_id, const char *oid, char *path, size_t path_len)
{
	struct {
		char tenc[256];
		char oenc[1024];
	} *enc;
	int err;

	enc = kmalloc(sizeof(*enc), GFP_KERNEL);
	if (!enc)
		return -ENOMEM;
	err = aios_http_encode_oid(txn_id, enc->tenc, sizeof(enc->tenc));
	if (err)
		goto out;
	err = aios_http_encode_oid(oid, enc->oenc, sizeof(enc->oenc));
	if (err)
		goto out;
	if (snprintf(path, path_len, "/txn/%s/o/%s", enc->tenc, enc->oenc) >= (int)path_len)
		err = -ENAMETOOLONG;
	else
		err = 0;
out:
	kfree(enc);
	return err;
}

int aios_http_txn_prepare_put(struct aios_http_client *c, const char *txn_id, const char *oid,
			      const void *body, size_t len, const char *lock_token,
			      u64 *cas_inout)
{
	struct {
		char path[AIOS_HTTP_TXN_PATH_MAX];
		char cas_hdrs[512];
		char lock_hdr[192];
		char all[900];
	} *b;
	int status = 0;
	int err;
	u64 new_cas = 0;

	if (!c || !txn_id || !*txn_id || !oid || !*oid)
		return -EINVAL;
	if (len > AIOS_HTTP_MAX_BODY)
		return -EFBIG;
	b = kmalloc(sizeof(*b), c->gfp);
	if (!b)
		return -ENOMEM;
	b->cas_hdrs[0] = '\0';
	b->lock_hdr[0] = '\0';
	err = txn_oid_path(txn_id, oid, b->path, sizeof(b->path));
	if (err)
		goto out;

	if (cas_inout) {
		err = aios_http_fill_posix_cas(c, oid, *cas_inout, b->cas_hdrs,
					       sizeof(b->cas_hdrs), &new_cas);
		if (err)
			goto out;
	}
	if (lock_token && *lock_token)
		snprintf(b->lock_hdr, sizeof(b->lock_hdr), "x-aios-lock-token: %s\r\n",
			 lock_token);

	snprintf(b->all, sizeof(b->all), "Content-Type: application/octet-stream\r\n%s%s",
		 b->cas_hdrs, b->lock_hdr);
	err = aios_http_request(c, "PUT", b->path, b->all, body, len, &status, NULL);
	if (err)
		goto out;
	err = aios_http_map_status(status);
	if (err)
		goto out;
	if (cas_inout)
		*cas_inout = new_cas;
	err = 0;
out:
	kfree(b);
	return err;
}
EXPORT_SYMBOL_GPL(aios_http_txn_prepare_put);

int aios_http_txn_prepare_delete(struct aios_http_client *c, const char *txn_id, const char *oid,
				 const char *lock_token)
{
	struct {
		char path[AIOS_HTTP_TXN_PATH_MAX];
		char lock_hdr[192];
	} *b;
	int status = 0;
	int err;

	if (!c || !txn_id || !*txn_id || !oid || !*oid)
		return -EINVAL;
	b = kmalloc(sizeof(*b), c->gfp);
	if (!b)
		return -ENOMEM;
	b->lock_hdr[0] = '\0';
	err = txn_oid_path(txn_id, oid, b->path, sizeof(b->path));
	if (err)
		goto out;
	if (lock_token && *lock_token)
		snprintf(b->lock_hdr, sizeof(b->lock_hdr), "x-aios-lock-token: %s\r\n",
			 lock_token);
	err = aios_http_request(c, "DELETE", b->path, b->lock_hdr[0] ? b->lock_hdr : NULL, NULL,
				0, &status, NULL);
	if (!err)
		err = aios_http_map_status(status);
out:
	kfree(b);
	return err;
}
EXPORT_SYMBOL_GPL(aios_http_txn_prepare_delete);

int aios_http_txn_commit(struct aios_http_client *c, const char *txn_id)
{
	char enc[256];
	char path[300];
	int status = 0;
	int err;

	if (!txn_id || !*txn_id)
		return -EINVAL;
	err = aios_http_encode_oid(txn_id, enc, sizeof(enc));
	if (err)
		return err;
	if (snprintf(path, sizeof(path), "/txn/%s/commit", enc) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	err = aios_http_request(c, "POST", path, NULL, NULL, 0, &status, NULL);
	if (err)
		return err;
	return aios_http_map_status(status);
}
EXPORT_SYMBOL_GPL(aios_http_txn_commit);

int aios_http_txn_abort(struct aios_http_client *c, const char *txn_id)
{
	char enc[256];
	char path[300];
	int status = 0;
	int err;

	if (!txn_id || !*txn_id)
		return -EINVAL;
	err = aios_http_encode_oid(txn_id, enc, sizeof(enc));
	if (err)
		return err;
	if (snprintf(path, sizeof(path), "/txn/%s/abort", enc) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	err = aios_http_request(c, "POST", path, NULL, NULL, 0, &status, NULL);
	if (err)
		return err;
	return aios_http_map_status(status);
}
EXPORT_SYMBOL_GPL(aios_http_txn_abort);
