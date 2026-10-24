#include "nixl/aios_nixl.hpp"

#include "cuobject/cuobject_endpoint.hpp"
#include "http/s3_auth.hpp"
#include "util/auth.hpp"

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

static_assert(aios::kNixlMaxRdmaBytes == aios::kCuObjectMaxTransferBytes);

class RecordingTransport : public aios::NixlRdmaTransport {
 public:
  struct Call {
    bool read{false};
    std::string key;
    std::string token;
    std::size_t len{0};
  };

  bool read_object(const std::string& key, std::size_t len, const std::string& rdma_token,
                   std::string& err) override {
    return note(true, key, len, rdma_token, err);
  }
  bool write_object(const std::string& key, std::size_t len, const std::string& rdma_token,
                    std::string& err) override {
    return note(false, key, len, rdma_token, err);
  }

  void set_fail(bool v) { fail_ = v; }
  std::vector<Call> calls;

 private:
  bool note(bool read, const std::string& key, std::size_t len, const std::string& token,
            std::string& err) {
    calls.push_back(Call{read, key, token, len});
    if (fail_) {
      err = "injected";
      return false;
    }
    return true;
  }
  bool fail_{false};
};

class GateTransport : public aios::NixlRdmaTransport {
 public:
  bool read_object(const std::string& key, std::size_t len, const std::string& rdma_token,
                   std::string& err) override {
    return block(key, len, rdma_token, err);
  }
  bool write_object(const std::string& key, std::size_t len, const std::string& rdma_token,
                    std::string& err) override {
    return block(key, len, rdma_token, err);
  }

  void wait_until_entered() {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] { return entered_; });
  }

  void open() {
    std::lock_guard lock(mu_);
    go_ = true;
    cv_.notify_all();
  }

 private:
  bool block(const std::string& key, std::size_t len, const std::string& token, std::string&) {
    std::unique_lock lock(mu_);
    (void)key;
    (void)len;
    (void)token;
    entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [&] { return go_; });
    return true;
  }
  std::mutex mu_;
  std::condition_variable cv_;
  bool entered_{false};
  bool go_{false};
};

aios::NixlMemInfo must_reg(aios::NixlEngine& engine, aios::NixlReg reg) {
  aios::NixlMemInfo info;
  std::string err;
  EXPECT_EQ(engine.register_mem(reg, info, err), aios::NixlStatus::Success) << err;
  return info;
}

aios::NixlStatus wait_done(aios::NixlEngine& engine, aios::NixlReq* req) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto st = aios::NixlStatus::InProg;
  while (std::chrono::steady_clock::now() < deadline) {
    st = engine.check(req);
    if (st != aios::NixlStatus::InProg) return st;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return st;
}

std::string server_canon(const std::string& request_target) {
  std::vector<std::string> segs;
  std::string cur;
  for (std::size_t i = 1; i < request_target.size(); ++i) {
    if (request_target[i] == '/') {
      segs.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(request_target[i]);
    }
  }
  segs.push_back(cur);
  std::ostringstream pe;
  pe << '/';
  for (std::size_t i = 0; i < segs.size(); ++i) {
    if (i) pe << '/';
    pe << aios::s3_uri_encode(segs[i], true);
  }
  return pe.str();
}

}  // namespace

TEST(NixlEngine, CuObjectRdmaAndDistinctKeys) {
  auto transport = std::make_shared<RecordingTransport>();
  aios::NixlConfig cfg;
  cfg.rdma_token = "fallback-token";
  aios::NixlEngine engine(cfg, transport);
  EXPECT_TRUE(engine.ok());

  aios::NixlEngine missing({}, nullptr);
  EXPECT_FALSE(missing.ok());

  aios::NixlReg buf;
  buf.mem = aios::NixlMem::Vram;
  buf.addr = 0x1000;
  buf.len = 4;
  buf.dev_id = 1;
  buf.meta = "gpu-token";
  const auto gpu = must_reg(engine, buf);

  aios::NixlReg host = buf;
  host.mem = aios::NixlMem::Dram;
  host.meta.clear();
  const auto dram = must_reg(engine, host);
  EXPECT_EQ(dram.rdma_token, "fallback-token");

  aios::NixlReg bad = buf;
  bad.meta.clear();
  aios::NixlConfig bare;
  aios::NixlEngine no_token(bare, transport);
  aios::NixlMemInfo ignored;
  std::string err;
  EXPECT_EQ(no_token.register_mem(bad, ignored, err), aios::NixlStatus::InvalidParam);

  aios::NixlReg obj;
  obj.mem = aios::NixlMem::Obj;
  obj.dev_id = 7;
  obj.meta = "bucket/one";
  const auto one = must_reg(engine, obj);
  obj.meta = "bucket/two";
  const auto two = must_reg(engine, obj);
  EXPECT_EQ(one.dev_id, two.dev_id);
  EXPECT_NE(one.obj_key, two.obj_key);

  obj.meta = "noslash";
  EXPECT_EQ(engine.register_mem(obj, ignored, err), aios::NixlStatus::InvalidParam);

  buf.len = aios::kNixlMaxRdmaBytes + 1;
  EXPECT_EQ(engine.register_mem(buf, ignored, err), aios::NixlStatus::InvalidParam);

  aios::NixlXferDesc local;
  local.addr = gpu.addr;
  local.len = gpu.len;
  local.dev_id = gpu.dev_id;
  local.info = &gpu;
  aios::NixlXferDesc remote;
  remote.addr = 0;
  remote.len = gpu.len;
  remote.dev_id = two.dev_id;
  remote.info = &two;

  aios::NixlReq* req = nullptr;
  EXPECT_EQ(engine.prep(aios::NixlOp::Read, {local}, {remote}, req, err),
            aios::NixlStatus::Success);
  EXPECT_EQ(engine.check(req), aios::NixlStatus::NotPosted);
  auto posted = engine.post(req);
  EXPECT_TRUE(posted == aios::NixlStatus::Success || posted == aios::NixlStatus::InProg);
  EXPECT_EQ(wait_done(engine, req), aios::NixlStatus::Success);
  ASSERT_EQ(transport->calls.size(), 1u);
  EXPECT_TRUE(transport->calls[0].read);
  EXPECT_EQ(transport->calls[0].key, "bucket/two");
  EXPECT_EQ(transport->calls[0].token, "gpu-token");
  EXPECT_EQ(transport->calls[0].len, 4u);

  EXPECT_EQ(engine.release(req), aios::NixlStatus::Success);
  local.info = &dram;
  local.addr = dram.addr;
  EXPECT_EQ(engine.prep(aios::NixlOp::Write, {local}, {remote}, req, err),
            aios::NixlStatus::Success);
  posted = engine.post(req);
  EXPECT_TRUE(posted == aios::NixlStatus::Success || posted == aios::NixlStatus::InProg);
  EXPECT_EQ(wait_done(engine, req), aios::NixlStatus::Success);
  ASSERT_GE(transport->calls.size(), 2u);
  EXPECT_FALSE(transport->calls.back().read);
  EXPECT_EQ(transport->calls.back().token, "fallback-token");

  remote.addr = 16;
  aios::NixlReq* ranged = nullptr;
  EXPECT_EQ(engine.prep(aios::NixlOp::Read, {local}, {remote}, ranged, err),
            aios::NixlStatus::NotSupported);
  EXPECT_EQ(ranged, nullptr);

  transport->set_fail(true);
  remote.addr = 0;
  aios::NixlReq* failed = nullptr;
  EXPECT_EQ(engine.prep(aios::NixlOp::Read, {local}, {remote}, failed, err),
            aios::NixlStatus::Success);
  EXPECT_EQ(wait_done(engine, failed), aios::NixlStatus::NotPosted);
  posted = engine.post(failed);
  if (posted == aios::NixlStatus::InProg) posted = wait_done(engine, failed);
  EXPECT_EQ(posted, aios::NixlStatus::Backend);
  EXPECT_EQ(engine.last_error(failed), "injected");
  EXPECT_EQ(engine.post(failed), aios::NixlStatus::Backend);

  EXPECT_EQ(engine.release(req), aios::NixlStatus::Success);
  EXPECT_EQ(req, nullptr);
  EXPECT_EQ(engine.release(failed), aios::NixlStatus::Success);
  EXPECT_EQ(failed, nullptr);
}

TEST(NixlEngine, ReleaseWhileInFlight) {
  auto transport = std::make_shared<GateTransport>();
  aios::NixlEngine engine({}, transport);

  aios::NixlReg buf;
  buf.mem = aios::NixlMem::Vram;
  buf.addr = 32;
  buf.len = 8;
  buf.meta = "tok";
  const auto gpu = must_reg(engine, buf);
  aios::NixlReg obj;
  obj.mem = aios::NixlMem::Obj;
  obj.meta = "b/k";
  const auto object = must_reg(engine, obj);

  aios::NixlXferDesc local;
  local.addr = gpu.addr;
  local.len = gpu.len;
  local.info = &gpu;
  aios::NixlXferDesc remote;
  remote.len = gpu.len;
  remote.info = &object;
  aios::NixlReq* req = nullptr;
  std::string err;
  ASSERT_EQ(engine.prep(aios::NixlOp::Read, {local}, {remote}, req, err),
            aios::NixlStatus::Success);
  EXPECT_EQ(engine.post(req), aios::NixlStatus::InProg);
  transport->wait_until_entered();
  EXPECT_EQ(engine.check(req), aios::NixlStatus::InProg);
  EXPECT_EQ(engine.post(req), aios::NixlStatus::RepostActive);
  EXPECT_EQ(engine.release(req), aios::NixlStatus::Backend);
  EXPECT_NE(req, nullptr);
  transport->open();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto st = aios::NixlStatus::Backend;
  while (std::chrono::steady_clock::now() < deadline && st != aios::NixlStatus::Success) {
    st = engine.release(req);
    if (st != aios::NixlStatus::Success) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  EXPECT_EQ(st, aios::NixlStatus::Success);
  EXPECT_EQ(req, nullptr);
}

TEST(NixlS3, SigV4MatchesServerCanonicalUri) {
  aios::NixlConfig cfg;
  cfg.s3_endpoint = "127.0.0.1:7481";
  cfg.access_key = "aios";
  cfg.secret_key = "secret";
  cfg.region = "us-east-1";
  const auto req =
      aios::build_s3_rdma_request(cfg, true, "bucket", "a b/c", 4, "rdma-tok", "20260101T000000Z");
  EXPECT_TRUE(req.error.empty()) << req.error;
  EXPECT_EQ(req.method, "PUT");
  EXPECT_EQ(req.canonical_uri, server_canon(req.target));
  EXPECT_EQ(req.headers.at(aios::kAmzRdmaToken), "rdma-tok");
  EXPECT_EQ(req.headers.at("content-length"), "4");
  EXPECT_EQ(req.payload_hash, "UNSIGNED-PAYLOAD");
  const auto auth = aios::s3_sigv4_verify(req.method, req.canonical_uri, "", req.headers,
                                          req.payload_hash, cfg.access_key, cfg.secret_key, 0);
  EXPECT_TRUE(auth.ok) << auth.error;

  const auto get =
      aios::build_s3_rdma_request(cfg, false, "bucket", "obj", 4, "rdma-tok", "20260101T000000Z");
  EXPECT_EQ(get.payload_hash, aios::sha256_hex(""));
  EXPECT_EQ(get.headers.count("content-length"), 0u);
  EXPECT_EQ(get.headers.at("range"), "bytes=0-3");
  const auto get_auth = aios::s3_sigv4_verify(get.method, get.canonical_uri, "", get.headers,
                                              get.payload_hash, cfg.access_key, cfg.secret_key, 0);
  EXPECT_TRUE(get_auth.ok) << get_auth.error;
}

TEST(NixlS3, LoopbackRequiresRdmaReply) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context ioc;
  tcp::acceptor acc(ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  const auto port = acc.local_endpoint().port();

  std::string seen;
  std::mutex mu;
  std::thread server([&] {
    boost::system::error_code ec;
    tcp::socket sock(ioc);
    acc.accept(sock, ec);
    if (ec) return;
    asio::streambuf buf;
    asio::read_until(sock, buf, "\r\n\r\n", ec);
    std::istream is(&buf);
    std::ostringstream ss;
    ss << is.rdbuf();
    {
      std::lock_guard lock(mu);
      seen = ss.str();
    }
    const char* resp =
        "HTTP/1.1 200 OK\r\n"
        "ETag: \"abc\"\r\n"
        "x-amz-rdma-reply: 200\r\n"
        "x-amz-rdma-bytes-transferred: 4\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    asio::write(sock, asio::buffer(resp, std::strlen(resp)), ec);
  });

  aios::NixlConfig cfg;
  cfg.s3_endpoint = "127.0.0.1:" + std::to_string(port);
  cfg.access_key = "aios";
  cfg.secret_key = "secret";
  cfg.socket_timeout_ms = 2000;
  auto transport = aios::make_s3_cuobject_transport(cfg);
  ASSERT_TRUE(transport);
  std::string err;
  EXPECT_TRUE(transport->read_object("bucket/obj", 4, "client-token", err)) << err;
  server.join();
  std::lock_guard lock(mu);
  EXPECT_NE(seen.find("GET /bucket/obj HTTP/1.1"), std::string::npos);
  EXPECT_NE(seen.find("x-amz-rdma-token: client-token"), std::string::npos);
  EXPECT_NE(seen.find("range: bytes=0-3"), std::string::npos);

  tcp::acceptor acc2(ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  const auto port2 = acc2.local_endpoint().port();
  std::thread server2([&] {
    boost::system::error_code ec;
    tcp::socket sock(ioc);
    acc2.accept(sock, ec);
    if (ec) return;
    asio::streambuf buf;
    asio::read_until(sock, buf, "\r\n\r\n", ec);
    const char* resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    asio::write(sock, asio::buffer(resp, std::strlen(resp)), ec);
  });
  cfg.s3_endpoint = "127.0.0.1:" + std::to_string(port2);
  auto transport2 = aios::make_s3_cuobject_transport(cfg);
  EXPECT_FALSE(transport2->read_object("bucket/obj", 4, "client-token", err));
  EXPECT_NE(err.find("x-amz-rdma-reply"), std::string::npos);
  server2.join();

  tcp::acceptor acc3(ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  const auto port3 = acc3.local_endpoint().port();
  std::thread server3([&] {
    boost::system::error_code ec;
    tcp::socket sock(ioc);
    acc3.accept(sock, ec);
    if (ec) return;
    asio::streambuf buf;
    asio::read_until(sock, buf, "\r\n\r\n", ec);
    const char* resp =
        "HTTP/1.1 200 OK\r\n"
        "ETag: \"abc\"\r\n"
        "x-amz-rdma-reply: ok\r\n"
        "x-amz-rdma-bytes-transferred: 4\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    asio::write(sock, asio::buffer(resp, std::strlen(resp)), ec);
  });
  cfg.s3_endpoint = "127.0.0.1:" + std::to_string(port3);
  auto transport3 = aios::make_s3_cuobject_transport(cfg);
  EXPECT_FALSE(transport3->read_object("bucket/obj", 4, "client-token", err));
  EXPECT_NE(err.find("x-amz-rdma-reply"), std::string::npos);
  server3.join();
}
