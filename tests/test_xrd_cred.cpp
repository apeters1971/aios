#include "test_helpers.hpp"
#include <gtest/gtest.h>
#include "xrd/aios_xrd_cred.h"

#include <pwd.h>
#include <unistd.h>

#include <cerrno>
#include <string>

TEST(XrdCred, Basic) {
  EXPECT_TRUE(aios_xrd_map_name(nullptr, nullptr, nullptr) == -EACCES) << "null args";
  uint32_t uid = 1, gid = 1;
  EXPECT_TRUE(aios_xrd_map_name("", &uid, &gid) == -EACCES) << "empty name";
  EXPECT_TRUE(aios_xrd_map_name("___no_such_user_aios_xyz___", &uid, &gid) == -EACCES) << "unknown user";

  const uid_t self = ::getuid();
  struct passwd pwd{};
  struct passwd* res = nullptr;
  char buf[4096];
  EXPECT_TRUE(getpwuid_r(self, &pwd, buf, sizeof(buf), &res) == 0 && res != nullptr) << "getpwuid self";
  if (res) {
    uint32_t mu = 0, mg = 0;
    EXPECT_TRUE(aios_xrd_map_name(res->pw_name, &mu, &mg) == 0) << "map self name";
    EXPECT_TRUE(mu == static_cast<uint32_t>(res->pw_uid)) << "uid match";
    EXPECT_TRUE(mg == static_cast<uint32_t>(res->pw_gid)) << "gid match";
  }

  }
