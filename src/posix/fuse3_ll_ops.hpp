#pragma once

#define FUSE_USE_VERSION 31
#include <fuse3/fuse_lowlevel.h>

#include "posix/aios_posix.h"

unsigned aios_fuse_ll_max_io(const aios_posix_fs* fs);
fuse_lowlevel_ops aios_fuse_ll_operations();
