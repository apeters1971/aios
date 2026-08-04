// SPDX-License-Identifier: GPL-2.0
#include "internal.h"

#include <crypto/hash.h>
#include <linux/kernel.h>
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

int aios_http_build_auth(const struct aios_http_client *c, const char *method,
			 const char *path, char *auth_hdrs, size_t auth_hdrs_len)
{
	/* Canonical: method\npath\ndate\nx-aios-content-sha256:...\nx-aios-date:...\n
	 * SignedHeaders\nUNSIGNED-PAYLOAD
	 * Signed header names are sorted alphabetically for the body lines. */
	char date[32];
	char canon[2048];
	char sig[65];
	s64 ms;
	int n;
	int err;

	ms = ktime_to_ms(ktime_get_real());
	snprintf(date, sizeof(date), "%lld", (long long)ms);

	n = snprintf(canon, sizeof(canon),
		     "%s\n%s\n%s\n"
		     "x-aios-content-sha256:UNSIGNED-PAYLOAD\n"
		     "x-aios-date:%s\n"
		     "x-aios-content-sha256;x-aios-date\n"
		     "UNSIGNED-PAYLOAD",
		     method, path, date, date);
	if (n < 0 || n >= (int)sizeof(canon))
		return -EOVERFLOW;

	err = aios_http_hmac_sha256_hex(c->cluster_key, strlen(c->cluster_key), canon,
					strlen(canon), sig);
	if (err)
		return err;

	n = snprintf(auth_hdrs, auth_hdrs_len,
		     "x-aios-date: %s\r\n"
		     "x-aios-content-sha256: UNSIGNED-PAYLOAD\r\n"
		     "Authorization: AIOS-HMAC-SHA256 Credential=stl, "
		     "SignedHeaders=x-aios-content-sha256;x-aios-date, Signature=%s\r\n",
		     date, sig);
	if (n < 0 || n >= (int)auth_hdrs_len)
		return -EOVERFLOW;
	return 0;
}
