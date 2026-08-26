#pragma once

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#include "posix/aios_posix.h"

unsigned aios_fuse_max_io(const aios_posix_fs* fs);
fuse_operations aios_fuse_operations();
