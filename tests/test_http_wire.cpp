#include "test_helpers.hpp"

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
#include <unistd.h>
#include <vector>

namespace {

using tcp = boost::asio::ip::tcp;

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
                          bool sign = true) {
  HttpResponse resp;
  if (sign) add_auth(headers, method, target, cluster_key);
  if (!body.empty() && headers.find("content-length") == headers.end()) {
    headers["content-length"] = std::to_string(body.size());
  }

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  boost::system::error_code ec;
  auto endpoints = resolver.resolve(host, port, ec);
  if (ec) {
    resp.status = -1;
    resp.body = "resolve: " + ec.message();
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
    resp.body = "connect: " + ec.message();
    return resp;
  }

  std::ostringstream req;
  req << method << ' ' << target << " HTTP/1.1\r\n";
  req << "Host: " << host << ':' << port << "\r\n";
  req << "Connection: close\r\n";
  for (const auto& [k, v] : headers) {
    req << k << ": " << v << "\r\n";
  }
  req << "\r\n";
  auto head = req.str();
  boost::asio::write(sock, boost::asio::buffer(head), ec);
  if (!ec && !body.empty()) {
    boost::asio::write(sock, boost::asio::buffer(body), ec);
  }
  if (ec) {
    resp.status = -1;
    resp.body = "write: " + ec.message();
    return resp;
  }

  boost::asio::streambuf buf;
  boost::asio::read_until(sock, buf, "\r\n\r\n", ec);
  if (ec && ec != boost::asio::error::eof) {
    resp.status = -1;
    resp.body = "read headers: " + ec.message();
    return resp;
  }

  std::istream is(&buf);
  std::string status_line;
  std::getline(is, status_line);
  if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();
  {
    std::istringstream ss(status_line);
    std::string http_ver, reason;
    ss >> http_ver >> resp.status;
    std::getline(ss, reason);
  }

  std::string line;
  std::size_t content_length = 0;
  bool chunked = false;
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto name = line.substr(0, colon);
    auto value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.erase(value.begin());
    }
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    resp.headers[name] = value;
    if (name == "content-length") {
      try {
        content_length = static_cast<std::size_t>(std::stoull(value));
      } catch (...) {
      }
    }
    if (name == "transfer-encoding" && value.find("chunked") != std::string::npos) {
      chunked = true;
    }
  }

  std::string already(std::istreambuf_iterator<char>(is), {});
  if (content_length > 0) {
    resp.body = already;
    while (resp.body.size() < content_length) {
      char tmp[4096];
      const auto n = sock.read_some(boost::asio::buffer(tmp), ec);
      if (n > 0) resp.body.append(tmp, tmp + n);
      if (ec) break;
    }
    if (resp.body.size() > content_length) resp.body.resize(content_length);
  } else if (!chunked) {
    resp.body = already;
    while (true) {
      char tmp[4096];
      const auto n = sock.read_some(boost::asio::buffer(tmp), ec);
      if (n > 0) resp.body.append(tmp, tmp + n);
      if (ec) break;
    }
  } else {
    resp.body = already;
  }
  return resp;
}

}  // namespace

int test_http_wire() {
  using namespace aios;
  using aios::test::DualStoreFixture;
  using aios::test::expect;
  using aios::test::failures;

  failures() = 0;

  DualStoreFixture fx("aios-http-wire");
  const int port_num = 18000 + static_cast<int>(::getpid() % 1000);
  const std::string port = std::to_string(port_num);
  fx.cfg.http_listen = "127.0.0.1:" + port;

  boost::asio::io_context ioc;
  HttpServer http(ioc, fx.cfg, *fx.svc);
  http.start();
  std::thread th([&] { ioc.run(); });

  const std::string host = "127.0.0.1";
  const std::string key = fx.cfg.cluster_key;

  // PUT /o/wire1 → 204
  {
    auto r = http_request(host, port, "PUT", "/o/wire1", {}, "hello-wire", key);
    expect(r.status == 204, "PUT wire1 204");
  }

  // GET /o/wire1 → 200 body
  {
    auto r = http_request(host, port, "GET", "/o/wire1", {}, "", key);
    expect(r.status == 200, "GET wire1 200");
    expect(r.body == "hello-wire", "GET wire1 body");
  }

  // HEAD /o/wire1 → 200
  {
    auto r = http_request(host, port, "HEAD", "/o/wire1", {}, "", key);
    expect(r.status == 200, "HEAD wire1 200");
  }

  // PUT redirect with x-aios-redirect, empty body → 204
  {
    std::unordered_map<std::string, std::string> h = {{"x-aios-redirect", "wire1"}};
    auto r = http_request(host, port, "PUT", "/o/wire-alias", h, "", key);
    expect(r.status == 204, "PUT redirect 204");
  }

  // GET redirect → 307 + Location
  {
    auto r = http_request(host, port, "GET", "/o/wire-alias", {}, "", key);
    expect(r.status == 307, "GET redirect 307");
    expect(r.headers.count("location") && r.headers["location"].find("wire1") != std::string::npos,
           "Location header");
  }

  // GET /o/wire1/versions → 200 JSON with versions
  {
    auto r = http_request(host, port, "GET", "/o/wire1/versions", {}, "", key);
    expect(r.status == 200, "versions 200");
    try {
      auto j = nlohmann::json::parse(r.body);
      expect(j.contains("versions") && j["versions"].is_array(), "versions array");
      expect(!j["versions"].empty(), "versions non-empty");
    } catch (...) {
      expect(false, "versions json parse");
    }
  }

  // Second put so we have multiple versions for later purge/delete-by-version.
  {
    auto r = http_request(host, port, "PUT", "/o/wire1", {}, "hello-wire-v2", key);
    expect(r.status == 204, "PUT wire1 v2");
  }
  {
    auto r = http_request(host, port, "PUT", "/o/wire1", {}, "hello-wire-v3", key);
    expect(r.status == 204, "PUT wire1 v3");
  }

  // POST /o/wire1/purge?keep=1 → 204
  {
    auto r = http_request(host, port, "POST", "/o/wire1/purge?keep=1", {}, "", key);
    expect(r.status == 204, "purge keep=1 204");
  }

  // Re-seed versions for versioned DELETE (purge above may have trimmed).
  {
    auto r = http_request(host, port, "PUT", "/o/wire1", {}, "after-purge-a", key);
    expect(r.status == 204, "put after purge a");
    r = http_request(host, port, "PUT", "/o/wire1", {}, "after-purge-b", key);
    expect(r.status == 204, "put after purge b");
  }

  std::uint64_t old_seq = 0;
  {
    auto r = http_request(host, port, "GET", "/o/wire1/versions", {}, "", key);
    expect(r.status == 200, "versions for delete");
    try {
      auto j = nlohmann::json::parse(r.body);
      expect(j["versions"].size() >= 2, "need 2 versions for delete-by-version");
      old_seq = j["versions"][1]["seq"].get<std::uint64_t>();
    } catch (...) {
      expect(false, "parse versions for delete");
    }
  }

  // DELETE ?version= for non-tip
  {
    const auto target = "/o/wire1?version=" + std::to_string(old_seq);
    auto r = http_request(host, port, "DELETE", target, {}, "", key);
    expect(r.status == 204, "DELETE non-tip version 204");
  }

  // DELETE /o/wire1 → 204 (delete marker)
  {
    auto r = http_request(host, port, "DELETE", "/o/wire1", {}, "", key);
    expect(r.status == 204, "DELETE tip 204");
  }

  // Recreate for remaining tests
  {
    auto r = http_request(host, port, "PUT", "/o/wire1", {}, "ABCDEFGH", key);
    expect(r.status == 204, "recreate wire1");
  }

  // GET /map → 200
  {
    auto r = http_request(host, port, "GET", "/map", {}, "", key);
    expect(r.status == 200, "GET /map 200");
  }

  // GET /o?prefix= → 200
  {
    auto r = http_request(host, port, "GET", "/o?prefix=", {}, "", key);
    expect(r.status == 200, "GET /o prefix 200");
  }

  // bad auth → 401
  {
    std::unordered_map<std::string, std::string> h = {
        {"x-aios-date", std::to_string(now_ms())},
        {"x-aios-content-sha256", "UNSIGNED-PAYLOAD"},
        {"authorization",
         "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=x-aios-content-sha256;x-aios-date, "
         "Signature=" +
             std::string(64, '0')},
    };
    auto r = http_request(host, port, "GET", "/o/wire1", h, "", key, /*sign=*/false);
    expect(r.status == 401, "bad auth 401");
  }

  // PUT with Content-Range
  {
    std::unordered_map<std::string, std::string> h = {
        {"content-range", "bytes 2-3/*"},
        {"content-length", "2"},
    };
    auto r = http_request(host, port, "PUT", "/o/wire1", h, "xy", key);
    expect(r.status == 204, "PUT Content-Range 204");
  }

  // GET with Range → 206
  {
    std::unordered_map<std::string, std::string> h = {{"range", "bytes=2-3"}};
    auto r = http_request(host, port, "GET", "/o/wire1", h, "", key);
    expect(r.status == 206, "GET Range 206");
    expect(r.body == "xy", "range body");
  }

  // GET ?version=1 (or whatever first version exists — use listed seq)
  {
    auto vr = http_request(host, port, "GET", "/o/wire1/versions", {}, "", key);
    expect(vr.status == 200, "list for version get");
    std::uint64_t v1 = 0;
    try {
      auto j = nlohmann::json::parse(vr.body);
      // oldest is last in newest-first list
      if (!j["versions"].empty()) {
        v1 = j["versions"].back()["seq"].get<std::uint64_t>();
      }
    } catch (...) {
      expect(false, "parse for version get");
    }
    if (v1 > 0) {
      auto r =
          http_request(host, port, "GET", "/o/wire1?version=" + std::to_string(v1), {}, "", key);
      expect(r.status == 200, "GET ?version= 200");
    }
  }

  // If-None-Match: * on existing → 412
  {
    std::unordered_map<std::string, std::string> h = {{"if-none-match", "*"}};
    auto r = http_request(host, port, "PUT", "/o/wire1", h, "nope", key);
    expect(r.status == 412, "If-None-Match * 412");
  }

  ioc.stop();
  th.join();
  return failures();
}
