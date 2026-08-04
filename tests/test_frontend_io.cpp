#include "metrics/frontend_io.hpp"
#include "test_helpers.hpp"

#include <iostream>

namespace {

int& failures() {
  static int n = 0;
  return n;
}
void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL frontend_io: " << msg << "\n";
    ++failures();
  }
}

}  // namespace

int test_frontend_io() {
  using namespace aios;
  failures() = 0;

  note_frontend_io(kFrontendS3, true, 100);
  note_frontend_io(kFrontendS3, false, 50);
  note_frontend_io(kFrontendFs, true, 200);
  note_frontend_io("custom", true, 7);

  auto logical = frontend_io_json();
  expect(logical.contains("s3") && logical.contains("fs") && logical.contains("vbd"),
         "reserved keys");
  expect(logical["s3"].value("write_bytes", 0ull) >= 100, "s3 write bytes");
  expect(logical["s3"].value("read_bytes", 0ull) >= 50, "s3 read bytes");
  expect(logical["fs"].value("write_bytes", 0ull) >= 200, "fs write bytes");
  expect(logical.contains("custom"), "custom frontend");

  auto admin = io_frontends_admin_json(nlohmann::json{{"s3", {{"put", 1}}}});
  expect(admin.contains("logical") && admin.contains("object_ops") && admin.contains("vbd_devices"),
         "admin shape");
  expect(admin["object_ops"]["s3"].value("put", 0) == 1, "object slice");

  if (failures() == 0) std::cout << "test_frontend_io OK\n";
  return failures();
}
