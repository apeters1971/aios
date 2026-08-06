#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "http/http_auth.hpp"
#include "http/http_server.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

using tcp = boost::asio::ip::tcp;
using aios::test::DualStoreFixture;
struct HttpResponse {
  int status{0};
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

void add_auth(std::unordered_map<std::string, std::string>& headers, const std::string& method,
              const std::string& target, const std::string& cluster_key) {
  const std::string date = std::to_string(aios::now_ms());
  headers["x-aios-date"] = date;
  headers["x-aios-content-sha256"] = "UNSIGNED-PAYLOAD";
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  const auto canon =
      aios::http_canonical(method, target, date, signed_headers, headers, "UNSIGNED-PAYLOAD");
  const auto sig = aios::http_sign(cluster_key, canon);
  headers["authorization"] = "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" +
                             signed_headers + ", Signature=" + sig;
}

HttpResponse http_request(const std::string& host, const std::string& port,
                          const std::string& method, const std::string& target,
                          std::unordered_map<std::string, std::string> headers,
                          const std::string& body, const std::string& cluster_key) {
  HttpResponse resp;
  add_auth(headers, method, target, cluster_key);
  if (!body.empty() && headers.find("content-length") == headers.end()) {
    headers["content-length"] = std::to_string(body.size());
  }

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  boost::system::error_code ec;
  auto endpoints = resolver.resolve(host, port, ec);
  if (ec) {
    resp.status = -1;
    return resp;
  }
  tcp::socket sock(ioc);
  for (int attempt = 0; attempt < 50; ++attempt) {
    boost::asio::connect(sock, endpoints, ec);
    if (!ec) break;
    sock = tcp::socket(ioc);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (ec) {
    resp.status = -1;
    return resp;
  }

  std::ostringstream req;
  req << method << ' ' << target << " HTTP/1.1\r\n";
  req << "Host: " << host << ':' << port << "\r\n";
  req << "Connection: close\r\n";
  for (const auto& [k, v] : headers) req << k << ": " << v << "\r\n";
  req << "\r\n";
  auto head = req.str();
  boost::asio::write(sock, boost::asio::buffer(head), ec);
  if (!ec && !body.empty()) boost::asio::write(sock, boost::asio::buffer(body), ec);
  if (ec) {
    resp.status = -1;
    return resp;
  }

  boost::asio::streambuf buf;
  boost::asio::read_until(sock, buf, "\r\n\r\n", ec);
  std::istream is(&buf);
  std::string status_line;
  std::getline(is, status_line);
  {
    std::istringstream ss(status_line);
    std::string ver, reason;
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
    resp.headers[name] = value;
    if (name == "content-length") {
      try {
        content_length = static_cast<std::size_t>(std::stoull(value));
      } catch (...) {
      }
    }
  }
  std::string already(std::istreambuf_iterator<char>(is), {});
  resp.body = already;
  while (resp.body.size() < content_length) {
    char tmp[4096];
    const auto n = sock.read_some(boost::asio::buffer(tmp), ec);
    if (n > 0) resp.body.append(tmp, tmp + n);
    if (ec) break;
  }
  if (content_length > 0 && resp.body.size() > content_length) resp.body.resize(content_length);
  return resp;
}


}  // namespace

TEST(LocksWatches, ServiceEnforcedLock) {
using namespace aios;
  // Service: enforced lock
  {
    DualStoreFixture fx("aios-locks");
    const auto* body = reinterpret_cast<const std::uint8_t*>("payload");
    auto acq = fx.svc->api_lock_acquire("lock/a", 5000);
    EXPECT_TRUE(acq.ok && acq.json_body && acq.json_body->contains("token")) << "lock acquire";
    const std::string token = (*acq.json_body)["token"].get<std::string>();

    auto denied = fx.svc->api_put("lock/a", body, 7, {}, true, {});
    EXPECT_TRUE(!denied.ok && denied.code == "lock_held") << "put without token denied";

    auto ok =
        fx.svc->api_put("lock/a", body, 7, {}, true, {}, std::nullopt, {}, token);
    EXPECT_TRUE(ok.ok) << "put with token ok";

    auto bad_renew = fx.svc->api_lock_renew("lock/a", "wrong", 5000);
    EXPECT_TRUE(!bad_renew.ok) << "renew wrong token";
    EXPECT_TRUE(fx.svc->api_lock_renew("lock/a", token, 5000).ok) << "renew ok";

    EXPECT_TRUE(fx.svc->api_lock_stat("lock/a").ok) << "stat held";
    EXPECT_TRUE(fx.svc->api_lock_release("lock/a", token).ok) << "release";
    EXPECT_TRUE(!fx.svc->api_lock_stat("lock/a").ok) << "stat free";
    EXPECT_TRUE(fx.svc->api_put("lock/a", body, 7, {}, true, {}).ok) << "put after unlock";
  }
}

TEST(LocksWatches, ServiceWatchWakesOnPut) {
using namespace aios;
  // Service: watch wakes on put
  {
    DualStoreFixture fx("aios-watch");
    const std::string oid = "watch/obj";
    std::atomic<bool> got{false};
    WatchEvent ev;
    std::thread waiter([&] {
      auto r = fx.svc->api_watch_oid(oid, 0, 2000);
      if (r.ok && r.watch_event) {
        ev = *r.watch_event;
        got = true;
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto* body = reinterpret_cast<const std::uint8_t*>("hi");
    EXPECT_TRUE(fx.svc->api_put(oid, body, 2, {}, true, {}).ok) << "put for watch";
    waiter.join();
    EXPECT_TRUE(got.load() && ev.oid == oid && ev.op == "put") << "watch woke on put";
  }
}

TEST(LocksWatches, HTTPLockWatchTimeout) {
using namespace aios;
  // HTTP: lock + watch timeout
  {
    DualStoreFixture fx("aios-locks-http");
    const int port_num = 18800 + static_cast<int>(::getpid() % 300);
    fx.cfg.http_listen = "127.0.0.1:" + std::to_string(port_num);
    const std::string port = std::to_string(port_num);
    const std::string host = "127.0.0.1";
    const std::string& key = fx.cfg.cluster_key;

    boost::asio::io_context ioc;
    HttpServer http(ioc, fx.cfg, *fx.svc, fx.membership);
    http.start();
    std::thread th([&] { ioc.run(); });

    auto acq = http_request(host, port, "POST", "/o/http-lock/lock",
                            {{"x-aios-lock-ttl-ms", "10000"}}, "", key);
    EXPECT_TRUE(acq.status == 201) << "HTTP lock acquire 201";
    std::string token;
    try {
      token = nlohmann::json::parse(acq.body).value("token", "");
    } catch (...) {
    }
    EXPECT_TRUE(!token.empty()) << "HTTP lock token";

    auto denied =
        http_request(host, port, "PUT", "/o/http-lock", {}, "nope", key);
    EXPECT_TRUE(denied.status == 409) << "HTTP put without token 409";

    auto put_ok = http_request(host, port, "PUT", "/o/http-lock",
                               {{"x-aios-lock-token", token}}, "yes", key);
    EXPECT_TRUE(put_ok.status == 204) << "HTTP put with token 204";

    auto watch = http_request(host, port, "GET", "/o/http-lock/watch?timeout_ms=200", {},
                              "", key);
    EXPECT_TRUE(watch.status == 204) << "HTTP watch timeout 204";

    ioc.stop();
    th.join();
  }
}


