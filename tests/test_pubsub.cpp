#include "test_helpers.hpp"

#include "http/http_auth.hpp"
#include "http/http_server.hpp"
#include "object/pubsub.hpp"
#include "util/base64.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using tcp = boost::asio::ip::tcp;
using aios::test::DualStoreFixture;
using aios::test::expect;
using aios::test::failures;

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

int test_pubsub() {
  using namespace aios;
  failures() = 0;

  // Ephemeral: waiting subscriber wakes; no backlog for late after_id
  {
    DualStoreFixture fx("aios-pubsub-eph");
    expect(fx.svc->api_pubsub_create("eph", DeliveryMode::Ephemeral).ok, "eph create");

    std::atomic<bool> got{false};
    std::vector<PubMessage> msgs;
    std::thread waiter([&] {
      auto r = fx.svc->api_pubsub_subscribe("eph", 0, true, 2000);
      if (r.ok && r.code != "timeout" && !r.pub_messages.empty()) {
        msgs = r.pub_messages;
        got = true;
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto* body = reinterpret_cast<const std::uint8_t*>("hello");
    auto pub = fx.svc->api_pubsub_publish("eph", body, 5, "text/plain");
    expect(pub.ok && pub.json_body && (*pub.json_body)["id"] == 1, "eph publish");
    waiter.join();
    expect(got.load() && msgs.size() == 1 && msgs[0].id == 1, "eph waiter woke");
    expect(std::string(msgs[0].data.begin(), msgs[0].data.end()) == "hello", "eph payload");

    // Late subscribe with after_id=0 must not see the missed ephemeral message.
    auto late = fx.svc->api_pubsub_subscribe("eph", 0, true, 200);
    expect(late.ok && late.code == "timeout", "eph no backlog");
  }

  // Buffered: catch-up + ring drop
  {
    DualStoreFixture fx("aios-pubsub-buf");
    expect(fx.svc->api_pubsub_create("buf", DeliveryMode::Buffered, 2).ok, "buf create");
    const auto* a = reinterpret_cast<const std::uint8_t*>("a");
    const auto* b = reinterpret_cast<const std::uint8_t*>("b");
    const auto* c = reinterpret_cast<const std::uint8_t*>("c");
    expect(fx.svc->api_pubsub_publish("buf", a, 1, "").ok, "buf pub a");
    expect(fx.svc->api_pubsub_publish("buf", b, 1, "").ok, "buf pub b");
    expect(fx.svc->api_pubsub_publish("buf", c, 1, "").ok, "buf pub c");

    auto catchup = fx.svc->api_pubsub_subscribe("buf", 0, true, 200);
    expect(catchup.ok && catchup.code != "timeout", "buf catchup ok");
    expect(catchup.pub_messages.size() == 2, "buf ring capacity 2");
    expect(catchup.pub_messages[0].id == 2 && catchup.pub_messages[1].id == 3,
           "buf oldest dropped");

    auto mismatch = fx.svc->api_pubsub_publish("buf", a, 1, "", DeliveryMode::Ephemeral);
    expect(!mismatch.ok && mismatch.code == "mode_mismatch", "buf mode sticky");
  }

  // Durable: object persist + catch-up + mode sticky
  {
    DualStoreFixture fx("aios-pubsub-dur");
    expect(fx.svc->api_pubsub_create("dur", DeliveryMode::Durable).ok, "dur create");
    const auto* body = reinterpret_cast<const std::uint8_t*>("persist-me");
    auto pub = fx.svc->api_pubsub_publish("dur", body, 10, "application/octet-stream");
    expect(pub.ok && pub.json_body, "dur publish");
    const auto id = (*pub.json_body)["id"].get<std::uint64_t>();
    expect(id == 1, "dur id 1");

    auto get = fx.svc->api_get(pubsub_msg_oid("dur", id), std::nullopt, std::nullopt, {});
    expect(get.ok && get.data && get.data->size() == 10, "dur msg object");
    expect(std::string(get.data->begin(), get.data->end()) == "persist-me", "dur msg body");

    auto head = fx.svc->api_head(pubsub_meta_oid("dur"), {});
    expect(head.ok, "dur meta tip");

    auto sub = fx.svc->api_pubsub_subscribe("dur", 0, true, 200);
    expect(sub.ok && sub.pub_messages.size() == 1 && sub.pub_messages[0].id == 1,
           "dur catchup");

    auto mismatch =
        fx.svc->api_pubsub_publish("dur", body, 10, "", DeliveryMode::Buffered);
    expect(!mismatch.ok && mismatch.code == "mode_mismatch", "dur mode sticky");
  }

  // HTTP: create buffered, timeout 204, then publish+subscribe 200
  {
    DualStoreFixture fx("aios-pubsub-http");
    const int port_num = 18900 + static_cast<int>(::getpid() % 300);
    fx.cfg.http_listen = "127.0.0.1:" + std::to_string(port_num);
    const std::string port = std::to_string(port_num);
    const std::string host = "127.0.0.1";
    const std::string& key = fx.cfg.cluster_key;

    boost::asio::io_context ioc;
    HttpServer http(ioc, fx.cfg, *fx.svc, fx.membership);
    http.start();
    std::thread th([&] { ioc.run(); });

    auto create = http_request(host, port, "PUT", "/pubsub/http-t?delivery=buffered&capacity=16",
                               {}, "", key);
    expect(create.status == 201, "HTTP pubsub create 201");

    auto timeout =
        http_request(host, port, "GET", "/pubsub/http-t/subscribe?timeout_ms=200", {}, "", key);
    expect(timeout.status == 204, "HTTP pubsub subscribe timeout 204");

    auto pub =
        http_request(host, port, "POST", "/pubsub/http-t/publish",
                     {{"content-type", "text/plain"}}, "wire", key);
    expect(pub.status == 201, "HTTP pubsub publish 201");

    auto sub = http_request(host, port, "GET", "/pubsub/http-t/subscribe?after_id=0&timeout_ms=1000",
                            {}, "", key);
    expect(sub.status == 200, "HTTP pubsub subscribe 200");
    try {
      auto j = nlohmann::json::parse(sub.body);
      expect(j.contains("messages") && j["messages"].is_array() && !j["messages"].empty(),
             "HTTP messages array");
      const auto& m0 = j["messages"][0];
      std::vector<std::uint8_t> decoded;
      std::string derr;
      expect(base64_decode(m0.value("data_b64", ""), decoded, derr), "HTTP data_b64");
      expect(std::string(decoded.begin(), decoded.end()) == "wire", "HTTP payload");
    } catch (...) {
      expect(false, "HTTP subscribe json");
    }

    auto stat = http_request(host, port, "GET", "/pubsub/http-t", {}, "", key);
    expect(stat.status == 200, "HTTP pubsub stat 200");

    ioc.stop();
    th.join();
  }

  return failures();
}
