// SPDX-License-Identifier: GPL-2.0
/*
 * aiosfs — AlmaLinux 9 / RHEL 9 (kernel 5.14) VFS prototype for AIOS.
 *
 * Kernel half of the stack: VFS ops upcall through /dev/aios_bridge to
 * userspace aios-kbridge, which calls libaios_posix.
 */
#include "aiosfs.h"

#include <linux/module.h>

static void aios_kill_sb(struct super_block *sb)
{
	kill_anon_super(sb);
}

static struct file_system_type aios_fs_type = {
	.owner = THIS_MODULE,
	.name = AIOSFS_NAME,
	.init_fs_context = aios_init_fs_context,
	.kill_sb = aios_kill_sb,
	.fs_flags = 0,
};

static int __init aiosfs_init(void)
{
	int err;

	err = aios_upcall_init();
	if (err)
		return err;
	err = register_filesystem(&aios_fs_type);
	if (err) {
		aios_upcall_exit();
		return err;
	}
	pr_info("aiosfs: loaded (prototype, use aios-kbridge + mount -t aios)\n");
	return 0;
}

static void __exit aiosfs_exit(void)
{
	unregister_filesystem(&aios_fs_type);
	aios_upcall_exit();
	pr_info("aiosfs: unloaded\n");
}

module_init(aiosfs_init);
module_exit(aiosfs_exit);

MODULE_AUTHOR("AIOS");
MODULE_DESCRIPTION("AIOS POSIX filesystem prototype (AlmaLinux 9)");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1.0");
