#pragma once

#include <sys/types.h>
#include <unistd.h>

namespace aios {

inline uid_t effective_uid() { return ::geteuid(); }
inline gid_t effective_gid() { return ::getegid(); }

}  // namespace aios
