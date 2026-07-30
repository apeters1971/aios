#include "util/auth.hpp"

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

int test_auth() {
  using namespace aios;
  const std::string key = "550e8400-e29b-41d4-a716-446655440000";
  const std::string other = "00000000-0000-0000-0000-000000000000";

  nlohmann::json body = {{"node_id", "n1"}, {"listen", "127.0.0.1:7400"}};
  auth_sign(body, MsgType::Hello, key);
  expect(body.contains("ts"), "has ts");
  expect(body.contains("sig"), "has sig");
  expect(body["sig"].get<std::string>().size() == 64, "sig hex len");

  std::string err;
  expect(auth_verify(body, MsgType::Hello, key, 60000, err), "verify ok");
  expect(err.empty(), "no err");

  expect(!auth_verify(body, MsgType::Hello, other, 60000, err), "wrong key fails");
  expect(!err.empty(), "wrong key err");

  auto tampered = body;
  tampered["node_id"] = "evil";
  expect(!auth_verify(tampered, MsgType::Hello, key, 60000, err), "tamper fails");

  nlohmann::json gossip = {
      {"membership", {{"members", nlohmann::json::array()}}},
      {"fs_table", {{"entries", nlohmann::json::array()}}},
  };
  auth_sign(gossip, MsgType::Gossip, key);
  expect(auth_verify(gossip, MsgType::Gossip, key, 60000, err), "gossip verify");

  // Skew reject
  body["ts"] = body["ts"].get<std::int64_t>() - 120000;
  body["sig"] = hmac_sha256_hex(key, auth_canonical(MsgType::Hello, body["ts"], body));
  expect(!auth_verify(body, MsgType::Hello, key, 60000, err), "skew fails");

  return failures;
}
