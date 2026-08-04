// SPDX-License-Identifier: GPL-2.0
#include "aiosvd.h"

#include <linux/kernel.h>

void aiosvd_header_oid(const char *pool, const char *name, char *out, size_t n)
{
	snprintf(out, n, "vd/%s/%s/header", pool, name);
}

void aiosvd_data_oid(const char *pool, const char *name, u64 objno, char *out, size_t n)
{
	snprintf(out, n, "vd/%s/%s/data.%016llx", pool, name, (unsigned long long)objno);
}
