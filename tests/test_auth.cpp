#include "util/auth.hpp"
#include <gtest/gtest.h>

#include <iostream>
#include <string>


TEST(Auth, Hmac) {
  using namespace aios;
  const std::string key = "550e8400-e29b-41d4-a716-446655440000";
  const std::string other = "00000000-0000-0000-0000-000000000000";

  nlohmann::json body = {{"node_id", "n1"}, {"listen", "127.0.0.1:7400"}};
  auth_sign(body, MsgType::Hello, key);
  EXPECT_TRUE(body.contains("ts")) << "has ts";
  EXPECT_TRUE(body.contains("sig")) << "has sig";
  EXPECT_TRUE(body["sig"].get<std::string>().size() == 64) << "sig hex len";

  std::string err;
  EXPECT_TRUE(auth_verify(body, MsgType::Hello, key, 60000, err)) << "verify ok";
  EXPECT_TRUE(err.empty()) << "no err";

  EXPECT_TRUE(!auth_verify(body, MsgType::Hello, other, 60000, err)) << "wrong key fails";
  EXPECT_TRUE(!err.empty()) << "wrong key err";

  auto tampered = body;
  tampered["node_id"] = "evil";
  EXPECT_TRUE(!auth_verify(tampered, MsgType::Hello, key, 60000, err)) << "tamper fails";

  nlohmann::json gossip = {
      {"membership", {{"members", nlohmann::json::array()}}},
      {"fs_table", {{"entries", nlohmann::json::array()}}},
  };
  auth_sign(gossip, MsgType::Gossip, key);
  EXPECT_TRUE(auth_verify(gossip, MsgType::Gossip, key, 60000, err)) << "gossip verify";

  // Skew reject
  body["ts"] = body["ts"].get<std::int64_t>() - 120000;
  body["sig"] = hmac_sha256_hex(key, auth_canonical(MsgType::Hello, body["ts"], body));
  EXPECT_TRUE(!auth_verify(body, MsgType::Hello, key, 60000, err)) << "skew fails";

  }
