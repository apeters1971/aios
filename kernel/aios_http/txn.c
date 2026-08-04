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
	char enc[1024];
	char path[1100];
	char extra[64];
	struct aios_http_buf body = { 0 };
	int status = 0;
	int err;

	if (!token_out || token_len < 8)
		return -EINVAL;
	err = aios_http_encode_oid(oid, enc, sizeof(enc));
	if (err)
		return err;
	if (snprintf(path, sizeof(path), "/o/%s/lock", enc) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	snprintf(extra, sizeof(extra), "x-aios-lock-ttl-ms: %d\r\n", ttl_ms > 0 ? ttl_ms : 30000);

	err = aios_http_request(c, "POST", path, extra, NULL, 0, &status, &body);
	if (err)
		return err;
	if (status != 201) {
		aios_http_buf_free(&body);
		return aios_http_map_status(status);
	}
	err = aios_http_json_string(body.data, body.len, "token", token_out, token_len);
	aios_http_buf_free(&body);
	return err;
}
EXPORT_SYMBOL_GPL(aios_http_lock_acquire);

int aios_http_lock_release(struct aios_http_client *c, const char *oid, const char *token)
{
	char enc[1024];
	char path[1100];
	char extra[192];
	int status = 0;
	int err;

	if (!token || !*token)
		return -EINVAL;
	err = aios_http_encode_oid(oid, enc, sizeof(enc));
	if (err)
		return err;
	if (snprintf(path, sizeof(path), "/o/%s/lock", enc) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	snprintf(extra, sizeof(extra), "x-aios-lock-token: %s\r\n", token);
	err = aios_http_request(c, "DELETE", path, extra, NULL, 0, &status, NULL);
	if (err)
		return err;
	return aios_http_map_status(status);
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
	char tenc[256];
	char oenc[1024];
	int err;

	err = aios_http_encode_oid(txn_id, tenc, sizeof(tenc));
	if (err)
		return err;
	err = aios_http_encode_oid(oid, oenc, sizeof(oenc));
	if (err)
		return err;
	if (snprintf(path, path_len, "/txn/%s/o/%s", tenc, oenc) >= (int)path_len)
		return -ENAMETOOLONG;
	return 0;
}

int aios_http_txn_prepare_put(struct aios_http_client *c, const char *txn_id, const char *oid,
			      const void *body, size_t len, const char *lock_token,
			      u64 *cas_inout)
{
	char path[1200];
	char cas_hdrs[512] = "";
	char lock_hdr[192] = "";
	char all[900];
	int status = 0;
	int err;
	u64 new_cas = 0;

	if (!txn_id || !*txn_id || !oid || !*oid)
		return -EINVAL;
	if (len > AIOS_HTTP_MAX_BODY)
		return -EFBIG;
	err = txn_oid_path(txn_id, oid, path, sizeof(path));
	if (err)
		return err;

	if (cas_inout) {
		err = aios_http_fill_posix_cas(c, oid, *cas_inout, cas_hdrs, sizeof(cas_hdrs),
					       &new_cas);
		if (err)
			return err;
	}
	if (lock_token && *lock_token)
		snprintf(lock_hdr, sizeof(lock_hdr), "x-aios-lock-token: %s\r\n", lock_token);

	snprintf(all, sizeof(all), "Content-Type: application/octet-stream\r\n%s%s", cas_hdrs,
		 lock_hdr);
	err = aios_http_request(c, "PUT", path, all, body, len, &status, NULL);
	if (err)
		return err;
	err = aios_http_map_status(status);
	if (err)
		return err;
	if (cas_inout)
		*cas_inout = new_cas;
	return 0;
}
EXPORT_SYMBOL_GPL(aios_http_txn_prepare_put);

int aios_http_txn_prepare_delete(struct aios_http_client *c, const char *txn_id, const char *oid,
				 const char *lock_token)
{
	char path[1200];
	char lock_hdr[192] = "";
	int status = 0;
	int err;

	if (!txn_id || !*txn_id || !oid || !*oid)
		return -EINVAL;
	err = txn_oid_path(txn_id, oid, path, sizeof(path));
	if (err)
		return err;
	if (lock_token && *lock_token)
		snprintf(lock_hdr, sizeof(lock_hdr), "x-aios-lock-token: %s\r\n", lock_token);
	err = aios_http_request(c, "DELETE", path, lock_hdr[0] ? lock_hdr : NULL, NULL, 0,
				&status, NULL);
	if (err)
		return err;
	return aios_http_map_status(status);
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
