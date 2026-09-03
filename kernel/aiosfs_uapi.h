/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Shared userspace ↔ kernel ↔ FUSE ioctl ABI for aiosfs.
 * The PREFETCHV argument is a self-contained inline range vector so the same
 * ioctl(2) works on an in-kernel aiosfs mount and on aios-fuse / aios-fusell
 * without nested userspace pointers (no FUSE_IOCTL_RETRY).
 */
#ifndef AIOSFS_UAPI_H
#define AIOSFS_UAPI_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __KERNEL__
typedef uint32_t __u32;
typedef uint64_t __u64;
#endif

#define AIOSFS_IOCTL_MAGIC 0xA2

#define AIOS_PREFETCH_MAX_RANGES 256
#define AIOS_PREFETCH_MAX_BYTES (256ull * 1024ull * 1024ull)
#define AIOS_PREFETCH_SUPPORTED_FLAGS 0u

struct aios_range {
	__u64 offset;
	__u64 length;
};

struct aios_prefetchv {
	__u32 nranges;
	__u32 flags;
	struct aios_range ranges[AIOS_PREFETCH_MAX_RANGES];
};

#define AIOS_IOC_PREFETCHV _IOW(AIOSFS_IOCTL_MAGIC, 0x01, struct aios_prefetchv)

#ifndef __KERNEL__
/* Same helper on a kernel aiosfs mount and on a FUSE mount. */
static inline int aios_prefetchv(int fd, const struct aios_range *ranges, uint32_t nranges)
{
	struct aios_prefetchv req;

	if (!ranges)
		return -EINVAL;
	if (nranges == 0 || nranges > AIOS_PREFETCH_MAX_RANGES)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.nranges = nranges;
	req.flags = 0;
	memcpy(req.ranges, ranges, (size_t)nranges * sizeof(ranges[0]));

	if (ioctl(fd, AIOS_IOC_PREFETCHV, &req) < 0)
		return -errno;
	return 0;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* AIOSFS_UAPI_H */
