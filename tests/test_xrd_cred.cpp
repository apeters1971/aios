#include "test_helpers.hpp"
#include "xrd/aios_xrd_cred.h"

#include <pwd.h>
#include <unistd.h>

#include <cerrno>
#include <string>

namespace {

using aios::test::expect;
using aios::test::failures;

}  // namespace

int test_xrd_cred() {
  failures() = 0;

  expect(aios_xrd_map_name(nullptr, nullptr, nullptr) == -EACCES, "null args");
  uint32_t uid = 1, gid = 1;
  expect(aios_xrd_map_name("", &uid, &gid) == -EACCES, "empty name");
  expect(aios_xrd_map_name("___no_such_user_aios_xyz___", &uid, &gid) == -EACCES,
         "unknown user");

  const uid_t self = ::getuid();
  struct passwd pwd{};
  struct passwd* res = nullptr;
  char buf[4096];
  expect(getpwuid_r(self, &pwd, buf, sizeof(buf), &res) == 0 && res != nullptr, "getpwuid self");
  if (res) {
    uint32_t mu = 0, mg = 0;
    expect(aios_xrd_map_name(res->pw_name, &mu, &mg) == 0, "map self name");
    expect(mu == static_cast<uint32_t>(res->pw_uid), "uid match");
    expect(mg == static_cast<uint32_t>(res->pw_gid), "gid match");
  }

  return failures();
}
