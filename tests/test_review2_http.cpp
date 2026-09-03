// Regression tests for the 2026-09-02 code review (http slice).
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "bench/http_bench.hpp"
#include "http/http_auth.hpp"
#include "http/http_server.hpp"
#include "http/s3_iam.hpp"
#include "http/s3_server.hpp"
#include "posix/aios_posix.h"
#include "util/auth.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
using tcp = boost::asio::ip::tcp;
using aios::test::DualStoreFixture;

int pid_port(int base) { return base + static_cast<int>(::getpid() % 200); }

struct HttpFixture {
  DualStoreFixture fx;
  int port_num;
  std::string host{"127.0.0.1"};
  std::string port;
  boost::asio::io_context ioc;
  std::unique_ptr<aios::HttpServer> http;
  std::thread th;

  // configure runs after DualStoreFixture set the defaults and before the server binds.
  template <typename Fn>
  HttpFixture(const char* prefix, int base_port, Fn&& configure)
      : fx(prefix, 2, 2, "nvme"), port_num(pid_port(base_port)) {
    port = std::to_string(port_num);
    fx.cfg.http_listen = host + ":" + port;
    configure(fx.cfg);
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership);
    http->start();
    th = std::thread([this] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  HttpFixture(const char* prefix, int base_port)
      : HttpFixture(prefix, base_port, [](aios::Config&) {}) {}

  ~HttpFixture() {
    http.reset();
    ioc.stop();
    if (th.joinable()) th.join();
  }
};

struct HttpResp {
  int status{-1};
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

// Minimal blocking client. When send_body is false the body is announced via
// Content-Length but never written, so the server's pre-body behaviour is visible.
struct RawConn {
  boost::asio::io_context ioc;
  tcp::socket sock{ioc};

  bool connect(const std::string& host, const std::string& port, int timeout_ms = 5000) {
    tcp::resolver resolver(ioc);
    boost::system::error_code ec;
    auto endpoints = resolver.resolve(host, port, ec);
    if (ec) return false;
    for (int attempt = 0; attempt < 50; ++attempt) {
      boost::asio::connect(sock, endpoints, ec);
      if (!ec) break;
      sock = tcp::socket(ioc);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (ec) return false;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return true;
  }

  void send_all(const std::string& s) {
    std::size_t done = 0;
    while (done < s.size()) {
      const auto n = ::send(sock.native_handle(), s.data() + done, s.size() - done, 0);
      if (n <= 0) return;
      done += static_cast<std::size_t>(n);
    }
  }

  void send_request(const std::string& host, const std::string& port, const std::string& method,
                    const std::string& target, std::unordered_map<std::string, std::string> headers,
                    const std::string& body, bool send_body = true,
                    std::size_t content_length_override = 0) {
    const std::size_t cl = content_length_override ? content_length_override : body.size();
    if (cl > 0 || method == "POST" || method == "PUT") headers["content-length"] = std::to_string(cl);
    std::ostringstream req;
    req << method << ' ' << target << " HTTP/1.1\r\n";
    req << "Host: " << host << ':' << port << "\r\n";
    if (!headers.count("connection")) req << "Connection: close\r\n";
    for (const auto& [k, v] : headers) req << k << ": " << v << "\r\n";
    req << "\r\n";
    send_all(req.str());
    if (send_body && !body.empty()) send_all(body);
  }

  // Reads one response. Returns status -1 on EOF/timeout before a status line.
  HttpResp read_response() {
    HttpResp resp;
    std::string buf;
    char c;
    std::size_t hdr_end = std::string::npos;
    while (hdr_end == std::string::npos) {
      const auto n = ::recv(sock.native_handle(), &c, 1, 0);
      if (n <= 0) return resp;
      buf.push_back(c);
      if (buf.size() >= 4 && buf.compare(buf.size() - 4, 4, "\r\n\r\n") == 0) {
        hdr_end = buf.size();
      }
    }
    std::istringstream is(buf);
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
      for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      resp.headers[name] = value;
      if (name == "content-length") content_length = static_cast<std::size_t>(std::stoull(value));
    }
    resp.body.resize(content_length);
    std::size_t got = 0;
    while (got < content_length) {
      const auto n =
          ::recv(sock.native_handle(), resp.body.data() + got, content_length - got, 0);
      if (n <= 0) break;
      got += static_cast<std::size_t>(n);
    }
    resp.body.resize(got);
    return resp;
  }

  // Blocks until the peer closes; returns true if EOF arrived within timeout_ms.
  bool wait_eof(int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[256];
    for (;;) {
      const auto n = ::recv(sock.native_handle(), buf, sizeof(buf), 0);
      if (n == 0) return true;
      if (n < 0) return false;
    }
  }
};

HttpResp http_req(const std::string& host, const std::string& port, const std::string& method,
                  const std::string& target, std::unordered_map<std::string, std::string> headers,
                  const std::string& body, bool send_body = true,
                  std::size_t content_length_override = 0) {
  RawConn c;
  if (!c.connect(host, port)) return {};
  c.send_request(host, port, method, target, std::move(headers), body, send_body,
                 content_length_override);
  return c.read_response();
}

// AIOS-HMAC-SHA256 with the pinned two-header SignedHeaders string. payload_hash
// is what goes into x-aios-content-sha256 and the canonical string; nonce is
// optional; date defaults to now.
void add_hmac(std::unordered_map<std::string, std::string>& headers, const std::string& method,
              const std::string& target, const std::string& cluster_key,
              const std::string& payload_hash = "UNSIGNED-PAYLOAD", const std::string& nonce = {},
              const std::string& date_override = {}) {
  const std::string date = date_override.empty() ? std::to_string(aios::now_ms()) : date_override;
  headers["x-aios-date"] = date;
  headers["x-aios-content-sha256"] = payload_hash;
  if (!nonce.empty()) headers["x-aios-nonce"] = nonce;
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  const auto canon =
      aios::http_canonical(method, target, date, signed_headers, headers, payload_hash);
  const auto sig = aios::http_sign(cluster_key, canon);
  headers["authorization"] = "AIOS-HMAC-SHA256 Credential=test, SignedHeaders=" + signed_headers +
                             ", Signature=" + sig;
}

std::size_t count_upload_temps() {
  std::size_t n = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::temp_directory_path(), ec)) {
    if (e.path().filename().string().rfind("aios-upload-", 0) == 0) ++n;
  }
  return n;
}

std::string sha_hex(const std::string& s) { return aios::sha256_hex(s); }

// ---------------------------------------------------------------------------
// S3 fixture: HTTP server + S3 front-end on a libaios_posix volume.
// ---------------------------------------------------------------------------

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

// SigV4 over host;x-amz-content-sha256;x-amz-date. canonical_query is the
// already-encoded, sorted canonical query string ("" for none).
void sign_s3(const std::string& method, const std::string& uri, const std::string& canonical_query,
             const std::string& hostport, const std::string& payload_hash,
             const std::string& access, const std::string& secret,
             std::unordered_map<std::string, std::string>& headers) {
  using namespace aios;
  const std::string region = "us-east-1";
  const auto amz = amz_now();
  const auto ds = amz.substr(0, 8);
  headers["x-amz-date"] = amz;
  headers["x-amz-content-sha256"] = payload_hash;
  headers["host"] = hostport;
  const std::string canon_headers = "host:" + hostport + "\n" +
                                    "x-amz-content-sha256:" + payload_hash + "\n" +
                                    "x-amz-date:" + amz + "\n";
  const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
  std::ostringstream canon;
  canon << method << '\n' << uri << '\n' << canonical_query << '\n' << canon_headers
        << signed_headers << '\n' << payload_hash;
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

struct S3Fixture {
  DualStoreFixture fx;
  std::string http_port;
  std::string s3_port;
  std::string s3_hostport;
  std::shared_ptr<aios::S3IamStore> iam;
  boost::asio::io_context ioc;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
  std::thread th;
  std::unique_ptr<aios::HttpServer> http;
  std::unique_ptr<aios::S3Server> s3;

  template <typename Fn>
  S3Fixture(const char* prefix, int http_base, int s3_base, Fn&& configure)
      : fx(prefix, 2, 2, "nvme"), work(boost::asio::make_work_guard(ioc)) {
    http_port = std::to_string(pid_port(http_base));
    s3_port = std::to_string(pid_port(s3_base));
    s3_hostport = "127.0.0.1:" + s3_port;
    fx.cfg.admin = true;
    fx.cfg.http_listen = "127.0.0.1:" + http_port;
    fx.cfg.s3_listen = s3_hostport;
    fx.cfg.s3_volume = "s3";
    fx.cfg.s3_access_key = "aios";
    configure(fx.cfg);
    iam = std::make_shared<aios::S3IamStore>(fx.cfg, *fx.svc);
    th = std::thread([&] { ioc.run(); });
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership, iam);
    http->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    s3 = std::make_unique<aios::S3Server>(ioc, fx.cfg, "127.0.0.1:" + http_port, iam);
    s3->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  S3Fixture(const char* prefix, int http_base, int s3_base)
      : S3Fixture(prefix, http_base, s3_base, [](aios::Config&) {}) {}

  ~S3Fixture() {
    s3->stop();
    s3.reset();
    http.reset();
    work.reset();
    ioc.stop();
    if (th.joinable()) th.join();
  }

  const std::string& key() const { return fx.cfg.cluster_key; }

  HttpResp s3_req(const std::string& method, const std::string& path,
                  const std::string& canonical_query, const std::string& body,
                  const std::string& access, const std::string& secret,
                  const std::string& payload_hash_override = {},
                  std::unordered_map<std::string, std::string> extra = {}) {
    std::unordered_map<std::string, std::string> h = std::move(extra);
    const auto payload = payload_hash_override.empty() ? sha_hex(body) : payload_hash_override;
    sign_s3(method, path, canonical_query, s3_hostport, payload, access, secret, h);
    const std::string target = canonical_query.empty() ? path : path + "?" + canonical_query;
    return http_req("127.0.0.1", s3_port, method, target, h, body);
  }

  std::string create_iam(const std::string& id, uint32_t uid, const std::string& bucket) {
    std::unordered_map<std::string, std::string> h;
    add_hmac(h, "POST", "/admin/api/s3/credentials", key());
    h["content-type"] = "application/json";
    nlohmann::json req{{"access_key_id", id}, {"uid", uid}, {"gid", 100},
                       {"buckets", nlohmann::json::array({bucket})}};
    auto r = http_req("127.0.0.1", http_port, "POST", "/admin/api/s3/credentials", h, req.dump());
    if (r.status != 201 && r.status != 200) return {};
    try {
      return nlohmann::json::parse(r.body).value("secret", "");
    } catch (...) {
      return {};
    }
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// HTTP-1: bodies on non-object endpoints never reach disk; staged uploads are
// removed on every exit path.
// ---------------------------------------------------------------------------

TEST(Review2Http, LargeBodyOnLoginIsRejectedWithoutTempFile) {
  HttpFixture http("aios-r2-h1", 21000, [](aios::Config& c) { c.admin = true; });
  const auto before = count_upload_temps();

  const std::string body(300 * 1024, 'x');
  auto r = http_req(http.host, http.port, "POST", "/admin/login",
                    {{"content-type", "application/json"}}, body);
  EXPECT_GE(r.status, 400);
  EXPECT_LT(r.status, 500);

  // Beyond the non-object ceiling the request is refused before the body is read.
  auto big = http_req(http.host, http.port, "POST", "/admin/login",
                      {{"content-type", "application/json"}}, "", false, 2u * 1024u * 1024u);
  EXPECT_EQ(big.status, 413);

  // Streamed object PUT with a bad signature: 401 before the body is consumed.
  std::unordered_map<std::string, std::string> h;
  add_hmac(h, "PUT", "/o/x", "wrong-key");
  auto put = http_req(http.host, http.port, "PUT", "/o/x", h, "", false, body.size());
  EXPECT_EQ(put.status, 401);

  // Staged PUT that is then rejected by the handler (reserved attr) must clean up.
  std::unordered_map<std::string, std::string> h2{{"x-aios-attr-aios.frozen", "1"}};
  add_hmac(h2, "PUT", "/o/staged", http.fx.cfg.cluster_key);
  auto staged = http_req(http.host, http.port, "PUT", "/o/staged", h2, body);
  EXPECT_EQ(staged.status, 400);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(count_upload_temps(), before);
}

// ---------------------------------------------------------------------------
// HTTP-3: invalid UTF-8 in oids must not throw out of json::dump.
// ---------------------------------------------------------------------------

TEST(Review2Http, InvalidUtf8OidListsAndServerSurvives) {
  HttpFixture http("aios-r2-h3", 21010);
  const std::string key = http.fx.cfg.cluster_key;
  {
    std::unordered_map<std::string, std::string> h;
    add_hmac(h, "PUT", "/o/bad%FF%FEoid", key);
    auto r = http_req(http.host, http.port, "PUT", "/o/bad%FF%FEoid", h, "payload");
    EXPECT_EQ(r.status, 204) << r.body;
  }
  {
    std::unordered_map<std::string, std::string> h;
    add_hmac(h, "GET", "/o?prefix=bad&scope=local", key);
    auto r = http_req(http.host, http.port, "GET", "/o?prefix=bad&scope=local", h, "");
    EXPECT_EQ(r.status, 200) << r.body;
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    ASSERT_EQ(j["objects"].size(), 1u);
    const std::string oid = j["objects"][0]["oid"].get<std::string>();
    EXPECT_NE(oid.find("bad"), std::string::npos);
    EXPECT_NE(oid.find("\xEF\xBF\xBD"), std::string::npos) << "replacement char expected";
  }
  {
    std::unordered_map<std::string, std::string> h;
    add_hmac(h, "GET", "/map", key);
    auto r = http_req(http.host, http.port, "GET", "/map", h, "");
    EXPECT_EQ(r.status, 200) << "server still alive";
  }
}

// ---------------------------------------------------------------------------
// HTTP-7: concurrent long polls are capped.
// ---------------------------------------------------------------------------

TEST(Review2Http, LongPollCapReturns503) {
  HttpFixture http("aios-r2-h7", 21020, [](aios::Config& c) { c.http_max_long_polls = 1; });
  const std::string key = http.fx.cfg.cluster_key;

  RawConn first;
  ASSERT_TRUE(first.connect(http.host, http.port));
  {
    std::unordered_map<std::string, std::string> h;
    add_hmac(h, "GET", "/watch?prefix=lp&timeout_ms=1500", key);
    first.send_request(http.host, http.port, "GET", "/watch?prefix=lp&timeout_ms=1500", h, "");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::unordered_map<std::string, std::string> h;
  add_hmac(h, "GET", "/watch?prefix=lp&timeout_ms=1500", key);
  auto second = http_req(http.host, http.port, "GET", "/watch?prefix=lp&timeout_ms=1500", h, "");
  EXPECT_EQ(second.status, 503) << second.body;

  auto r1 = first.read_response();
  EXPECT_EQ(r1.status, 204) << "first poll times out normally";

  // Slot released: a new long poll is accepted again.
  std::unordered_map<std::string, std::string> h3;
  add_hmac(h3, "GET", "/watch?prefix=lp&timeout_ms=100", key);
  auto third = http_req(http.host, http.port, "GET", "/watch?prefix=lp&timeout_ms=100", h3, "");
  EXPECT_EQ(third.status, 204) << third.body;
}

// ---------------------------------------------------------------------------
// HTTP-4: replay protection.
// ---------------------------------------------------------------------------

TEST(Review2Http, ReplayedSignedDeleteIsRejected) {
  HttpFixture http("aios-r2-h4", 21030);
  const std::string key = http.fx.cfg.cluster_key;
  const std::string empty_sha = sha_hex("");

  std::unordered_map<std::string, std::string> h;
  add_hmac(h, "PUT", "/o/replay", key, sha_hex("v1"));
  EXPECT_EQ(http_req(http.host, http.port, "PUT", "/o/replay", h, "v1").status, 204);

  std::unordered_map<std::string, std::string> del;
  add_hmac(del, "DELETE", "/o/replay", key, empty_sha);
  auto first = http_req(http.host, http.port, "DELETE", "/o/replay", del, "");
  EXPECT_EQ(first.status, 204) << first.body;
  auto again = http_req(http.host, http.port, "DELETE", "/o/replay", del, "");
  EXPECT_EQ(again.status, 401) << "identical signed request replayed: " << again.body;
  EXPECT_NE(again.body.find("replayed"), std::string::npos) << again.body;

  // Two distinct signed requests (different dates) both pass. Offset from the
  // first DELETE's date, not now±5: a 5 ms window can collide with that date
  // and look like a replay (401) instead of a delete of a missing object (404).
  const auto orig = std::stoll(del.at("x-aios-date"));
  const auto d1 = std::to_string(orig - 10000);
  const auto d2 = std::to_string(orig + 10000);
  std::unordered_map<std::string, std::string> a, b;
  add_hmac(a, "DELETE", "/o/replay", key, empty_sha, "", d1);
  add_hmac(b, "DELETE", "/o/replay", key, empty_sha, "", d2);
  EXPECT_EQ(http_req(http.host, http.port, "DELETE", "/o/replay", a, "").status, 404);
  EXPECT_EQ(http_req(http.host, http.port, "DELETE", "/o/replay", b, "").status, 404);

  // Retried GETs without a nonce are not replays.
  std::unordered_map<std::string, std::string> g;
  add_hmac(g, "GET", "/map", key, empty_sha);
  EXPECT_EQ(http_req(http.host, http.port, "GET", "/map", g, "").status, 200);
  EXPECT_EQ(http_req(http.host, http.port, "GET", "/map", g, "").status, 200);
}

TEST(Review2Http, NonceIsSignedAndSingleUse) {
  using namespace aios;
  const std::string key = "550e8400-e29b-41d4-a716-446655440000";
  HttpReplayCache cache;
  std::unordered_map<std::string, std::string> h;
  add_hmac(h, "GET", "/map", key, "UNSIGNED-PAYLOAD", "nonce-1");
  auto ok = http_auth_verify("GET", "/map", h, "UNSIGNED-PAYLOAD", key, 60000, &cache);
  EXPECT_TRUE(ok.ok) << ok.error;
  auto replay = http_auth_verify("GET", "/map", h, "UNSIGNED-PAYLOAD", key, 60000, &cache);
  EXPECT_FALSE(replay.ok);
  EXPECT_EQ(replay.error, "replayed request");

  // The nonce is part of the canonical string: changing it invalidates the signature.
  h["x-aios-nonce"] = "nonce-2";
  auto tampered = http_auth_verify("GET", "/map", h, "UNSIGNED-PAYLOAD", key, 60000, &cache);
  EXPECT_FALSE(tampered.ok);
  EXPECT_EQ(tampered.error, "bad signature");

  // Without a nonce the canonical string is the pre-existing (kernel-pinned) form.
  std::unordered_map<std::string, std::string> plain = {
      {"x-aios-date", "1700000000000"}, {"x-aios-content-sha256", "UNSIGNED-PAYLOAD"}};
  EXPECT_EQ(http_canonical("PUT", "/o/x", "1700000000000", "x-aios-content-sha256;x-aios-date",
                           plain, "UNSIGNED-PAYLOAD"),
            "PUT\n/o/x\n1700000000000\nx-aios-content-sha256;x-aios-date:\n"
            "x-aios-content-sha256;x-aios-date\nUNSIGNED-PAYLOAD");

  // Cache is bounded and time-evicting.
  HttpReplayCache small;
  EXPECT_TRUE(small.check_and_insert("k", 100, 50));
  EXPECT_FALSE(small.check_and_insert("k", 100, 60));
  EXPECT_TRUE(small.check_and_insert("k", 200, 150)) << "expired entry is reusable";
}

// ---------------------------------------------------------------------------
// HTTP-10: cookie sessions need the x-aios-admin header for mutations; login
// attempts are throttled per source address.
// ---------------------------------------------------------------------------

TEST(Review2Http, AdminCookieMutationRequiresHeaderAndLoginIsThrottled) {
  HttpFixture http("aios-r2-h10", 21040, [](aios::Config& c) { c.admin = true; });
  const std::string key = http.fx.cfg.cluster_key;

  auto login = http_req(http.host, http.port, "POST", "/admin/login",
                        {{"content-type", "application/json"}},
                        nlohmann::json{{"cluster_key", key}}.dump());
  ASSERT_EQ(login.status, 200) << login.body;
  const auto set_cookie = login.headers["set-cookie"];
  const auto cookie = set_cookie.substr(0, set_cookie.find(';'));
  ASSERT_FALSE(cookie.empty());

  // GET with cookie only: fine.
  auto get = http_req(http.host, http.port, "GET", "/admin/api/status", {{"cookie", cookie}}, "");
  EXPECT_EQ(get.status, 200);

  const std::string body = nlohmann::json{{"admin_metrics_public", false}}.dump();
  auto no_hdr = http_req(http.host, http.port, "POST", "/admin/api/settings",
                         {{"cookie", cookie}, {"content-type", "application/json"}}, body);
  EXPECT_EQ(no_hdr.status, 403) << no_hdr.body;

  auto with_hdr = http_req(http.host, http.port, "POST", "/admin/api/settings",
                           {{"cookie", cookie},
                            {"content-type", "application/json"},
                            {"x-aios-admin", "1"}},
                           body);
  EXPECT_EQ(with_hdr.status, 200) << with_hdr.body;

  // HMAC-signed mutation is unaffected by the CSRF header rule.
  std::unordered_map<std::string, std::string> h;
  add_hmac(h, "POST", "/admin/api/settings", key, sha_hex(body));
  h["content-type"] = "application/json";
  EXPECT_EQ(http_req(http.host, http.port, "POST", "/admin/api/settings", h, body).status, 200);

  // Login throttle: five failures, then 429 even for the right key.
  for (int i = 0; i < 5; ++i) {
    auto bad = http_req(http.host, http.port, "POST", "/admin/login",
                        {{"content-type", "application/json"}},
                        nlohmann::json{{"cluster_key", "nope"}}.dump());
    EXPECT_EQ(bad.status, 401);
  }
  auto throttled = http_req(http.host, http.port, "POST", "/admin/login",
                            {{"content-type", "application/json"}},
                            nlohmann::json{{"cluster_key", key}}.dump());
  EXPECT_EQ(throttled.status, 429) << throttled.body;
  EXPECT_FALSE(throttled.headers["retry-after"].empty());
}

// ---------------------------------------------------------------------------
// HTTP-12: '+' in a path segment is a literal byte.
// ---------------------------------------------------------------------------

TEST(Review2Http, PlusInPathIsLiteral) {
  HttpFixture http("aios-r2-h12", 21050);
  const std::string key = http.fx.cfg.cluster_key;
  std::unordered_map<std::string, std::string> h;
  add_hmac(h, "PUT", "/o/a+b", key);
  EXPECT_EQ(http_req(http.host, http.port, "PUT", "/o/a+b", h, "plus").status, 204);

  std::unordered_map<std::string, std::string> g;
  add_hmac(g, "GET", "/o/a%2Bb", key);
  auto r = http_req(http.host, http.port, "GET", "/o/a%2Bb", g, "");
  EXPECT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(r.body, "plus");

  std::unordered_map<std::string, std::string> sp;
  add_hmac(sp, "GET", "/o/a%20b", key);
  EXPECT_EQ(http_req(http.host, http.port, "GET", "/o/a%20b", sp, "").status, 404)
      << "a+b must not alias 'a b'";

  // Query strings keep form semantics.
  std::unordered_map<std::string, std::string> l;
  add_hmac(l, "GET", "/o?prefix=a%2B&scope=local", key);
  auto lst = http_req(http.host, http.port, "GET", "/o?prefix=a%2B&scope=local", l, "");
  EXPECT_EQ(lst.status, 200);
  EXPECT_NE(lst.body.find("a+b"), std::string::npos) << lst.body;
}

// ---------------------------------------------------------------------------
// POS-11: streamed bodies are checked against a concrete x-aios-content-sha256;
// UNSIGNED-PAYLOAD can be turned off.
// ---------------------------------------------------------------------------

TEST(Review2Http, StreamedPutVerifiesConcreteContentSha) {
  HttpFixture http("aios-r2-pos11", 21060);
  const std::string key = http.fx.cfg.cluster_key;
  const std::string body(300 * 1024, 'q');
  const auto good = sha_hex(body);
  const auto wrong = sha_hex(body + "x");

  std::unordered_map<std::string, std::string> bad;
  add_hmac(bad, "PUT", "/o/streamed", key, wrong);
  auto r_bad = http_req(http.host, http.port, "PUT", "/o/streamed", bad, body);
  EXPECT_EQ(r_bad.status, 400) << r_bad.body;
  EXPECT_NE(r_bad.body.find("content_sha256_mismatch"), std::string::npos) << r_bad.body;

  std::unordered_map<std::string, std::string> ok;
  add_hmac(ok, "PUT", "/o/streamed", key, good);
  auto r_ok = http_req(http.host, http.port, "PUT", "/o/streamed", ok, body);
  EXPECT_EQ(r_ok.status, 204) << r_ok.body;

  std::unordered_map<std::string, std::string> g;
  add_hmac(g, "GET", "/o/streamed", key);
  auto got = http_req(http.host, http.port, "GET", "/o/streamed", g, "");
  EXPECT_EQ(got.status, 200);
  EXPECT_EQ(got.body.size(), body.size());

  // In-memory body: the signature is computed over the real body hash, so a
  // header that does not match the bytes fails authentication.
  std::unordered_map<std::string, std::string> small_bad;
  add_hmac(small_bad, "PUT", "/o/small", key, sha_hex("other"));
  const int small_status = http_req(http.host, http.port, "PUT", "/o/small", small_bad, "data").status;
  EXPECT_TRUE(small_status == 400 || small_status == 401) << small_status;

  // UNSIGNED-PAYLOAD stays accepted by default.
  std::unordered_map<std::string, std::string> u;
  add_hmac(u, "PUT", "/o/unsigned", key);
  EXPECT_EQ(http_req(http.host, http.port, "PUT", "/o/unsigned", u, body).status, 204);
}

TEST(Review2Http, RequireSignedPayloadRejectsUnsigned) {
  HttpFixture http("aios-r2-pos11b", 21070,
                   [](aios::Config& c) { c.http_require_signed_payload = true; });
  const std::string key = http.fx.cfg.cluster_key;
  const std::string body = "hello";

  std::unordered_map<std::string, std::string> u;
  add_hmac(u, "PUT", "/o/x", key);
  auto r = http_req(http.host, http.port, "PUT", "/o/x", u, body);
  EXPECT_EQ(r.status, 401) << r.body;
  EXPECT_NE(r.body.find("signed_payload_required"), std::string::npos) << r.body;

  std::unordered_map<std::string, std::string> s;
  add_hmac(s, "PUT", "/o/x", key, sha_hex(body));
  EXPECT_EQ(http_req(http.host, http.port, "PUT", "/o/x", s, body).status, 204);

  // Bodiless requests are unaffected.
  std::unordered_map<std::string, std::string> g;
  add_hmac(g, "GET", "/o/x", key);
  EXPECT_EQ(http_req(http.host, http.port, "GET", "/o/x", g, "").status, 200);
}

// ---------------------------------------------------------------------------
// STO-1: daemon-owned attributes and archive bag oids are off limits to clients.
// ---------------------------------------------------------------------------

TEST(Review2Http, ReservedAttrsAndArchiveBagOidsRejected) {
  HttpFixture http("aios-r2-sto1", 21080);
  const std::string key = http.fx.cfg.cluster_key;

  for (const char* attr : {"x-aios-attr-aios.frozen", "x-aios-attr-aios.archive_state",
                           "x-aios-attr-aios.tape_uri", "x-aios-attr-aios.bag_id",
                           "x-aios-attr-aios.bag.compression"}) {
    std::unordered_map<std::string, std::string> h{{attr, "1"}};
    add_hmac(h, "PUT", "/o/attrs", key);
    auto r = http_req(http.host, http.port, "PUT", "/o/attrs", h, "x");
    EXPECT_EQ(r.status, 400) << attr << ": " << r.body;
    EXPECT_NE(r.body.find("reserved_attr"), std::string::npos) << r.body;
  }
  // Ordinary namespaced attrs still work.
  std::unordered_map<std::string, std::string> ok{{"x-aios-attr-aios.posix.cas", "7"},
                                                  {"x-aios-attr-mine", "1"}};
  add_hmac(ok, "PUT", "/o/attrs", key);
  EXPECT_EQ(http_req(http.host, http.port, "PUT", "/o/attrs", ok, "x").status, 204);

  const std::string bag = "/o/archive%2Fbag%2F0001";
  std::unordered_map<std::string, std::string> p;
  add_hmac(p, "PUT", bag, key);
  auto put = http_req(http.host, http.port, "PUT", bag, p, "bag");
  EXPECT_EQ(put.status, 403) << put.body;
  std::unordered_map<std::string, std::string> d;
  add_hmac(d, "DELETE", bag, key);
  EXPECT_EQ(http_req(http.host, http.port, "DELETE", bag, d, "").status, 403);
  std::unordered_map<std::string, std::string> g;
  add_hmac(g, "GET", bag, key);
  EXPECT_EQ(http_req(http.host, http.port, "GET", bag, g, "").status, 404) << "reads still allowed";
}

// ---------------------------------------------------------------------------
// HTTP-6: the admin bench runner only ever targets the local listener.
// ---------------------------------------------------------------------------

TEST(Review2Http, BenchIgnoresCallerSuppliedEndpoint) {
  aios::HttpBenchJob job("127.0.0.1:1", "key");
  EXPECT_EQ(job.defaults()["config"]["endpoint"], "127.0.0.1:1");
  auto r = job.start(nlohmann::json{{"endpoint", "10.255.255.1:80"}, {"ops", 1}, {"warmup", 0}});
  if (r.contains("config")) {
    EXPECT_EQ(r["config"]["endpoint"], "127.0.0.1:1") << r.dump();
  }
  for (int i = 0; i < 200; ++i) {
    if (job.status()["state"] != "running") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  auto st = job.status();
  EXPECT_EQ(st["config"]["endpoint"], "127.0.0.1:1") << st.dump();
  EXPECT_NE(st["state"], "running");
}

// ---------------------------------------------------------------------------
// S3: HTTP-2 / HTTP-5 / HTTP-8 / HTTP-9 / POS-1
// ---------------------------------------------------------------------------

TEST(Review2Http, S3IdleConnectionIsClosedAndBodyIsNotReadBeforeAuth) {
  S3Fixture s3("aios-r2-s3a", 21100, 21200,
               [](aios::Config& c) { c.http_idle_timeout_ms = 700; });

  RawConn idle;
  ASSERT_TRUE(idle.connect("127.0.0.1", s3.s3_port));
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(idle.wait_eof(5000)) << "server must close an idle S3 connection";
  const auto waited = std::chrono::steady_clock::now() - t0;
  EXPECT_LT(waited, std::chrono::milliseconds(4000));

  // Unauthenticated PUT announcing a large body: rejected before any body byte.
  auto r = http_req("127.0.0.1", s3.s3_port, "PUT", "/photos/huge",
                    {{"x-amz-content-sha256", "UNSIGNED-PAYLOAD"}}, "", false,
                    10u * 1024u * 1024u);
  EXPECT_EQ(r.status, 403) << r.body;

  // Larger than the in-memory ceiling: EntityTooLarge without reading the body.
  auto huge = http_req("127.0.0.1", s3.s3_port, "PUT", "/photos/huge",
                       {{"x-amz-content-sha256", "UNSIGNED-PAYLOAD"}}, "", false,
                       s3.fx.cfg.s3_max_body_bytes + 1);
  EXPECT_EQ(huge.status, 413) << huge.body;

  // Bad signature is also decided from headers alone.
  std::unordered_map<std::string, std::string> h;
  sign_s3("PUT", "/photos/huge", "", s3.s3_hostport, "UNSIGNED-PAYLOAD", "aios", "wrong", h);
  auto bad = http_req("127.0.0.1", s3.s3_port, "PUT", "/photos/huge", h, "", false,
                      10u * 1024u * 1024u);
  EXPECT_EQ(bad.status, 403) << bad.body;
  EXPECT_NE(bad.body.find("SignatureDoesNotMatch"), std::string::npos) << bad.body;
}

TEST(Review2Http, S3ContentShaMismatchAndStreamingRejected) {
  S3Fixture s3("aios-r2-s3b", 21110, 21210);
  const std::string key = s3.key();
  EXPECT_EQ(s3.s3_req("PUT", "/docs", "", "", "aios", key).status, 200);

  const std::string body = "hello-s3";
  auto wrong = s3.s3_req("PUT", "/docs/a.txt", "", body, "aios", key, sha_hex("not-the-body"));
  EXPECT_EQ(wrong.status, 400) << wrong.body;
  EXPECT_NE(wrong.body.find("XAmzContentSHA256Mismatch"), std::string::npos) << wrong.body;

  auto streaming = s3.s3_req("PUT", "/docs/a.txt", "", body, "aios", key,
                             "STREAMING-AWS4-HMAC-SHA256-PAYLOAD");
  EXPECT_EQ(streaming.status, 501) << streaming.body;

  auto ok = s3.s3_req("PUT", "/docs/a.txt", "", body, "aios", key);
  EXPECT_EQ(ok.status, 200) << ok.body;
  auto unsigned_ok = s3.s3_req("PUT", "/docs/b.txt", "", body, "aios", key, "UNSIGNED-PAYLOAD");
  EXPECT_EQ(unsigned_ok.status, 200) << unsigned_ok.body;

  auto get = s3.s3_req("GET", "/docs/a.txt", "", "", "aios", key);
  EXPECT_EQ(get.status, 200);
  EXPECT_EQ(get.body, body);
}

TEST(Review2Http, S3MultipartIsOwnerScopedAndDuplicateQueryKeysSign) {
  S3Fixture s3("aios-r2-s3c", 21120, 21220);
  const std::string key = s3.key();
  const std::string secret_a = s3.create_iam("alice", 1001, "shared");
  const std::string secret_b = s3.create_iam("bob", 1002, "shared");
  ASSERT_FALSE(secret_a.empty());
  ASSERT_FALSE(secret_b.empty());
  EXPECT_EQ(s3.s3_req("PUT", "/shared", "", "", "alice", secret_a).status, 200);

  auto init = s3.s3_req("POST", "/shared/big.bin", "uploads=", "", "alice", secret_a);
  ASSERT_EQ(init.status, 200) << init.body;
  const auto a = init.body.find("<UploadId>");
  const auto b = init.body.find("</UploadId>");
  ASSERT_NE(a, std::string::npos);
  ASSERT_NE(b, std::string::npos);
  const std::string upload_id = init.body.substr(a + 10, b - a - 10);
  EXPECT_EQ(upload_id.size(), 32u) << "128-bit CSPRNG id";

  const std::string part_query = "partNumber=1&uploadId=" + upload_id;
  auto bob_part = s3.s3_req("PUT", "/shared/big.bin", part_query, "bobdata", "bob", secret_b);
  EXPECT_EQ(bob_part.status, 403) << bob_part.body;
  auto bob_complete =
      s3.s3_req("POST", "/shared/big.bin", "uploadId=" + upload_id, "", "bob", secret_b);
  EXPECT_EQ(bob_complete.status, 403) << bob_complete.body;
  auto bob_abort =
      s3.s3_req("DELETE", "/shared/big.bin", "uploadId=" + upload_id, "", "bob", secret_b);
  EXPECT_EQ(bob_abort.status, 403) << bob_abort.body;

  auto alice_part =
      s3.s3_req("PUT", "/shared/big.bin", part_query, "alicedata", "alice", secret_a);
  EXPECT_EQ(alice_part.status, 200) << alice_part.body;
  auto alice_complete =
      s3.s3_req("POST", "/shared/big.bin", "uploadId=" + upload_id, "", "alice", secret_a);
  EXPECT_EQ(alice_complete.status, 200) << alice_complete.body;
  auto get = s3.s3_req("GET", "/shared/big.bin", "", "", "alice", secret_a);
  EXPECT_EQ(get.status, 200);
  EXPECT_EQ(get.body, "alicedata");

  // HTTP-9: a duplicated query key stays two canonical pairs (stable-sorted).
  auto list = s3.s3_req("GET", "/shared", "list-type=2&prefix=big&prefix=zzz", "", "alice",
                        secret_a);
  EXPECT_EQ(list.status, 200) << list.body;
  auto list_dup_values = s3.s3_req("GET", "/shared", "prefix=a&prefix=b", "", "alice", secret_a);
  EXPECT_EQ(list_dup_values.status, 200) << list_dup_values.body;
}

TEST(Review2Http, S3PutIsVisibleFromAFreshPosixMount) {
  S3Fixture s3("aios-r2-pos1", 21130, 21230);
  const std::string key = s3.key();
  EXPECT_EQ(s3.s3_req("PUT", "/vis", "", "", "aios", key).status, 200);
  const std::string body(70000, 'v');
  auto put = s3.s3_req("PUT", "/vis/obj.bin", "", body, "aios", key);
  ASSERT_EQ(put.status, 200) << put.body;

  aios_posix_config pcfg{};
  const std::string endpoint = "127.0.0.1:" + s3.http_port;
  pcfg.endpoint = endpoint.c_str();
  pcfg.cluster_key = key.c_str();
  pcfg.volume = "s3";
  int err = 0;
  auto* fs2 = aios_posix_mount(&pcfg, &err);
  ASSERT_NE(fs2, nullptr) << "second mount: " << err;
  aios_posix_stat bst{}, fst{};
  ASSERT_EQ(aios_posix_lookup(fs2, 1, "vis", &bst), 0);
  ASSERT_EQ(aios_posix_lookup(fs2, bst.ino, "obj.bin", &fst), 0);
  EXPECT_EQ(fst.size, body.size()) << "size must be durable before S3 answers 200";
  aios_posix_stat st2{};
  EXPECT_EQ(aios_posix_getattr(fs2, fst.ino, &st2), 0);
  EXPECT_EQ(st2.size, body.size());
  aios_posix_unmount(fs2);
}
