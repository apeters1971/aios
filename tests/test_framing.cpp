#include "net/framing.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
}

}  // namespace

int test_framing() {
  using namespace aios;

  Frame in;
  in.type = MsgType::Gossip;
  in.body = {
      {"membership", {{"members", nlohmann::json::array()}}},
      {"fs_table", {{"entries", nlohmann::json::array()}}},
  };

  auto bytes = encode_frame(in);
  expect(bytes.size() >= kHeaderSize, "encoded has header");
  expect(std::memcmp(bytes.data(), "AIOS", 4) == 0, "magic");
  expect(bytes[4] == kProtoVersion, "version");
  expect(bytes[5] == static_cast<std::uint8_t>(MsgType::Gossip), "type");

  Frame out;
  std::size_t consumed = 0;
  std::string err;
  expect(decode_frame(bytes.data(), bytes.size(), out, consumed, err), "decode ok");
  expect(err.empty(), "no err");
  expect(consumed == bytes.size(), "consumed all");
  expect(out.type == MsgType::Gossip, "type roundtrip");
  expect(out.body.contains("membership"), "body membership");

  // Incomplete
  Frame partial;
  expect(!decode_frame(bytes.data(), 4, partial, consumed, err), "incomplete fails");

  // Bad magic
  auto bad = bytes;
  bad[0] = 'X';
  expect(!decode_frame(bad.data(), bad.size(), partial, consumed, err), "bad magic");

  // Hello encode
  Frame hello;
  hello.type = MsgType::Hello;
  hello.body = {{"node_id", "n1"}, {"listen", "127.0.0.1:7400"}};
  auto hb = encode_frame(hello);
  expect(decode_frame(hb.data(), hb.size(), out, consumed, err), "hello decode");
  expect(out.body["node_id"] == "n1", "hello node_id");

  return failures;
}
