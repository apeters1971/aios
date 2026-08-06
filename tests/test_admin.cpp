#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "http/http_auth.hpp"
#include "http/http_server.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

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
                          const std::string& body, const std::string& cluster_key,
                          bool auth = true) {
  HttpResponse resp;
  if (auth) add_auth(headers, method, target, cluster_key);
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
    resp.headers[name] = value;
    if (name == "content-length") {
      content_length = static_cast<std::size_t>(std::stoull(value));
    }
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

TEST(Admin, AdminDisabled404) {
using namespace aios;
  // Admin disabled → 404
  {
    DualStoreFixture fx("aios-admin-off");
    fx.cfg.http_listen = "127.0.0.1:19380";
    boost::asio::io_context ioc;
    HttpServer http(ioc, fx.cfg, *fx.svc, fx.membership);
    http.start();
    std::thread th([&] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto r = http_request("127.0.0.1", "19380", "GET", "/admin/status", {}, "",
                          fx.cfg.cluster_key);
    EXPECT_TRUE(r.status == 404) << "admin disabled → 404";

    ioc.stop();
    th.join();
  }
}

TEST(Admin, AdminEnabledStatusOpsConfigMetricsOpsIncrementOnPUT) {
using namespace aios;
  // Admin enabled: status, ops, config, metrics; ops increment on PUT
  {
    DualStoreFixture fx("aios-admin-on");
    fx.cfg.admin = true;
    fx.cfg.admin_metrics_public = true;
    fx.cfg.http_listen = "127.0.0.1:19381";
    boost::asio::io_context ioc;
    HttpServer http(ioc, fx.cfg, *fx.svc, fx.membership);
    http.start();
    std::thread th([&] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const std::string key = fx.cfg.cluster_key;
    auto put = http_request("127.0.0.1", "19381", "PUT", "/o/admin-obj", {}, "hello", key);
    EXPECT_TRUE(put.status == 204) << "put for ops";

    auto st = http_request("127.0.0.1", "19381", "GET", "/admin/status", {}, "", key);
    EXPECT_TRUE(st.status == 200) << "admin status 200";
    try {
      auto j = nlohmann::json::parse(st.body);
      EXPECT_TRUE(j.value("admin", false)) << "status.admin";
      EXPECT_TRUE(j.contains("ops") && j["ops"].value("put", 0ull) >= 1) << "ops.put >= 1";
    } catch (...) {
      EXPECT_TRUE(false) << "status json";
    }

    auto cfg = http_request("127.0.0.1", "19381", "GET", "/admin/config", {}, "", key);
    EXPECT_TRUE(cfg.status == 200) << "admin config 200";
    try {
      auto j = nlohmann::json::parse(cfg.body);
      EXPECT_TRUE(j.value("cluster_key", "") == "***") << "cluster_key redacted";
    } catch (...) {
      EXPECT_TRUE(false) << "config json";
    }

    auto met = http_request("127.0.0.1", "19381", "GET", "/metrics", {}, "", key,
                            /*auth=*/false);
    EXPECT_TRUE(met.status == 200) << "public metrics 200";
    EXPECT_TRUE(met.body.find("aios_ops_put_total") != std::string::npos) << "prom put metric";
    EXPECT_TRUE(met.body.find("aios_http_requests_total") != std::string::npos) << "prom http metric";

    auto cl = http_request("127.0.0.1", "19381", "GET", "/admin/cluster", {}, "", key);
    EXPECT_TRUE(cl.status == 200) << "admin cluster 200";

    // App label → ops_by_label + prometheus app_label series
    {
      auto bad = http_request("127.0.0.1", "19381", "PUT", "/o/labeled",
                              {{"x-aios-app-label", "bad label!"}}, "x", key);
      EXPECT_TRUE(bad.status == 400) << "invalid app label rejected";

      auto lp = http_request("127.0.0.1", "19381", "PUT", "/o/labeled",
                             {{"x-aios-app-label", "etl/job-1"}}, "payload", key);
      EXPECT_TRUE(lp.status == 204) << "labeled put";

      auto ops = http_request("127.0.0.1", "19381", "GET", "/admin/ops", {}, "", key);
      EXPECT_TRUE(ops.status == 200) << "ops after label";
      try {
        auto j = nlohmann::json::parse(ops.body);
        EXPECT_TRUE(j.contains("ops_by_label") && j["ops_by_label"].contains("etl/job-1")) << "ops_by_label has etl/job-1";
        EXPECT_TRUE(j["ops_by_label"]["etl/job-1"].value("put", 0ull) >= 1) << "label put count";
      } catch (...) {
        EXPECT_TRUE(false) << "ops_by_label json";
      }

      auto met2 = http_request("127.0.0.1", "19381", "GET", "/metrics", {}, "", key, false);
      EXPECT_TRUE(met2.body.find("app_label=\"etl/job-1\"") != std::string::npos) << "prom app_label series";
    }

    // Web UI static + cluster-key session login
    {
      auto idx = http_request("127.0.0.1", "19381", "GET", "/admin/", {}, "", key,
                              /*auth=*/false);
      EXPECT_TRUE(idx.status == 200) << "admin index.html without HMAC";
      EXPECT_TRUE(idx.body.find("AIOS") != std::string::npos) << "admin html has brand";

      auto bad_login =
          http_request("127.0.0.1", "19381", "POST", "/admin/login",
                       {{"content-type", "application/json"}},
                       R"({"cluster_key":"wrong"})", key, /*auth=*/false);
      EXPECT_TRUE(bad_login.status == 401) << "bad cluster key rejected";

      auto login = http_request("127.0.0.1", "19381", "POST", "/admin/login",
                                {{"content-type", "application/json"}},
                                std::string("{\"cluster_key\":\"") + key + "\"}", key,
                                /*auth=*/false);
      EXPECT_TRUE(login.status == 200) << "login ok";
      auto set_cookie = login.headers.count("set-cookie") ? login.headers["set-cookie"] : "";
      EXPECT_TRUE(set_cookie.find("aios_admin=") != std::string::npos) << "session cookie set";
      std::string cookie = set_cookie;
      auto semi = cookie.find(';');
      if (semi != std::string::npos) cookie = cookie.substr(0, semi);

      auto api = http_request("127.0.0.1", "19381", "GET", "/admin/api/status",
                              {{"cookie", cookie}}, "", key, /*auth=*/false);
      EXPECT_TRUE(api.status == 200) << "cookie session status";
      try {
        auto j = nlohmann::json::parse(api.body);
        EXPECT_TRUE(j.value("admin", false)) << "api status.admin";
      } catch (...) {
        EXPECT_TRUE(false) << "api status json";
      }

      auto settings = http_request(
          "127.0.0.1", "19381", "POST", "/admin/api/settings",
          {{"cookie", cookie}, {"content-type", "application/json"}},
          R"({"admin_metrics_public":false})", key, /*auth=*/false);
      EXPECT_TRUE(settings.status == 200) << "settings toggle";
      auto met_priv = http_request("127.0.0.1", "19381", "GET", "/metrics", {}, "", key,
                                   /*auth=*/false);
      EXPECT_TRUE(met_priv.status == 401) << "metrics private after toggle";
      // restore public for cleanliness
      http_request("127.0.0.1", "19381", "POST", "/admin/api/settings",
                   {{"cookie", cookie}, {"content-type", "application/json"}},
                   R"({"admin_metrics_public":true})", key, /*auth=*/false);
    }

    ioc.stop();
    th.join();
  }
}


