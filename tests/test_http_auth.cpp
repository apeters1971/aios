#include "http/http_auth.hpp"
#include <gtest/gtest.h>
#include "util/log.hpp"

#include <iostream>
#include <string>
#include <unordered_map>


TEST(HttpAuth, Basic) {
  using namespace aios;
  const std::string key = "550e8400-e29b-41d4-a716-446655440000";
  const std::string date = std::to_string(now_ms());
  std::unordered_map<std::string, std::string> headers = {
      {"x-aios-date", date},
      {"x-aios-content-sha256", "UNSIGNED-PAYLOAD"},
  };
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  const auto canon = http_canonical("PUT", "/o/foo", date, signed_headers, headers,
                                    "UNSIGNED-PAYLOAD");
  const auto sig = http_sign(key, canon);
  headers["authorization"] =
      "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" + signed_headers +
      ", Signature=" + sig;

  auto ok = http_auth_verify("PUT", "/o/foo", headers, "UNSIGNED-PAYLOAD", key, 60000);
  EXPECT_TRUE(ok.ok) << "verify ok";

  headers["authorization"] =
      "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" + signed_headers +
      ", Signature=" + std::string(64, '0');
  auto bad = http_auth_verify("PUT", "/o/foo", headers, "UNSIGNED-PAYLOAD", key, 60000);
  EXPECT_TRUE(!bad.ok) << "bad sig rejected";

  // Wrong method → fail
  headers["authorization"] =
      "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" + signed_headers +
      ", Signature=" + sig;
  auto wrong_method =
      http_auth_verify("GET", "/o/foo", headers, "UNSIGNED-PAYLOAD", key, 60000);
  EXPECT_TRUE(!wrong_method.ok) << "wrong method rejected";

  // Skew: stale date
  {
    std::unordered_map<std::string, std::string> h = {
        {"x-aios-date", std::to_string(now_ms() - 3600'000)},
        {"x-aios-content-sha256", "UNSIGNED-PAYLOAD"},
    };
    const auto c = http_canonical("PUT", "/o/foo", h["x-aios-date"], signed_headers, h,
                                  "UNSIGNED-PAYLOAD");
    h["authorization"] = "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" + signed_headers +
                         ", Signature=" + http_sign(key, c);
    auto skew = http_auth_verify("PUT", "/o/foo", h, "UNSIGNED-PAYLOAD", key, 1000);
    EXPECT_TRUE(!skew.ok) << "skew rejected";
  }

  // header_get helper
  EXPECT_TRUE(header_get({{"Foo", "1"}, {"x-aios-date", "9"}}, "x-aios-date") == "9") << "header_get";
  EXPECT_TRUE(header_get({{"Foo", "1"}}, "missing").empty()) << "header_get missing";

  // Hashed payload path (non UNSIGNED)
  {
    const std::string hash =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";  // sha256("")
    const std::string d = std::to_string(now_ms());
    std::unordered_map<std::string, std::string> h = {
        {"x-aios-date", d},
        {"x-aios-content-sha256", hash},
    };
    const std::string sh = "x-aios-content-sha256;x-aios-date";
    const auto c = http_canonical("GET", "/map", d, sh, h, hash);
    h["authorization"] =
        "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" + sh + ", Signature=" + http_sign(key, c);
    auto ok2 = http_auth_verify("GET", "/map", h, hash, key, 60000);
    EXPECT_TRUE(ok2.ok) << "hashed payload verify";
    auto mismatch = http_auth_verify("GET", "/map", h, "UNSIGNED-PAYLOAD", key, 60000);
    EXPECT_TRUE(!mismatch.ok) << "payload hash mismatch";
  }

  }
