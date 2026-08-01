#include "http/http_auth.hpp"
#include "util/log.hpp"

#include <iostream>
#include <string>
#include <unordered_map>

namespace {

int failures = 0;
void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
}

}  // namespace

int test_http_auth() {
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
  expect(ok.ok, "verify ok");

  headers["authorization"] =
      "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" + signed_headers +
      ", Signature=" + std::string(64, '0');
  auto bad = http_auth_verify("PUT", "/o/foo", headers, "UNSIGNED-PAYLOAD", key, 60000);
  expect(!bad.ok, "bad sig rejected");

  return failures;
}
