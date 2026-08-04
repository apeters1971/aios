// SPDX-License-Identifier: GPL-2.0
#include "aiosvd.h"

struct aios_http_client *aiosvd_client_get(struct aiosvd_device *dev)
{
	if (!dev || !dev->http_pool)
		return NULL;
	return aios_http_pool_get(dev->http_pool);
}

void aiosvd_client_put(struct aiosvd_device *dev, struct aios_http_client *c)
{
	if (!dev || !dev->http_pool || !c)
		return;
	aios_http_pool_put(dev->http_pool, c);
}

int aiosvd_clients_create(struct aiosvd_device *dev, const char *endpoint, const char *key,
			  const char *app_label, u32 n)
{
	struct aios_http_pool *p;

	if (n < 1)
		n = 1;
	if (n > AIOSVD_MAX_CLIENTS)
		n = AIOSVD_MAX_CLIENTS;

	p = aios_http_pool_create(endpoint, key, app_label, n, GFP_KERNEL);
	if (IS_ERR(p))
		return PTR_ERR(p);
	aios_http_pool_set_timeout_ms(p, 30000);
	dev->http_pool = p;
	return 0;
}

void aiosvd_clients_destroy(struct aiosvd_device *dev)
{
	if (!dev->http_pool)
		return;
	aios_http_pool_destroy(dev->http_pool);
	dev->http_pool = NULL;
}
