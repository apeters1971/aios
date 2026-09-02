// Session HTTP wire regressions: short bodies, timeouts, size limits, payload hashing.
#include <gtest/gtest.h>

#include "client/error.hpp"
#include "client/session.hpp"
#include "http/http_auth.hpp"
#include "util/auth.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cctype>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using tcp = boost::asio::ip::tcp;

struct StubServer {
  boost::asio::io_context ioc;
  tcp::acceptor acc;
  std::thread th;
  std::string port;
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::atomic<int> accept_count{0};
  std::string last_request;

  explicit StubServer(std::function<std::string(const std::string&)> handler, int accepts = 1,
                      int reqs_per_conn = 1)
      : acc(ioc, tcp::endpoint(tcp::v4(), 0)) {
    port = std::to_string(acc.local_endpoint().port());
    th = std::thread([this, handler = std::move(handler), accepts, reqs_per_conn] {
      ready.store(true);
      for (int i = 0; i < accepts; ++i) {
        boost::system::error_code ec;
        tcp::socket sock(ioc);
        acc.accept(sock, ec);
        if (ec) return;
        accept_count.fetch_add(1);

        for (int r = 0; r < reqs_per_conn; ++r) {
          boost::asio::streambuf buf;
          boost::asio::read_until(sock, buf, "\r\n\r\n", ec);
          if (ec) return;
          std::istream is(&buf);
          std::string req((std::istreambuf_iterator<char>(is)), {});
          last_request = req;
          std::size_t content_length = 0;
          {
            auto lower = req;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const auto p = lower.find("content-length:");
            if (p != std::string::npos) {
              const auto end = lower.find("\r\n", p);
              const auto line = req.substr(p, end - p);
              const auto colon = line.find(':');
              if (colon != std::string::npos) {
                try {
                  content_length = static_cast<std::size_t>(std::stoull(line.substr(colon + 1)));
                } catch (...) {
                }
              }
            }
          }
          const auto hdr_end = req.find("\r\n\r\n");
          std::string body;
          if (hdr_end != std::string::npos) {
            body = req.substr(hdr_end + 4);
            while (body.size() < content_length) {
              char tmp[1024];
              const auto n = sock.read_some(boost::asio::buffer(tmp), ec);
              if (n == 0 || ec) break;
              body.append(tmp, tmp + n);
            }
            last_request = req.substr(0, hdr_end + 4) + body;
          }

          if (stop.load()) return;
          const std::string resp = handler(last_request);
          if (stop.load()) return;
          boost::asio::write(sock, boost::asio::buffer(resp), ec);
          if (ec) return;
        }
        sock.close(ec);
      }
    });
    while (!ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ~StubServer() {
    stop.store(true);
    boost::system::error_code ec;
    acc.close(ec);
    if (th.joinable()) th.join();
  }
};

std::string http_response(int status, const std::string& body,
                          const std::vector<std::pair<std::string, std::string>>& extra = {}) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << status << " OK\r\n";
  oss << "Content-Length: " << body.size() << "\r\n";
  for (const auto& [k, v] : extra) oss << k << ": " << v << "\r\n";
  oss << "Connection: close\r\n\r\n";
  oss << body;
  return oss.str();
}

}  // namespace

TEST(SessionWireC5, ShortResponseBodyIsRejected) {
  using namespace aios;
  StubServer stub([](const std::string&) {
    return "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\nabc";
  });

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 2000;
  Session s(cfg);

  try {
    s.request("GET", "/objects/x");
    FAIL() << "expected short body error";
  } catch (const client_error& e) {
    EXPECT_EQ(e.code(), "http");
    EXPECT_NE(std::string(e.what()).find("short response body"), std::string::npos);
  }
}

TEST(SessionWireC7, OversizeContentLengthIsRejected) {
  using namespace aios;
  StubServer stub([](const std::string&) {
    return "HTTP/1.1 200 OK\r\nContent-Length: 20000000\r\nConnection: close\r\n\r\n";
  });

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 2000;
  Session s(cfg);

  try {
    s.request("GET", "/objects/x");
    FAIL() << "expected payload_too_large";
  } catch (const client_error& e) {
    EXPECT_EQ(e.code(), "payload_too_large");
  }
}

TEST(SessionWireC7b, NegativeContentLengthIsRejected) {
  using namespace aios;
  StubServer stub([](const std::string&) {
    return "HTTP/1.1 200 OK\r\nContent-Length: -1\r\nConnection: close\r\n\r\n";
  });

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 2000;
  Session s(cfg);

  try {
    s.request("GET", "/objects/x");
    FAIL() << "expected invalid Content-Length";
  } catch (const client_error& e) {
    EXPECT_EQ(e.code(), "http");
    EXPECT_NE(std::string(e.what()).find("invalid Content-Length"), std::string::npos);
  }
}

// The stub accepts the request and never answers until released. The assertions
// that matter are on the *outcome* — a `client_error` with code "http" whose
// message names the timeout — not on how long it took. The elapsed-time checks
// are deliberately loose: the lower bound proves the timeout was actually
// applied (a failure before ~one timeout would be a different error path), the
// upper bound only proves the client did not wait for the stub's release, and
// is generous enough to absorb a loaded CI runner.
TEST(SessionWireC6, SocketTimeoutSurfacesAsHttpError) {
  using namespace aios;
  constexpr int kTimeoutMs = 200;
  constexpr int kLowerBoundMs = kTimeoutMs / 2;   // scheduler slack; must not be 0
  constexpr int kUpperBoundMs = 5000;             // << the "forever" the stub would hang

  std::atomic<bool> hang{true};
  StubServer stub([&](const std::string&) {
    while (hang.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return http_response(200, "late");
  });

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = kTimeoutMs;
  Session s(cfg);

  const auto t0 = std::chrono::steady_clock::now();
  bool threw = false;
  std::string code;
  std::string what;
  try {
    s.request("GET", "/objects/x");
  } catch (const client_error& e) {
    threw = true;
    code = e.code();
    what = e.what();
  }
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
          .count();
  hang.store(false);   // release the stub thread before any assertion can abort the test

  ASSERT_TRUE(threw) << "expected client_error on socket timeout";
  EXPECT_EQ(code, "http") << what;
  EXPECT_NE(what.find("timeout"), std::string::npos) << what;
  EXPECT_GE(ms, kLowerBoundMs) << "failed before the socket timeout could have fired: " << what;
  EXPECT_LE(ms, kUpperBoundMs) << "must not wait for the stub to answer";
}

TEST(SessionWireC8, RequestBodyIsContentSha256Hashed) {
  using namespace aios;
  StubServer stub([](const std::string&) { return http_response(200, "ok"); });

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 2000;
  Session s(cfg);

  const std::string body = "signed-payload-bytes";
  s.request("PUT", "/objects/c8", {}, body);

  auto lower = stub.last_request;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  EXPECT_EQ(lower.find("unsigned-payload"), std::string::npos);
  const auto expect = sha256_hex(body);
  EXPECT_NE(stub.last_request.find(expect), std::string::npos)
      << "request missing x-aios-content-sha256=" << expect;
}

// Large bodies used to be sent as UNSIGNED-PAYLOAD; since POS-11 every body is
// covered by the signature via its real SHA-256.
TEST(SessionWireC8, StreamedRequestBodyIsSignedWithRealSha256) {
  using namespace aios;
  StubServer stub([](const std::string&) { return http_response(200, "ok"); });

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 2000;
  Session s(cfg);

  const std::string body(kHttpStreamBodyBytes + 1, 'A');
  s.request("PUT", "/o/1M", {}, body);

  auto lower = stub.last_request;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  EXPECT_EQ(lower.find("unsigned-payload"), std::string::npos)
      << stub.last_request.substr(0, 800);
  const auto expect = sha256_hex(body);
  EXPECT_NE(lower.find("x-aios-content-sha256: " + expect), std::string::npos)
      << stub.last_request.substr(0, 800);
}

TEST(SessionWireC8, AbsoluteRedirectToUnknownHostIsRejected) {
  using namespace aios;
  // Second accept serves the one-shot /admin/cluster refresh (empty peers).
  StubServer stub(
      [](const std::string& req) {
        if (req.find("GET /admin/cluster ") != std::string::npos) {
          return http_response(200, R"({"admin_peers":[]})");
        }
        return http_response(307, R"({"code":"not_primary"})",
                             {{"Location", "http://evil.example:9/o/stolen"}});
      },
      /*accepts=*/2);

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 2000;
  Session s(cfg);

  try {
    s.request("PUT", "/o/x", {}, "payload");
    FAIL() << "expected redirect target rejection";
  } catch (const client_error& e) {
    EXPECT_EQ(e.code(), "http");
    EXPECT_NE(std::string(e.what()).find("redirect target not in cluster"), std::string::npos);
  }
}

TEST(SessionWireC8, AbsoluteRedirectToAllowlistedPeerIsFollowed) {
  using namespace aios;
  StubServer peer([](const std::string&) { return http_response(200, "primary-ok"); });
  const std::string peer_hp = "127.0.0.1:" + peer.port;
  StubServer stub([&](const std::string&) {
    return http_response(307, R"({"code":"not_primary"})",
                         {{"Location", "http://" + peer_hp + "/o/x"}});
  });

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 2000;
  cfg.redirect_peers = {peer_hp};
  Session s(cfg);

  auto resp = s.request("PUT", "/o/x", {}, "payload");
  EXPECT_EQ(resp.status, 200);
  EXPECT_EQ(resp.body, "primary-ok");
}

TEST(SessionWireC8, RelativeRedirectIsFollowed) {
  using namespace aios;
  StubServer stub(
      [](const std::string& req) {
        if (req.find("GET /o/alias ") != std::string::npos) {
          return http_response(307, "", {{"Location", "/o/target"}});
        }
        return http_response(200, "target-body");
      },
      /*accepts=*/2);

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 2000;
  Session s(cfg);

  auto resp = s.request("GET", "/o/alias");
  EXPECT_EQ(resp.status, 200);
  EXPECT_EQ(resp.body, "target-body");
}

TEST(SessionWireC8, AbsoluteRedirectAllowedAfterClusterRefresh) {
  using namespace aios;
  StubServer peer([](const std::string&) { return http_response(200, "via-refresh"); });
  const std::string peer_hp = "127.0.0.1:" + peer.port;

  StubServer stub(
      [&](const std::string& req) {
        if (req.find("GET /admin/cluster ") != std::string::npos) {
          const std::string body =
              std::string(R"({"node_id":"n0","admin_peers":[{"http_addr":")") + peer_hp +
              R"("}]})";
          return http_response(200, body);
        }
        return http_response(307, R"({"code":"not_primary"})",
                             {{"Location", "http://" + peer_hp + "/o/x"}});
      },
      /*accepts=*/2);

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 2000;
  Session s(cfg);

  auto resp = s.request("PUT", "/o/x", {}, "payload");
  EXPECT_EQ(resp.status, 200);
  EXPECT_EQ(resp.body, "via-refresh");
}

TEST(SessionWireKeepAlive, ReusesTcpConnection) {
  using namespace aios;
  std::atomic<int> reqs{0};
  StubServer stub(
      [&](const std::string&) {
        reqs.fetch_add(1);
        return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok";
      },
      /*accepts=*/1, /*reqs_per_conn=*/2);

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 2000;
  Session s(cfg);

  auto a = s.request("GET", "/one");
  auto b = s.request("GET", "/two");
  EXPECT_EQ(a.status, 200);
  EXPECT_EQ(b.status, 200);
  EXPECT_EQ(a.body, "ok");
  EXPECT_EQ(b.body, "ok");
  EXPECT_EQ(stub.accept_count.load(), 1);
  EXPECT_EQ(reqs.load(), 2);
}

TEST(SessionWireKeepAlive, HeadDoesNotWaitForAdvertisedBody) {
  using namespace aios;
  StubServer stub(
      [](const std::string& req) {
        if (req.find("HEAD ") == 0) {
          return "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: keep-alive\r\n\r\n";
        }
        return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok";
      },
      /*accepts=*/1, /*reqs_per_conn=*/2);

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 2000;
  Session s(cfg);

  auto head = s.request("HEAD", "/o/x");
  auto get = s.request("GET", "/o/x");
  EXPECT_EQ(head.status, 200);
  EXPECT_TRUE(head.body.empty());
  EXPECT_EQ(get.status, 200);
  EXPECT_EQ(get.body, "ok");
  EXPECT_EQ(stub.accept_count.load(), 1);
}

TEST(SessionWireKeepAlive, HeadErrorJsonIsDrainedBeforeNextRequest) {
  using namespace aios;
  const std::string err = R"({"code":"nf"})";
  StubServer stub(
      [&](const std::string& req) {
        if (req.find("HEAD ") == 0) {
          return "HTTP/1.1 404 Not Found\r\nContent-Length: " + std::to_string(err.size()) +
                 "\r\nConnection: keep-alive\r\n\r\n" + err;
        }
        return std::string(
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok");
      },
      /*accepts=*/1, /*reqs_per_conn=*/2);

  SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 2000;
  Session s(cfg);

  auto head = s.request("HEAD", "/o/missing");
  auto get = s.request("GET", "/o/x");
  EXPECT_EQ(head.status, 404);
  EXPECT_TRUE(head.body.empty());
  EXPECT_EQ(get.status, 200);
  EXPECT_EQ(get.body, "ok");
  EXPECT_EQ(stub.accept_count.load(), 1);
}
