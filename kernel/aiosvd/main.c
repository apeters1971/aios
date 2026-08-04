// SPDX-License-Identifier: GPL-2.0
/*
 * aiosvd — AIOS Volume Device (block device over AIOS objects).
 *
 * Maps a logical volume to /dev/aiosvdN with data striped across
 * vd/{pool}/{name}/data.* objects via aios_http.ko.
 */
#include "aiosvd.h"

#include <linux/module.h>

int aiosvd_major;
struct aiosvd_device aiosvd_devs[AIOSVD_MAX_DEVS];
DEFINE_MUTEX(aiosvd_devs_mu);

static int __init aiosvd_init(void)
{
	int err;

	aiosvd_major = register_blkdev(0, AIOSVD_DISK_PREFIX);
	if (aiosvd_major < 0)
		return aiosvd_major;

	err = aiosvd_ctl_init();
	if (err) {
		unregister_blkdev(aiosvd_major, AIOSVD_DISK_PREFIX);
		return err;
	}

	pr_info("aiosvd: loaded (ctl=/dev/%s, disks=/dev/%sN)\n", AIOSVD_CTL_NAME,
		AIOSVD_DISK_PREFIX);
	return 0;
}

static void __exit aiosvd_exit(void)
{
	aiosvd_ctl_exit();
	unregister_blkdev(aiosvd_major, AIOSVD_DISK_PREFIX);
	pr_info("aiosvd: unloaded\n");
}

module_init(aiosvd_init);
module_exit(aiosvd_exit);

MODULE_AUTHOR("AIOS");
MODULE_DESCRIPTION("AIOS Volume Device — block device over AIOS object store");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.3.0");
MODULE_SOFTDEP("pre: aios_http");
