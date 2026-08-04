#include "http/s3_auth.hpp"
#include "util/auth.hpp"

#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace {

int failures = 0;
void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL s3_auth: " << msg << "\n";
    ++failures;
  }
}

std::string amz_now() {
  const auto t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

std::string hex_lower(const std::string& raw) {
  static const char* hexd = "0123456789abcdef";
  std::string out(raw.size() * 2, '\0');
  for (std::size_t i = 0; i < raw.size(); ++i) {
    auto c = static_cast<unsigned char>(raw[i]);
    out[i * 2] = hexd[c >> 4];
    out[i * 2 + 1] = hexd[c & 0xf];
  }
  return out;
}

void sign_request(const std::string& method, const std::string& uri, const std::string& query,
                  const std::string& amz, const std::string& payload_hash,
                  const std::string& access, const std::string& secret, const std::string& region,
                  std::unordered_map<std::string, std::string>& headers) {
  using namespace aios;
  const auto ds = amz.substr(0, 8);
  headers["x-amz-date"] = amz;
  headers["x-amz-content-sha256"] = payload_hash;
  headers["host"] = "127.0.0.1:7481";

  const std::string canon_headers = "host:127.0.0.1:7481\n"
                                    "x-amz-content-sha256:" +
                                    payload_hash + "\n"
                                    "x-amz-date:" + amz + "\n";
  const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
  std::ostringstream canon;
  canon << method << '\n' << uri << '\n' << query << '\n' << canon_headers << signed_headers
        << '\n' << payload_hash;
  const auto canon_hash = sha256_hex(canon.str());
  std::ostringstream sts;
  sts << "AWS4-HMAC-SHA256\n" << amz << '\n' << ds << '/' << region << "/s3/aws4_request\n"
      << canon_hash;
  auto k_date = hmac_sha256_raw("AWS4" + secret, ds);
  auto k_region = hmac_sha256_raw(k_date, region);
  auto k_service = hmac_sha256_raw(k_region, "s3");
  auto k_signing = hmac_sha256_raw(k_service, "aws4_request");
  auto sig = hex_lower(hmac_sha256_raw(k_signing, sts.str()));
  headers["authorization"] =
      "AWS4-HMAC-SHA256 Credential=" + access + "/" + ds + "/" + region +
      "/s3/aws4_request, SignedHeaders=" + signed_headers + ", Signature=" + sig;
}

}  // namespace

int test_s3_auth() {
  using namespace aios;
  failures = 0;

  expect(s3_uri_encode("a/b", false) == "a/b", "uri encode keep slash");
  expect(s3_uri_encode("a b", true) == "a%20b", "uri encode space");

  const std::string access = "aios";
  const std::string secret = "test-cluster-key";
  const std::string region = "us-east-1";
  const auto amz = amz_now();
  const auto payload = sha256_hex("");
  std::unordered_map<std::string, std::string> headers;
  sign_request("GET", "/", "", amz, payload, access, secret, region, headers);

  auto ok = s3_sigv4_verify("GET", "/", "", headers, payload, access, secret, 60000);
  expect(ok.ok, "sigv4 verify ok");
  expect(ok.access_key == access, "access key");
  expect(ok.region == region, "region");

  auto bad = s3_sigv4_verify("GET", "/", "", headers, payload, access, "wrong", 60000);
  expect(!bad.ok, "wrong secret rejected");

  auto bad_ak = s3_sigv4_verify("GET", "/", "", headers, payload, "other", secret, 60000);
  expect(!bad_ak.ok, "wrong access key rejected");

  if (failures == 0) std::cout << "test_s3_auth OK\n";
  return failures;
}
