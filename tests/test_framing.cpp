#include "net/framing.hpp"
#include <gtest/gtest.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>


TEST(Framing, RoundTrip) {
  using namespace aios;

  Frame in;
  in.type = MsgType::Gossip;
  in.body = {
      {"membership", {{"members", nlohmann::json::array()}}},
      {"fs_table", {{"entries", nlohmann::json::array()}}},
  };

  auto bytes = encode_frame(in);
  EXPECT_TRUE(bytes.size() >= kHeaderSize) << "encoded has header";
  EXPECT_TRUE(std::memcmp(bytes.data(), "AIOS", 4) == 0) << "magic";
  EXPECT_TRUE(bytes[4] == kProtoVersion) << "version";
  EXPECT_TRUE(bytes[5] == static_cast<std::uint8_t>(MsgType::Gossip)) << "type";

  Frame out;
  std::size_t consumed = 0;
  std::string err;
  EXPECT_TRUE(decode_frame(bytes.data(), bytes.size(), out, consumed, err)) << "decode ok";
  EXPECT_TRUE(err.empty()) << "no err";
  EXPECT_TRUE(consumed == bytes.size()) << "consumed all";
  EXPECT_TRUE(out.type == MsgType::Gossip) << "type roundtrip";
  EXPECT_TRUE(out.body.contains("membership")) << "body membership";

  // Incomplete
  Frame partial;
  EXPECT_TRUE(!decode_frame(bytes.data(), 4, partial, consumed, err)) << "incomplete fails";

  // Bad magic
  auto bad = bytes;
  bad[0] = 'X';
  EXPECT_TRUE(!decode_frame(bad.data(), bad.size(), partial, consumed, err)) << "bad magic";

  // Hello encode
  Frame hello;
  hello.type = MsgType::Hello;
  hello.body = {{"node_id", "n1"}, {"listen", "127.0.0.1:7400"}};
  auto hb = encode_frame(hello);
  EXPECT_TRUE(decode_frame(hb.data(), hb.size(), out, consumed, err)) << "hello decode";
  EXPECT_TRUE(out.body["node_id"] == "n1") << "hello node_id";

  }
