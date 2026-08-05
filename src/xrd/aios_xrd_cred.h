#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Map a local account name to uid/gid via getpwnam_r.
 * Returns 0 on success, -EACCES if name is empty/unknown. */
int aios_xrd_map_name(const char* name, uint32_t* uid_out, uint32_t* gid_out);

#ifdef __cplusplus
}
#endif
