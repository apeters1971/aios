#include "test_helpers.hpp"

#include "cluster/place.hpp"
#include "ec/codec_factory.hpp"
#include "ec/ec_attrs.hpp"
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
#include <vector>

namespace {

using tcp = boost::asio::ip::tcp;
using aios::test::expect;
using aios::test::failures;
using aios::test::temp_root;

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
    std::getline(ss, reason);
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

struct EcHttpFixture {
  std::filesystem::path root;
  std::string p1, p2, p3;
  aios::MembershipTable membership;
  aios::FsTable fs_table;
  aios::Config cfg;
  aios::ClusterMap map;
  aios::LocalStores stores;
  std::unique_ptr<aios::ObjectService> svc;
  std::string port;

  explicit EcHttpFixture(const std::string& ec_codec = "xor", int port_offset = 0) {
    using namespace aios;
    root = temp_root("aios-http-ec");
    for (auto name : {"t1", "t2", "t3"}) {
      std::filesystem::create_directories(root / name / "aios");
    }
    p1 = (root / "t1" / "aios").string();
    p2 = (root / "t2" / "aios").string();
    p3 = (root / "t3" / "aios").string();

    membership.set_local("node-a", "127.0.0.1:7400");
    std::vector<AiosTarget> local;
    for (const auto& path : {p1, p2, p3}) {
      AiosTarget t;
      t.mount = path;
      t.target_path = path;
      t.aios_path = path;
      t.usable = true;
      t.bavail = 1000;
      local.push_back(t);
    }
    fs_table.set_local("node-a", local);

    cfg.node_id = "node-a";
    cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
    cfg.durability = "ec";
    cfg.ec_k = 2;
    cfg.ec_m = 1;
    cfg.ec_codec = ec_codec;
    std::string err;
    expect(normalize_config(cfg, err), "normalize http ec");
    cfg.clone_required = false;
    cfg.max_versions = 16;
    const int port_num = 18500 + static_cast<int>(::getpid() % 500) * 2 + port_offset;
    port = std::to_string(port_num);
    cfg.http_listen = "127.0.0.1:" + port;

    map = ClusterMap::build(membership, fs_table, cfg.replica_count);
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    stores.sync_paths({p1, p2, p3}, opts);
    svc = std::make_unique<ObjectService>(cfg, map, stores);
    svc->set_advertise("127.0.0.1:7400");
  }

  ~EcHttpFixture() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

}  // namespace

int test_http_ec() {
  using namespace aios;
  failures() = 0;

  EcHttpFixture fx("xor");
  boost::asio::io_context ioc;
  HttpServer http(ioc, fx.cfg, *fx.svc);
  http.start();
  std::thread th([&] { ioc.run(); });

  const std::string host = "127.0.0.1";
  const std::string& port = fx.port;
  const std::string& key = fx.cfg.cluster_key;
  const std::string body = "http-ec-payload!";

  // PUT via HTTP → EC stripe
  {
    auto r = http_request(host, port, "PUT", "/o/http-ec-1", {}, body, key);
    expect(r.status == 204, "HTTP EC PUT 204");
    expect(r.headers.count("x-aios-version"), "PUT version header");
    expect(r.headers.count("x-aios-crc32c"), "PUT full crc header");
  }

  // GET returns full reconstructed body + size/attr headers
  {
    auto r = http_request(host, port, "GET", "/o/http-ec-1", {}, "", key);
    expect(r.status == 200, "HTTP EC GET 200");
    expect(r.body == body, "HTTP EC GET body");
    expect(r.headers.count("x-aios-size") && r.headers["x-aios-size"] == std::to_string(body.size()),
           "HTTP EC x-aios-size full");
    expect(r.headers.count("x-aios-attr-aios.ec.k"), "HTTP EC attr k");
    expect(r.headers.count("x-aios-attr-aios.ec.codec") &&
               r.headers["x-aios-attr-aios.ec.codec"] == "xor",
           "HTTP EC attr codec");
  }

  // HEAD
  {
    auto r = http_request(host, port, "HEAD", "/o/http-ec-1", {}, "", key);
    expect(r.status == 200, "HTTP EC HEAD 200");
    expect(r.headers.count("content-length") &&
               r.headers["content-length"] == std::to_string(body.size()),
           "HTTP EC HEAD Content-Length");
  }

  // Partial GET reconstructs then slices
  {
    std::unordered_map<std::string, std::string> h = {{"range", "bytes=5-7"}};
    auto r = http_request(host, port, "GET", "/o/http-ec-1", h, "", key);
    expect(r.status == 206, "HTTP EC Range 206");
    expect(r.body == body.substr(5, 3), "HTTP EC range body");
  }

  // Ranged PUT rejected
  {
    std::unordered_map<std::string, std::string> h = {
        {"content-range", "bytes 0-0/*"},
        {"content-length", "1"},
    };
    auto r = http_request(host, port, "PUT", "/o/http-ec-1", h, "Z", key);
    expect(r.status == 400, "HTTP EC ranged PUT 400");
  }

  // Degraded: purge one shard tip, HTTP GET still works
  {
    auto pl = place("http-ec-1", fx.map);
    auto* victim = fx.stores.get(pl.acting_set[2].aios_path);
    std::string err;
    auto st = victim->stat("http-ec-1", err);
    expect(st.has_value(), "victim tip");
    expect(victim->purge_version("http-ec-1", st->seq, true, err), "purge shard");
    auto r = http_request(host, port, "GET", "/o/http-ec-1", {}, "", key);
    expect(r.status == 200 && r.body == body, "HTTP EC degraded GET");
  }

  // HTTP txn prepare/commit of two objects on EC cluster
  {
    auto begin = http_request(host, port, "POST", "/txn", {}, "", key);
    expect(begin.status == 201, "HTTP EC txn begin");
    std::string txn_id;
    try {
      txn_id = nlohmann::json::parse(begin.body).value("txn_id", "");
    } catch (...) {
      expect(false, "txn begin json");
    }
    expect(!txn_id.empty(), "txn id");

    auto p1 = http_request(host, port, "PUT", "/txn/" + txn_id + "/o/txn-a", {}, "alpha", key);
    auto p2 = http_request(host, port, "PUT", "/txn/" + txn_id + "/o/txn-b", {}, "bravo", key);
    expect(p1.status == 200 && p2.status == 200, "HTTP EC txn prepare");

    // Invisible before commit
    expect(http_request(host, port, "GET", "/o/txn-a", {}, "", key).status != 200,
           "txn-a hidden");

    auto commit = http_request(host, port, "POST", "/txn/" + txn_id + "/commit", {}, "", key);
    expect(commit.status == 200, "HTTP EC txn commit");

    auto ga = http_request(host, port, "GET", "/o/txn-a", {}, "", key);
    auto gb = http_request(host, port, "GET", "/o/txn-b", {}, "", key);
    expect(ga.status == 200 && ga.body == "alpha", "txn-a after commit");
    expect(gb.status == 200 && gb.body == "bravo", "txn-b after commit");
  }

  // ISA-L over HTTP when available
  if (isal_ec_available()) {
    ioc.stop();
    th.join();

    // Brief pause so the previous listen socket can fully release.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EcHttpFixture fx_isal("isal", /*port_offset=*/1);
    boost::asio::io_context ioc2;
    HttpServer http2(ioc2, fx_isal.cfg, *fx_isal.svc);
    http2.start();
    std::thread th2([&] { ioc2.run(); });

    auto r =
        http_request(host, fx_isal.port, "PUT", "/o/http-ec-isal", {}, "isal-http", key);
    expect(r.status == 204, "HTTP ISA-L PUT");
    auto g = http_request(host, fx_isal.port, "GET", "/o/http-ec-isal", {}, "", key);
    expect(g.status == 200 && g.body == "isal-http", "HTTP ISA-L GET");
    expect(g.headers.count("x-aios-attr-aios.ec.codec") &&
               g.headers["x-aios-attr-aios.ec.codec"] == "isal",
           "HTTP ISA-L codec attr");

    ioc2.stop();
    th2.join();
    return failures();
  }

  ioc.stop();
  th.join();
  return failures();
}
