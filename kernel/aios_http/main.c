// SPDX-License-Identifier: GPL-2.0
/*
 * aios_http — in-kernel AIOS HTTP/1.1 + HMAC client (AlmaLinux 9 / RHEL 9).
 */
#include "internal.h"

#include <linux/module.h>

static int __init aios_http_init(void)
{
	pr_info("aios_http: loaded (in-kernel AIOS HTTP client)\n");
	return 0;
}

static void __exit aios_http_exit(void)
{
	pr_info("aios_http: unloaded\n");
}

module_init(aios_http_init);
module_exit(aios_http_exit);

MODULE_AUTHOR("AIOS");
MODULE_DESCRIPTION("AIOS in-kernel HTTP/1.1 + HMAC client");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.2.0");
