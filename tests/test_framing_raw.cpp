#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "net/framing.hpp"

#include <cstring>
#include <string>
#include <vector>

TEST(FramingRaw, RoundTrip) {
  using namespace aios;
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
  EXPECT_TRUE(bytes.size() >= kHeaderSize) << "encoded has header";
  EXPECT_TRUE(std::memcmp(bytes.data(), "AIOS", 4) == 0) << "magic";
  EXPECT_TRUE(bytes[5] == static_cast<std::uint8_t>(MsgType::ObjectPutRange)) << "type byte";

  Frame out;
  std::size_t consumed = 0;
  std::string err;
  EXPECT_TRUE(decode_frame(bytes.data(), bytes.size(), out, consumed, err)) << "decode ok";
  EXPECT_TRUE(err.empty()) << "no err";
  EXPECT_TRUE(consumed == bytes.size()) << "consumed all";
  EXPECT_TRUE(out.type == MsgType::ObjectPutRange) << "type roundtrip";
  EXPECT_TRUE((out.flags & kFlagRawBody) != 0) << "raw flag set";
  EXPECT_TRUE(out.body.value("oid", "") == "raw-obj") << "json oid";
  EXPECT_TRUE(out.body.value("offset", static_cast<std::uint64_t>(0)) == 4) << "json offset";
  EXPECT_TRUE(std::string(out.raw.begin(), out.raw.end()) == raw_bytes) << "raw bytes roundtrip";

  EXPECT_TRUE(std::string(msg_type_name(MsgType::ObjectPublishTip)) == "ObjectPublishTip") << "msg ObjectPublishTip";
  EXPECT_TRUE(std::string(msg_type_name(MsgType::ObjectAbortVersion)) == "ObjectAbortVersion") << "msg ObjectAbortVersion";
  EXPECT_TRUE(std::string(msg_type_name(MsgType::ObjectListVersions)) == "ObjectListVersions") << "msg ObjectListVersions";
  EXPECT_TRUE(std::string(msg_type_name(MsgType::ObjectPurgeVersions)) == "ObjectPurgeVersions") << "msg ObjectPurgeVersions";

  }
