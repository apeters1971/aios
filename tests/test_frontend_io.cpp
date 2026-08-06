#include "metrics/frontend_io.hpp"
#include <gtest/gtest.h>
#include "test_helpers.hpp"

#include <iostream>


TEST(FrontendIo, Basic) {
  using namespace aios;
  note_frontend_io(kFrontendS3, true, 100);
  note_frontend_io(kFrontendS3, false, 50);
  note_frontend_io(kFrontendFs, true, 200);
  note_frontend_io("custom", true, 7);

  auto logical = frontend_io_json();
  EXPECT_TRUE(logical.contains("s3") && logical.contains("fs") && logical.contains("vbd")) << "reserved keys";
  EXPECT_TRUE(logical["s3"].value("write_bytes", 0ull) >= 100) << "s3 write bytes";
  EXPECT_TRUE(logical["s3"].value("read_bytes", 0ull) >= 50) << "s3 read bytes";
  EXPECT_TRUE(logical["fs"].value("write_bytes", 0ull) >= 200) << "fs write bytes";
  EXPECT_TRUE(logical.contains("custom")) << "custom frontend";

  auto admin = io_frontends_admin_json(nlohmann::json{{"s3", {{"put", 1}}}});
  EXPECT_TRUE(admin.contains("logical") && admin.contains("object_ops") && admin.contains("vbd_devices")) << "admin shape";
  EXPECT_TRUE(admin["object_ops"]["s3"].value("put", 0) == 1) << "object slice";

  }
