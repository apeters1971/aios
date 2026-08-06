#include "http/http_auth.hpp"
#include <gtest/gtest.h>
#include "http/http_server.hpp"
#include "http/s3_auth.hpp"
#include "http/s3_iam.hpp"
#include "http/s3_server.hpp"
#include "posix/aios_posix.h"
#include "test_helpers.hpp"
#include "util/auth.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <unistd.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace {

using tcp = boost::asio::ip::tcp;
using aios::test::DualStoreFixture;

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

void sign_s3(const std::string& method, const std::string& uri, const std::string& hostport,
             const std::string& amz, const std::string& payload_hash, const std::string& access,
             const std::string& secret, const std::string& region,
             std::unordered_map<std::string, std::string>& headers) {
  using namespace aios;
  const auto ds = amz.substr(0, 8);
  headers["x-amz-date"] = amz;
  headers["x-amz-content-sha256"] = payload_hash;
  headers["host"] = hostport;
  const std::string canon_headers = "host:" + hostport + "\n"
                                    "x-amz-content-sha256:" +
                                    payload_hash + "\n"
                                    "x-amz-date:" + amz + "\n";
  const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
  std::ostringstream canon;
  canon << method << '\n' << uri << '\n' << '\n' << canon_headers << signed_headers << '\n'
        << payload_hash;
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

void add_hmac(std::unordered_map<std::string, std::string>& headers, const std::string& method,
              const std::string& target, const std::string& cluster_key) {
  const std::string date = std::to_string(aios::now_ms());
  headers["x-aios-date"] = date;
  headers["x-aios-content-sha256"] = "UNSIGNED-PAYLOAD";
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  const auto canon =
      aios::http_canonical(method, target, date, signed_headers, headers, "UNSIGNED-PAYLOAD");
  const auto sig = aios::http_sign(cluster_key, canon);
  headers["authorization"] = "AIOS-HMAC-SHA256 Credential=test, SignedHeaders=" + signed_headers +
                             ", Signature=" + sig;
}

struct HttpResp {
  int status{-1};
  std::string body;
};

HttpResp http_req(const std::string& host, const std::string& port, const std::string& method,
                  const std::string& target, std::unordered_map<std::string, std::string> headers,
                  const std::string& body) {
  HttpResp resp;
  if (!body.empty() && headers.find("content-length") == headers.end()) {
    headers["content-length"] = std::to_string(body.size());
  }
  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  boost::system::error_code ec;
  auto endpoints = resolver.resolve(host, port, ec);
  if (ec) return resp;
  tcp::socket sock(ioc);
  for (int attempt = 0; attempt < 50; ++attempt) {
    boost::asio::connect(sock, endpoints, ec);
    if (!ec) break;
    sock = tcp::socket(ioc);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (ec) return resp;
  std::ostringstream req;
  req << method << ' ' << target << " HTTP/1.1\r\n";
  req << "Host: " << host << ':' << port << "\r\n";
  req << "Connection: close\r\n";
  for (const auto& [k, v] : headers) req << k << ": " << v << "\r\n";
  req << "\r\n";
  auto head = req.str();
  boost::asio::write(sock, boost::asio::buffer(head), ec);
  if (!body.empty()) boost::asio::write(sock, boost::asio::buffer(body), ec);
  boost::asio::streambuf buf;
  boost::asio::read_until(sock, buf, "\r\n\r\n", ec);
  std::istream is(&buf);
  std::string status_line;
  std::getline(is, status_line);
  {
    std::istringstream ss(status_line);
    std::string ver;
    ss >> ver >> resp.status;
  }
  std::string line;
  std::size_t content_length = 0;
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto name = line.substr(0, colon);
    auto value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name == "content-length") content_length = static_cast<std::size_t>(std::stoull(value));
  }
  std::size_t have = static_cast<std::size_t>(buf.size());
  if (have > content_length) have = content_length;
  resp.body.assign(have, '\0');
  if (have) is.read(resp.body.data(), static_cast<std::streamsize>(have));
  std::size_t need = content_length - have;
  resp.body.resize(content_length);
  while (need > 0) {
    const auto n = boost::asio::read(
        sock, boost::asio::buffer(resp.body.data() + (content_length - need), need),
        boost::asio::transfer_at_least(1), ec);
    if (ec) break;
    need -= n;
  }
  return resp;
}


}  // namespace

TEST(S3Iam, UnitStoreCreateListFindRemoveViaObjectService) {
using namespace aios;
  // Unit: store create / list / find / remove via ObjectService
  {
    DualStoreFixture fx("aios-s3iam-store");
    fx.cfg.s3_volume = "s3";
    fx.cfg.s3_access_key = "aios";
    S3IamStore store(fx.cfg, *fx.svc);
    std::string err;
    S3Credential in;
    in.access_key_id = "photos-rw";
    in.uid = 1001;
    in.gid = 100;
    in.buckets = {"photos"};
    auto created = store.create(in, err);
    EXPECT_TRUE(created.has_value()) << "create ok";
    EXPECT_TRUE(created && !created->secret.empty()) << "secret generated";
    auto found = store.find("photos-rw");
    EXPECT_TRUE(found.has_value()) << "find ok";
    EXPECT_TRUE(found && found->uid == 1001 && found->gid == 100) << "uid/gid";
    EXPECT_TRUE(found && store.allows_bucket(*found, "photos")) << "allow photos";
    EXPECT_TRUE(found && !store.allows_bucket(*found, "other")) << "deny other";
    auto listed = store.list_redacted();
    EXPECT_TRUE(listed["credentials"].size() == 1) << "list size";
    EXPECT_TRUE(listed["credentials"][0].value("secret", "") == "***") << "secret redacted";

    S3Credential dup = in;
    dup.secret.clear();
    EXPECT_TRUE(!store.create(dup, err)) << "duplicate id rejected";
    S3Credential clash;
    clash.access_key_id = "aios";
    clash.uid = 1;
    clash.gid = 1;
    clash.buckets = {"photos"};
    EXPECT_TRUE(!store.create(clash, err)) << "global s3_access_key conflict rejected";
    S3Credential nobuckets;
    nobuckets.access_key_id = "nobuckets";
    nobuckets.uid = 1;
    nobuckets.gid = 1;
    EXPECT_TRUE(!store.create(nobuckets, err)) << "empty buckets rejected";

    EXPECT_TRUE(store.remove("photos-rw", err)) << "remove ok";
    EXPECT_TRUE(!store.find("photos-rw")) << "gone after remove";
    EXPECT_TRUE(!store.remove("photos-rw", err)) << "remove missing fails";
  }
}

TEST(S3Iam, IntegrationAdminCreateS3SigV4PutWithOwnershipDenyOtherBucket) {
using namespace aios;
  // Integration: admin create → S3 SigV4 put with ownership → deny other bucket
  {
    DualStoreFixture fx("aios-s3iam-e2e");
    fx.cfg.admin = true;
    const std::string http_port = std::to_string(19490 + (getpid() % 200));
    const std::string s3_port = std::to_string(19690 + (getpid() % 200));
    fx.cfg.http_listen = "127.0.0.1:" + http_port;
    fx.cfg.s3_listen = "127.0.0.1:" + s3_port;
    fx.cfg.s3_volume = "s3";
    fx.cfg.s3_access_key = "aios";
    const std::string s3_hostport = "127.0.0.1:" + s3_port;

    auto iam = std::make_shared<S3IamStore>(fx.cfg, *fx.svc);
    boost::asio::io_context ioc;
    auto work = boost::asio::make_work_guard(ioc);
    std::thread th([&] { ioc.run(); });
    HttpServer http(ioc, fx.cfg, *fx.svc, fx.membership, iam);
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // unique_ptr so we can unmount before stopping ioc (rstat flush uses HTTP).
    auto s3 = std::make_unique<S3Server>(ioc, fx.cfg, "127.0.0.1:" + http_port, iam);
    s3->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const std::string key = fx.cfg.cluster_key;
    std::unordered_map<std::string, std::string> h;
    add_hmac(h, "POST", "/admin/api/s3/credentials", key);
    h["content-type"] = "application/json";
    auto created = http_req(
        "127.0.0.1", http_port, "POST", "/admin/api/s3/credentials", h,
        R"({"access_key_id":"photos-rw","uid":1001,"gid":100,"buckets":["photos"]})");
    EXPECT_TRUE(created.status == 201 || created.status == 200) << "admin create cred";
    std::string secret;
    try {
      auto j = nlohmann::json::parse(created.body);
      secret = j.value("secret", "");
    } catch (...) {
    }
    EXPECT_TRUE(!secret.empty()) << "admin returned secret";

    // Root key can create any bucket
    {
      std::unordered_map<std::string, std::string> sh;
      const auto amz = amz_now();
      const auto payload = sha256_hex("");
      sign_s3("PUT", "/other", s3_hostport, amz, payload, "aios", key, "us-east-1", sh);
      auto r = http_req("127.0.0.1", s3_port, "PUT", "/other", sh, "");
      EXPECT_TRUE(r.status == 200) << "root CreateBucket other";
    }

    // Wrong IAM secret rejected
    {
      std::unordered_map<std::string, std::string> sh;
      const auto amz = amz_now();
      const auto payload = sha256_hex("");
      sign_s3("GET", "/", s3_hostport, amz, payload, "photos-rw", "wrong-secret", "us-east-1", sh);
      auto r = http_req("127.0.0.1", s3_port, "GET", "/", sh, "");
      EXPECT_TRUE(r.status == 403) << "wrong iam secret rejected";
    }

    // IAM key denied on other
    {
      std::unordered_map<std::string, std::string> sh;
      const auto amz = amz_now();
      const auto payload = sha256_hex("");
      sign_s3("PUT", "/other/x", s3_hostport, amz, payload, "photos-rw", secret, "us-east-1",
              sh);
      auto r = http_req("127.0.0.1", s3_port, "PUT", "/other/x", sh, "");
      EXPECT_TRUE(r.status == 403) << "iam denied other bucket";
    }

    // IAM CreateBucket + PutObject on photos
    {
      std::unordered_map<std::string, std::string> sh;
      auto amz = amz_now();
      auto payload = sha256_hex("");
      sign_s3("PUT", "/photos", s3_hostport, amz, payload, "photos-rw", secret, "us-east-1", sh);
      auto mb = http_req("127.0.0.1", s3_port, "PUT", "/photos", sh, "");
      EXPECT_TRUE(mb.status == 200) << "iam CreateBucket photos";

      const std::string body = "hello-iam";
      sh.clear();
      amz = amz_now();
      payload = sha256_hex(body);
      sign_s3("PUT", "/photos/a.txt", s3_hostport, amz, payload, "photos-rw", secret, "us-east-1",
              sh);
      auto put = http_req("127.0.0.1", s3_port, "PUT", "/photos/a.txt", sh, body);
      EXPECT_TRUE(put.status == 200) << "iam PutObject";
    }

    // ListBuckets: IAM sees only allowlisted; root sees both
    {
      std::unordered_map<std::string, std::string> sh;
      auto amz = amz_now();
      auto payload = sha256_hex("");
      sign_s3("GET", "/", s3_hostport, amz, payload, "photos-rw", secret, "us-east-1", sh);
      auto iam_ls = http_req("127.0.0.1", s3_port, "GET", "/", sh, "");
      EXPECT_TRUE(iam_ls.status == 200) << "iam ListBuckets";
      EXPECT_TRUE(iam_ls.body.find("<Name>photos</Name>") != std::string::npos) << "iam lists photos";
      EXPECT_TRUE(iam_ls.body.find("<Name>other</Name>") == std::string::npos) << "iam hides other";

      sh.clear();
      amz = amz_now();
      payload = sha256_hex("");
      sign_s3("GET", "/", s3_hostport, amz, payload, "aios", key, "us-east-1", sh);
      auto root_ls = http_req("127.0.0.1", s3_port, "GET", "/", sh, "");
      EXPECT_TRUE(root_ls.status == 200) << "root ListBuckets";
      EXPECT_TRUE(root_ls.body.find("<Name>photos</Name>") != std::string::npos) << "root lists photos";
      EXPECT_TRUE(root_ls.body.find("<Name>other</Name>") != std::string::npos) << "root lists other";
    }

    // Verify POSIX uid/gid via aios_posix
    {
      aios_posix_config pcfg{};
      const std::string endpoint = "127.0.0.1:" + http_port;
      pcfg.endpoint = endpoint.c_str();
      pcfg.cluster_key = key.c_str();
      pcfg.volume = "s3";
      int err = 0;
      auto* fs = aios_posix_mount(&pcfg, &err);
      EXPECT_TRUE(fs != nullptr) << "posix mount";
      if (fs) {
        aios_posix_stat bst{}, fst{};
        EXPECT_TRUE(aios_posix_lookup(fs, 1, "photos", &bst) == 0) << "lookup photos";
        EXPECT_TRUE(bst.uid == 1001 && bst.gid == 100) << "bucket ownership";
        EXPECT_TRUE(aios_posix_lookup(fs, bst.ino, "a.txt", &fst) == 0) << "lookup a.txt";
        EXPECT_TRUE(fst.uid == 1001 && fst.gid == 100) << "file ownership";
        aios_posix_unmount(fs);
      }
    }

    // HMAC list + delete
    {
      std::unordered_map<std::string, std::string> h2;
      add_hmac(h2, "GET", "/admin/api/s3/credentials", key);
      auto lst = http_req("127.0.0.1", http_port, "GET", "/admin/api/s3/credentials", h2, "");
      EXPECT_TRUE(lst.status == 200) << "list credentials";
      try {
        auto j = nlohmann::json::parse(lst.body);
        EXPECT_TRUE(j["credentials"].size() == 1) << "one cred listed";
        EXPECT_TRUE(j["credentials"][0].value("secret", "") == "***") << "listed secret redacted";
      } catch (...) {
        EXPECT_TRUE(false) << "list json";
      }

      std::unordered_map<std::string, std::string> h3;
      add_hmac(h3, "DELETE", "/admin/api/s3/credentials/photos-rw", key);
      auto del = http_req("127.0.0.1", http_port, "DELETE", "/admin/api/s3/credentials/photos-rw",
                          h3, "");
      EXPECT_TRUE(del.status == 200) << "admin delete cred";

      // Deleted key can no longer authenticate
      std::unordered_map<std::string, std::string> sh;
      const auto amz = amz_now();
      const auto payload = sha256_hex("");
      sign_s3("GET", "/", s3_hostport, amz, payload, "photos-rw", secret, "us-east-1", sh);
      auto r = http_req("127.0.0.1", s3_port, "GET", "/", sh, "");
      EXPECT_TRUE(r.status == 403) << "deleted iam key rejected";
    }

    s3->stop();  // close accept + unmount while HTTP still serves rstat flush
    s3.reset();
    work.reset();
    ioc.stop();
    th.join();
  }
}

TEST(S3Iam, ParseAccessKeyHelper) {
using namespace aios;
  // parse access key helper
  {
    std::unordered_map<std::string, std::string> headers;
    headers["authorization"] =
        "AWS4-HMAC-SHA256 Credential=photos-rw/20260101/us-east-1/s3/aws4_request, "
        "SignedHeaders=host, Signature=abc";
    EXPECT_TRUE(aios::s3_sigv4_access_key(headers) == "photos-rw") << "parse akid";
  }
}


