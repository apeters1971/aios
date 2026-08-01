#include "test_helpers.hpp"

#include "net/framing.hpp"

#include <cstring>
#include <string>
#include <vector>

int test_framing_raw() {
  using namespace aios;
  using aios::test::expect;
  using aios::test::failures;

  failures() = 0;

  Frame in;
  in.type = MsgType::ObjectPutRange;
  in.flags = kFlagRawBody;
  in.body = {
      {"epoch", 1},
      {"aios_path", "/tmp/aios"},
      {"oid", "raw-obj"},
      {"offset", 4},
      {"role", "primary"},
  };
  const std::string raw_bytes = "RAWDATA!!";
  in.raw.assign(raw_bytes.begin(), raw_bytes.end());

  auto bytes = encode_frame(in);
  expect(bytes.size() >= kHeaderSize, "encoded has header");
  expect(std::memcmp(bytes.data(), "AIOS", 4) == 0, "magic");
  expect(bytes[5] == static_cast<std::uint8_t>(MsgType::ObjectPutRange), "type byte");

  Frame out;
  std::size_t consumed = 0;
  std::string err;
  expect(decode_frame(bytes.data(), bytes.size(), out, consumed, err), "decode ok");
  expect(err.empty(), "no err");
  expect(consumed == bytes.size(), "consumed all");
  expect(out.type == MsgType::ObjectPutRange, "type roundtrip");
  expect((out.flags & kFlagRawBody) != 0, "raw flag set");
  expect(out.body.value("oid", "") == "raw-obj", "json oid");
  expect(out.body.value("offset", static_cast<std::uint64_t>(0)) == 4, "json offset");
  expect(std::string(out.raw.begin(), out.raw.end()) == raw_bytes, "raw bytes roundtrip");

  expect(std::string(msg_type_name(MsgType::ObjectPublishTip)) == "ObjectPublishTip",
         "msg ObjectPublishTip");
  expect(std::string(msg_type_name(MsgType::ObjectAbortVersion)) == "ObjectAbortVersion",
         "msg ObjectAbortVersion");
  expect(std::string(msg_type_name(MsgType::ObjectListVersions)) == "ObjectListVersions",
         "msg ObjectListVersions");
  expect(std::string(msg_type_name(MsgType::ObjectPurgeVersions)) == "ObjectPurgeVersions",
         "msg ObjectPurgeVersions");

  return failures();
}
