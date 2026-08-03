#pragma once

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

fuse_operations aios_fuse_operations();
