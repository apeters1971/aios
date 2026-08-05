#include "xrd/aios_xrd_cred.h"

#include <cerrno>
#include <cstring>
#include <pwd.h>

extern "C" int aios_xrd_map_name(const char* name, uint32_t* uid_out, uint32_t* gid_out) {
  if (!name || !*name || !uid_out || !gid_out) return -EACCES;
  struct passwd pwd{};
  struct passwd* res = nullptr;
  char buf[4096];
  const int rc = getpwnam_r(name, &pwd, buf, sizeof(buf), &res);
  if (rc != 0 || !res) return -EACCES;
  *uid_out = static_cast<uint32_t>(res->pw_uid);
  *gid_out = static_cast<uint32_t>(res->pw_gid);
  return 0;
}
