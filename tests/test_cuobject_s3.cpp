#include <gtest/gtest.h>
#include "cuobject/cuobject_endpoint.hpp"
#include "cuobject/cuobject_s3_xfer.hpp"
#include "http/http_server.hpp"
#include "http/s3_server.hpp"
#include "nixl/aios_nixl.hpp"
#include "test_helpers.hpp"
#include "util/auth.hpp"

#include <boost/asio.hpp>

#include <unistd.h>

#include <chrono>
#include <ctime>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

TEST(CuObjectS3, Basic) {
  using namespace aios;
  // Null / factory
  {
    auto null_ep = make_null_cuobject_endpoint();
    EXPECT_TRUE(null_ep && !null_ep->available()) << "null unavailable";
    std::string reply, err;
    EXPECT_TRUE(!null_ep->rdma_get("k", nullptr, 0, "tok", reply, err)) << "null get fails";
    Config cfg;
    EXPECT_TRUE(!make_cuobject_endpoint(cfg)->available()) << "factory empty listen → null";
  }

  // Stub GET/PUT
  {
    StubCuObjectEndpoint stub;
    std::vector<std::uint8_t> buf(16, 'A');
    std::string reply, err;
    EXPECT_TRUE(stub.rdma_get("b/k", buf.data(), buf.size(), "tok1", reply, err)) << "stub get";
    EXPECT_TRUE(reply == "stub-ok") << "stub get reply";
    EXPECT_TRUE(stub.get_calls() == 1 && stub.last_get_size() == 16) << "stub get meta";

    stub.set_put_payload({'R', 'D', 'M', 'A'});
    std::vector<std::uint8_t> in(4);
    EXPECT_TRUE(stub.rdma_put("b/k", in.data(), in.size(), "tok2", reply, err)) << "stub put";
    EXPECT_TRUE(std::string(in.begin(), in.end()) == "RDMA") << "stub put payload";
  }

  // Helpers mirror S3Server control-plane decisions
  {
    EXPECT_TRUE(s3_want_rdma_get("tok", false, true, 4)) << "want rdma get";
    EXPECT_TRUE(!s3_want_rdma_get("", false, true, 4)) << "no token → no rdma";
    EXPECT_TRUE(!s3_want_rdma_get("tok", true, true, 4)) << "multi-range → no rdma";
    EXPECT_TRUE(!s3_want_rdma_get("tok", false, false, 4)) << "head → no rdma";
    EXPECT_TRUE(!s3_want_rdma_get("tok", false, true, 0)) << "empty → no rdma";

    auto stub = std::make_shared<StubCuObjectEndpoint>(true);
    std::string data = "hello-tcp";
    std::string reply, err;
    EXPECT_TRUE(s3_try_rdma_get(stub.get(), "cuobj/tcp.bin", "client-token-get", data.data(),
                                data.size(), reply, err))
        << "helper rdma get";
    EXPECT_TRUE(reply == "stub-ok") << "helper get reply";
    EXPECT_TRUE(stub->last_get_token() == "client-token-get") << "helper get token";
    EXPECT_TRUE(stub->get_calls() == 1) << "helper get calls";

    // GET failure → caller falls back to TCP (helper returns false)
    stub->set_fail(true);
    EXPECT_TRUE(
        !s3_try_rdma_get(stub.get(), "cuobj/tcp.bin", "tok", data.data(), data.size(), reply, err))
        << "helper get fail";
    stub->set_fail(false);

    stub->set_put_payload({'R', 'D', 'M', 'A'});
    std::vector<std::uint8_t> put_buf(4);
    EXPECT_TRUE(s3_try_rdma_put(stub.get(), "cuobj/rdma.bin", "client-token-put", put_buf.data(),
                                put_buf.size(), reply, err))
        << "helper rdma put";
    EXPECT_TRUE(std::string(put_buf.begin(), put_buf.end()) == "RDMA") << "helper put bytes";
    EXPECT_TRUE(stub->put_calls() == 1) << "helper put calls";

    auto null_ep = make_null_cuobject_endpoint();
    EXPECT_TRUE(!s3_try_rdma_put(null_ep.get(), "cuobj/fail.bin", "tok", put_buf.data(),
                                 put_buf.size(), reply, err))
        << "put rejected when unavailable";
    EXPECT_TRUE(err.find("unavailable") != std::string::npos) << "unavailable message";
  }
}

namespace {

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

// Signer for requests that carry no x-amz-rdma-token: host, content hash, date.
void sign_plain(const std::string& method, const std::string& uri, const std::string& hostport,
                const std::string& amz, const std::string& access, const std::string& secret,
                std::unordered_map<std::string, std::string>& headers) {
  using namespace aios;
  const std::string region = "us-east-1";
  const auto ds = amz.substr(0, 8);
  const std::string payload = sha256_hex("");
  headers["x-amz-date"] = amz;
  headers["x-amz-content-sha256"] = payload;
  headers["host"] = hostport;
  const std::string canon_headers =
      "host:" + hostport + "\nx-amz-content-sha256:" + payload + "\nx-amz-date:" + amz + "\n";
  const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
  const auto canon = method + "\n" + uri + "\n\n" + canon_headers + signed_headers + "\n" + payload;
  const auto canon_hash = sha256_hex(canon);
  const auto sts =
      "AWS4-HMAC-SHA256\n" + amz + "\n" + ds + "/" + region + "/s3/aws4_request\n" + canon_hash;
  auto k_date = hmac_sha256_raw("AWS4" + secret, ds);
  auto k_region = hmac_sha256_raw(k_date, region);
  auto k_service = hmac_sha256_raw(k_region, "s3");
  auto k_signing = hmac_sha256_raw(k_service, "aws4_request");
  headers["authorization"] = "AWS4-HMAC-SHA256 Credential=" + access + "/" + ds + "/" + region +
                             "/s3/aws4_request, SignedHeaders=" + signed_headers +
                             ", Signature=" + hex_lower(hmac_sha256_raw(k_signing, sts));
}

struct HttpResp {
  int status{-1};
  std::string body;
  std::unordered_map<std::string, std::string> headers;
};

HttpResp http_exchange(const std::string& host, const std::string& port, const std::string& method,
                       const std::string& target,
                       std::unordered_map<std::string, std::string> headers,
                       const std::string& body) {
  HttpResp resp;
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::resolver resolver(ioc);
  boost::system::error_code ec;
  auto endpoints = resolver.resolve(host, port, ec);
  if (ec) return resp;
  boost::asio::ip::tcp::socket sock(ioc);
  for (int attempt = 0; attempt < 50; ++attempt) {
    boost::asio::connect(sock, endpoints, ec);
    if (!ec) break;
    sock = boost::asio::ip::tcp::socket(ioc);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (ec) return resp;
  std::ostringstream req;
  req << method << ' ' << target << " HTTP/1.1\r\n";
  req << "Host: " << host << ':' << port << "\r\n";
  req << "Connection: close\r\n";
  for (const auto& [k, v] : headers) {
    if (k == "host" || k == "Host") continue;
    req << k << ": " << v << "\r\n";
  }
  req << "\r\n";
  boost::asio::write(sock, boost::asio::buffer(req.str()), ec);
  if (ec) return resp;
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
  bool have_cl = false;
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto name = line.substr(0, colon);
    auto value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name == "content-length") {
      have_cl = true;
      content_length = static_cast<std::size_t>(std::stoull(value));
    }
    resp.headers.emplace(std::move(name), std::move(value));
  }
  if (!have_cl || content_length == 0) return resp;
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
    if (ec || n == 0) break;
    need -= static_cast<std::size_t>(n);
  }
  return resp;
}

const char* kSizedToken =
    "00007f7c00001000:00000004:00004d16:0000:010203:1:00000000000000000000000000000000";

}  // namespace

TEST(CuObjectS3, NixlTokenLayoutAndPutPlan) {
  using namespace aios;
  const auto tok = parse_rdma_token(kSizedToken);
  EXPECT_TRUE(tok.cuobject && tok.has_addr && tok.has_size);
  EXPECT_EQ(tok.addr, 0x00007f7c00001000ull);
  EXPECT_EQ(tok.size, 4u);
  EXPECT_FALSE(parse_rdma_token("aios-synthetic-rdma-token").cuobject);
  EXPECT_TRUE(parse_rdma_token("0123456789abcdef0:00000004").unsupported);
  EXPECT_TRUE(parse_rdma_token("zz:00000004").malformed);
  EXPECT_TRUE(parse_rdma_token("0000000000000000:00000004").malformed);

  const auto from_token = s3_plan_rdma_put(0, kSizedToken);
  EXPECT_TRUE(from_token.ok);
  EXPECT_EQ(from_token.len, 4u);
  const auto from_length = s3_plan_rdma_put(4, "aios-synthetic-rdma-token");
  EXPECT_TRUE(from_length.ok);
  EXPECT_EQ(from_length.len, 4u);
  const auto unsized = s3_plan_rdma_put(0, "aios-synthetic-rdma-token");
  EXPECT_FALSE(unsized.ok);
  EXPECT_EQ(unsized.status, 501);
  const auto clash = s3_plan_rdma_put(5, kSizedToken);
  EXPECT_FALSE(clash.ok);
  EXPECT_EQ(clash.status, 400);

  EXPECT_EQ(s3_rdma_get_http_status(false, 0, 3, 4), 200);
  EXPECT_EQ(s3_rdma_get_http_status(true, 0, 3, 4), 200);
  EXPECT_EQ(s3_rdma_get_http_status(true, 0, 0, 4), 206);
  EXPECT_FALSE(s3_want_rdma_get("00007f7c00001000:00000002:00:0000:010203:1:00", false, true, 4));
}

TEST(CuObjectS3, ServerSpeaksNixlReply) {
  using namespace aios;
  using aios::test::DualStoreFixture;
  const int http_port_n = 23100 + static_cast<int>(::getpid() % 200);
  const int s3_port_n = 23300 + static_cast<int>(::getpid() % 200);
  const std::string http_port = std::to_string(http_port_n);
  const std::string s3_port = std::to_string(s3_port_n);
  const std::string hostport = "127.0.0.1:" + s3_port;

  DualStoreFixture fx("aios-cuobj-rdma");
  fx.cfg.http_listen = "127.0.0.1:" + http_port;
  fx.cfg.s3_listen = hostport;
  fx.cfg.s3_volume = "s3";
  fx.cfg.s3_access_key = "aios";

  boost::asio::io_context ioc;
  auto work = boost::asio::make_work_guard(ioc);
  std::thread th([&] { ioc.run(); });
  auto http = std::make_unique<HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership);
  http->start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto stub = std::make_shared<StubCuObjectEndpoint>(true);
  stub->set_put_payload({'R', 'D', 'M', 'A'});
  auto s3 = std::make_unique<S3Server>(ioc, fx.cfg, "127.0.0.1:" + http_port, nullptr, stub);
  s3->start();

  NixlConfig cfg;
  cfg.s3_endpoint = hostport;
  cfg.access_key = "aios";
  cfg.secret_key = fx.cfg.cluster_key;
  cfg.region = "us-east-1";

  auto send = [&](S3RdmaRequest req) {
    EXPECT_TRUE(req.error.empty()) << req.error;
    return http_exchange("127.0.0.1", s3_port, req.method, req.target, req.headers, "");
  };

  struct Stop {
    std::unique_ptr<S3Server>& s3;
    std::unique_ptr<HttpServer>& http;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>& work;
    boost::asio::io_context& ioc;
    std::thread& th;
    ~Stop() {
      if (s3) s3->stop();
      s3.reset();
      http.reset();
      work.reset();
      ioc.stop();
      if (th.joinable()) th.join();
    }
  } stop{s3, http, work, ioc, th};

  auto plain = [&](const std::string& method, const std::string& uri) {
    std::unordered_map<std::string, std::string> h;
    sign_plain(method, uri, hostport, amz_now(), "aios", fx.cfg.cluster_key, h);
    return http_exchange("127.0.0.1", s3_port, method, uri, h, "");
  };

  {
    auto created = plain("PUT", "/cuobj");
    ASSERT_EQ(created.status, 200) << created.body;
  }

  {
    auto put = send(build_s3_rdma_request(cfg, true, "cuobj", "obj", 4, "client-token", amz_now()));
    ASSERT_EQ(put.status, 200) << put.body;
    EXPECT_EQ(put.headers["x-amz-rdma-reply"], "200");
    EXPECT_EQ(put.headers["x-amz-rdma-bytes-transferred"], "4");
    EXPECT_FALSE(put.headers["etag"].empty());
    EXPECT_TRUE(put.body.empty());
    EXPECT_EQ(stub->last_put_size(), 4u);
    EXPECT_EQ(stub->put_calls(), 1);
  }

  {
    auto tcp = plain("GET", "/cuobj/obj");
    ASSERT_EQ(tcp.status, 200) << tcp.body;
    EXPECT_EQ(tcp.body, "RDMA");
    EXPECT_TRUE(tcp.headers.count("x-amz-rdma-reply") == 0);
  }

  {
    auto req = build_s3_rdma_request(cfg, false, "cuobj", "obj", 4, "client-token", amz_now());
    req.headers.erase("range");
    auto got = send(std::move(req));
    ASSERT_EQ(got.status, 200) << got.body;
    EXPECT_EQ(got.headers["x-amz-rdma-reply"], "200");
    EXPECT_EQ(got.headers["x-amz-rdma-bytes-transferred"], "4");
    EXPECT_EQ(got.headers["content-length"], "0");
    EXPECT_FALSE(got.headers["etag"].empty());
    EXPECT_TRUE(got.body.empty());
    EXPECT_EQ(stub->last_get_size(), 4u);
  }

  {
    auto req = build_s3_rdma_request(cfg, false, "cuobj", "obj", 1, "client-token", amz_now());
    auto got = send(std::move(req));
    ASSERT_EQ(got.status, 206) << got.body;
    EXPECT_EQ(got.headers["x-amz-rdma-reply"], "206");
    EXPECT_EQ(got.headers["x-amz-rdma-bytes-transferred"], "1");
    EXPECT_EQ(got.headers["content-length"], "0");
    EXPECT_EQ(got.headers["content-range"], "bytes 0-0/4");
    EXPECT_TRUE(got.body.empty());
    EXPECT_EQ(stub->last_get_size(), 1u);
  }

  stub->set_put_payload({});
  {
    auto req = build_s3_rdma_request(cfg, true, "cuobj", "from-token", 4, kSizedToken, amz_now());
    req.headers["content-length"] = "0";
    auto put = send(std::move(req));
    ASSERT_EQ(put.status, 200) << put.body;
    EXPECT_EQ(put.headers["x-amz-rdma-reply"], "200");
    EXPECT_EQ(put.headers["x-amz-rdma-bytes-transferred"], "4");
    EXPECT_EQ(stub->last_put_size(), 4u);
    auto tcp = plain("GET", "/cuobj/from-token");
    ASSERT_EQ(tcp.status, 200) << tcp.body;
    EXPECT_EQ(tcp.body, std::string(4, '\xAB'));
  }

  {
    auto req = build_s3_rdma_request(cfg, true, "cuobj", "nosize", 4, "plain-token", amz_now());
    req.headers["content-length"] = "0";
    auto put = send(std::move(req));
    EXPECT_EQ(put.status, 501) << put.body;
    EXPECT_EQ(put.headers["x-amz-rdma-reply"], "501");
    auto missing = plain("GET", "/cuobj/nosize");
    EXPECT_EQ(missing.status, 404);
  }

  {
    auto req = build_s3_rdma_request(cfg, true, "cuobj", "clash", 5, kSizedToken, amz_now());
    auto put = send(std::move(req));
    EXPECT_EQ(put.status, 400) << put.body;
    EXPECT_EQ(put.headers["x-amz-rdma-reply"], "400");
    EXPECT_EQ(plain("GET", "/cuobj/clash").status, 404);
  }

  {
    auto req = build_s3_rdma_request(cfg, false, "cuobj", "obj", 4, "zz:00000004", amz_now());
    auto got = send(std::move(req));
    EXPECT_EQ(got.status, 400) << got.body;
    EXPECT_EQ(got.headers["x-amz-rdma-reply"], "400");
  }

  s3->set_cuobject_endpoint(make_null_cuobject_endpoint());
  {
    auto put =
        send(build_s3_rdma_request(cfg, true, "cuobj", "noep", 4, "client-token", amz_now()));
    EXPECT_EQ(put.status, 501) << put.body;
    EXPECT_EQ(put.headers["x-amz-rdma-reply"], "501");
    EXPECT_EQ(plain("GET", "/cuobj/noep").status, 404);
  }
}
