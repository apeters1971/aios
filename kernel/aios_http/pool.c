// SPDX-License-Identifier: GPL-2.0
/*
 * Shared HTTP client pool for aiosvd and aiosfs.
 */
#include "internal.h"

#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

struct aios_http_pool *aios_http_pool_create(const char *endpoint, const char *cluster_key,
					     const char *app_label, unsigned int n, gfp_t gfp)
{
	struct aios_http_pool *p;
	unsigned int i;

	if (!endpoint || !cluster_key || !*endpoint || !*cluster_key)
		return ERR_PTR(-EINVAL);
	if (n < 1)
		n = 1;
	if (n > BITS_PER_LONG)
		n = BITS_PER_LONG;

	p = kzalloc(sizeof(*p), gfp);
	if (!p)
		return ERR_PTR(-ENOMEM);

	p->clients = kcalloc(n, sizeof(*p->clients), gfp);
	if (!p->clients) {
		kfree(p);
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0; i < n; i++) {
		struct aios_http_client *c;

		c = aios_http_client_create(endpoint, cluster_key, gfp);
		if (IS_ERR(c)) {
			int err = PTR_ERR(c);

			while (i--)
				aios_http_client_destroy(p->clients[i]);
			kfree(p->clients);
			kfree(p);
			return ERR_PTR(err);
		}
		if (app_label && app_label[0])
			aios_http_client_set_app_label(c, app_label);
		p->clients[i] = c;
	}
	p->n = n;
	p->busy = 0;
	spin_lock_init(&p->lock);
	sema_init(&p->sem, n);
	return p;
}
EXPORT_SYMBOL_GPL(aios_http_pool_create);

void aios_http_pool_destroy(struct aios_http_pool *p)
{
	unsigned int i;

	if (!p)
		return;
	for (i = 0; i < p->n; i++)
		aios_http_client_destroy(p->clients[i]);
	kfree(p->clients);
	kfree(p);
}
EXPORT_SYMBOL_GPL(aios_http_pool_destroy);

struct aios_http_client *aios_http_pool_get(struct aios_http_pool *p)
{
	struct aios_http_client *c = NULL;
	unsigned int i;

	if (!p || !p->n)
		return NULL;
	down(&p->sem);
	spin_lock(&p->lock);
	for (i = 0; i < p->n; i++) {
		if (!test_and_set_bit(i, &p->busy)) {
			c = p->clients[i];
			break;
		}
	}
	spin_unlock(&p->lock);
	return c;
}
EXPORT_SYMBOL_GPL(aios_http_pool_get);

void aios_http_pool_put(struct aios_http_pool *p, struct aios_http_client *c)
{
	unsigned int i;

	if (!p || !c)
		return;
	spin_lock(&p->lock);
	for (i = 0; i < p->n; i++) {
		if (p->clients[i] == c) {
			clear_bit(i, &p->busy);
			break;
		}
	}
	spin_unlock(&p->lock);
	up(&p->sem);
}
EXPORT_SYMBOL_GPL(aios_http_pool_put);

void aios_http_pool_set_timeout_ms(struct aios_http_pool *p, unsigned int ms)
{
	unsigned int i;

	if (!p)
		return;
	for (i = 0; i < p->n; i++)
		aios_http_client_set_timeout_ms(p->clients[i], ms);
}
EXPORT_SYMBOL_GPL(aios_http_pool_set_timeout_ms);

void aios_http_pool_get_stats(struct aios_http_pool *p, u64 *timeouts, u64 *reconnects)
{
	unsigned int i;
	u64 t = 0, r = 0;

	if (!p) {
		if (timeouts)
			*timeouts = 0;
		if (reconnects)
			*reconnects = 0;
		return;
	}
	for (i = 0; i < p->n; i++) {
		u64 ct = 0, cr = 0;

		aios_http_client_get_stats(p->clients[i], &ct, &cr);
		t += ct;
		r += cr;
	}
	if (timeouts)
		*timeouts = t;
	if (reconnects)
		*reconnects = r;
}
EXPORT_SYMBOL_GPL(aios_http_pool_get_stats);

unsigned int aios_http_pool_size(struct aios_http_pool *p)
{
	return p ? p->n : 0;
}
EXPORT_SYMBOL_GPL(aios_http_pool_size);
