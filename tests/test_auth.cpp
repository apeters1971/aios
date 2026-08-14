#include "util/auth.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>


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

  const auto hex = hmac_sha256_hex("k", "data");
  EXPECT_EQ(hex.size(), 64u);
  for (char c : hex) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << "lowercase hex";
  }
  }

TEST(Auth, ConcurrentVerifyAndReplay) {
  using namespace aios;
  const std::string key = "550e8400-e29b-41d4-a716-446655440000";
  constexpr int kThreads = 8;
  constexpr int kPerThread = 32;
  std::atomic<int> ok{0};
  std::atomic<int> replay{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        nlohmann::json body = {{"node_id", "n" + std::to_string(t)},
                               {"listen", "127.0.0.1:" + std::to_string(7400 + i)},
                               {"i", i},
                               {"t", t}};
        auth_sign(body, MsgType::Hello, key);
        std::string err;
        if (auth_verify(body, MsgType::Hello, key, 60000, err)) {
          ok.fetch_add(1);
        }
        if (!auth_verify(body, MsgType::Hello, key, 60000, err) && err == "replay") {
          replay.fetch_add(1);
        }
      }
    });
  }
  for (auto& th : ts) th.join();
  EXPECT_EQ(ok.load(), kThreads * kPerThread);
  EXPECT_EQ(replay.load(), kThreads * kPerThread);
}
