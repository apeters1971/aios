#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "cluster/place.hpp"
#include "ec/ec_attrs.hpp"
#include "http/http_auth.hpp"
#include "http/http_server.hpp"
#include "net/framing.hpp"
#include "object/object_layout.hpp"
#include "object/repair.hpp"
#include "util/base64.hpp"
#include "util/log.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

using tcp = boost::asio::ip::tcp;
using aios::test::temp_root;

struct MixedLayoutFixture {
  std::filesystem::path root;
  std::string p1, p2, p3;
  aios::MembershipTable membership;
  aios::FsTable fs_table;
  aios::Config cfg;
  aios::ClusterMap map;
  aios::LocalStores stores;
  std::unique_ptr<aios::ObjectService> svc;

  MixedLayoutFixture() {
    using namespace aios;
    root = temp_root("aios-layout");
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
      t.storage_class = "nvme";
      t.usable = true;
      t.bavail = 1000;
      local.push_back(t);
    }
    fs_table.set_local("node-a", local);

    cfg.node_id = "node-a";
    cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
    cfg.durability = "replica";
    cfg.replica_count = 3;
    cfg.write_quorum = 2;
    cfg.ec_k = 2;
    cfg.ec_m = 1;
    cfg.clone_required = false;
    cfg.max_versions = 16;

    map = ClusterMap::build(membership, fs_table, cfg.replica_count, PlacementConfig{});
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    stores.sync_paths({p1, p2, p3}, opts);
    svc = std::make_unique<ObjectService>(cfg, map, stores);
    svc->set_advertise("127.0.0.1:7400");
  }

  ~MixedLayoutFixture() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
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

struct HttpResponse {
  int status{0};
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

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

TEST(Layout, Basic) {
  using namespace aios;
  // resolve_object_layout defaults + caps
  {
    Config cfg;
    cfg.durability = "replica";
    cfg.replica_count = 3;
    cfg.ec_k = 2;
    cfg.ec_m = 1;
    cfg.max_ec_k = 4;
    ObjectLayout layout;
    std::string err;
    EXPECT_TRUE(resolve_object_layout(cfg, "any", {}, layout, err) && !layout.is_ec() && layout.n == 3) << "default replica layout";
    LayoutRequest ec_req;
    ec_req.layout = "ec";
    EXPECT_TRUE(resolve_object_layout(cfg, "any", ec_req, layout, err) && layout.is_ec() &&
               layout.n == 3 && layout.ec_codec == "xor") << "request ec uses defaults";
    ec_req.ec_k = 8;
    EXPECT_TRUE(!resolve_object_layout(cfg, "any", ec_req, layout, err)) << "ec_k cap";
  }

  // Prefix layout rules: longest match; request overrides
  {
    Config cfg;
    cfg.durability = "replica";
    cfg.replica_count = 3;
    cfg.ec_k = 2;
    cfg.ec_m = 1;
    cfg.layout_rules = {
        {.prefix = "cold/", .layout = "ec", .ec_k = 2, .ec_m = 1, .ec_codec = "xor"},
        {.prefix = "hot/", .layout = "replica"},
        {.prefix = "cold/logs/", .layout = "replica"},
    };
    std::string err;
    EXPECT_TRUE(normalize_config(cfg, err)) << "normalize layout_rules";

    ObjectLayout layout;
    EXPECT_TRUE(resolve_object_layout(cfg, "cold/x", {}, layout, err) && layout.is_ec()) << "cold/ → ec rule";
    EXPECT_TRUE(resolve_object_layout(cfg, "hot/x", {}, layout, err) && !layout.is_ec()) << "hot/ → replica rule";
    EXPECT_TRUE(resolve_object_layout(cfg, "cold/logs/a", {}, layout, err) && !layout.is_ec()) << "longer prefix cold/logs/ beats cold/";
    EXPECT_TRUE(resolve_object_layout(cfg, "other/x", {}, layout, err) && !layout.is_ec()) << "no rule → cluster default";

    LayoutRequest force_repl;
    force_repl.layout = "replica";
    EXPECT_TRUE(resolve_object_layout(cfg, "cold/x", force_repl, layout, err) && !layout.is_ec()) << "request wins over prefix rule";
  }

  // Service: prefix rules applied on PUT without headers
  {
    MixedLayoutFixture fx;
    fx.cfg.layout_rules = {
        {.prefix = "cold/", .layout = "ec", .ec_k = 2, .ec_m = 1, .ec_codec = "xor"},
        {.prefix = "hot/", .layout = "replica"},
    };
    std::string nerr;
    EXPECT_TRUE(normalize_config(fx.cfg, nerr)) << "normalize fixture rules";
    fx.svc = std::make_unique<ObjectService>(fx.cfg, fx.map, fx.stores);
    fx.svc->set_advertise("127.0.0.1:7400");

    const auto* body = reinterpret_cast<const std::uint8_t*>("prefix-rule-body");
    auto cold = fx.svc->api_put("cold/obj", body, 16, {}, true, {});
    EXPECT_TRUE(cold.ok && attrs_are_ec(cold.attrs)) << "cold/ put uses EC rule";
    auto hot = fx.svc->api_put("hot/obj", body, 16, {}, true, {});
    EXPECT_TRUE(hot.ok && !attrs_are_ec(hot.attrs) && hot.attrs.at(kLayoutAttr) == "replica") << "hot/ put uses replica rule";
    LayoutRequest force_repl;
    force_repl.layout = "replica";
    auto override_put =
        fx.svc->api_put("cold/override", body, 16, {}, true, {}, std::nullopt, force_repl);
    EXPECT_TRUE(override_put.ok && !attrs_are_ec(override_put.attrs)) << "header/request overrides cold/ EC rule";
  }

  // place(oid, map, n, "nvme") hard-fails when n > targets
  {
    MixedLayoutFixture fx;
    auto ok = place("o", fx.map, 3, "nvme");
    EXPECT_TRUE(ok.acting_set.size() == 3) << "place n=3";
    EXPECT_TRUE(place("o", fx.map, 4, "nvme").acting_set.empty()) << "place n>targets empty";
    EXPECT_TRUE(place("o", fx.map, "nvme").acting_set.size() == 3) << "place compat";
  }

  // Same cluster: replica object + EC object
  {
    MixedLayoutFixture fx;
    const auto* repl_body = reinterpret_cast<const std::uint8_t*>("replica-body!!");
    const auto* ec_body = reinterpret_cast<const std::uint8_t*>("ec-body-payload");

    auto rput = fx.svc->api_put("layout/repl", repl_body, 14, {}, true, {});
    EXPECT_TRUE(rput.ok) << "replica put";
    EXPECT_TRUE(rput.attrs.count(kLayoutAttr) && rput.attrs.at(kLayoutAttr) == "replica") << "replica layout attr";
    EXPECT_TRUE(!attrs_are_ec(rput.attrs)) << "replica has no ec attrs";

    LayoutRequest ec_req;
    ec_req.layout = "ec";
    auto eput = fx.svc->api_put("layout/ec", ec_body, 15, {}, true, {}, std::nullopt, ec_req);
    EXPECT_TRUE(eput.ok) << "ec put on replica-default cluster";
    EXPECT_TRUE(eput.attrs.count(kLayoutAttr) && eput.attrs.at(kLayoutAttr) == "ec") << "ec layout attr";
    EXPECT_TRUE(attrs_are_ec(eput.attrs)) << "ec attrs present";

    auto rg = fx.svc->api_get("layout/repl", std::nullopt, std::nullopt, {});
    EXPECT_TRUE(rg.ok && rg.data &&
               std::string(rg.data->begin(), rg.data->end()) == "replica-body!!") << "replica get";
    auto eg = fx.svc->api_get("layout/ec", std::nullopt, std::nullopt, {});
    EXPECT_TRUE(eg.ok && eg.data &&
               std::string(eg.data->begin(), eg.data->end()) == "ec-body-payload") << "ec get";

    // Ranged put on EC tip rejected; on replica tip ok.
    EXPECT_TRUE(!fx.svc->api_put_range("layout/ec", 0, ec_body, 1, {}, false, {}).ok) << "range put on ec tip rejected";
    EXPECT_TRUE(fx.svc->api_put_range("layout/repl", 0, reinterpret_cast<const std::uint8_t*>("R"),
                                 1, {}, false, {})
               .ok) << "range put on replica tip ok";

    // Repair EC object after dropping a shard (attrs-first, not cfg.durability).
    auto pl = place("layout/ec", fx.map, 3, "nvme");
    std::string err;
    auto* victim = fx.stores.get(pl.acting_set[2].aios_path);
    auto st = victim->stat("layout/ec", err);
    EXPECT_TRUE(st.has_value() && victim->purge_version("layout/ec", st->seq, true, err)) << "purge ec shard";
    auto stats = run_repair(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 256);
    EXPECT_TRUE(stats.repaired >= 1) << "ec repaired on replica-default cluster";
    auto eg2 = fx.svc->api_get("layout/ec", std::nullopt, std::nullopt, {});
    EXPECT_TRUE(eg2.ok && eg2.data &&
               std::string(eg2.data->begin(), eg2.data->end()) == "ec-body-payload") << "ec get after repair";
  }

  // TCP++ ObjectPut with layout fields
  {
    MixedLayoutFixture fx;
    const std::string oid = "layout/tcp-ec";
    auto pl = place(oid, fx.map, 3, "nvme");
    EXPECT_TRUE(!pl.acting_set.empty()) << "tcp place";
    const auto& primary = pl.acting_set[0];

    Frame put;
    put.type = MsgType::ObjectPut;
    put.body = {
        {"epoch", fx.map.epoch},
        {"aios_path", primary.aios_path},
        {"oid", oid},
        {"data_b64", base64_encode(std::string("tcp-ec-body!"))},
        {"attrs", nlohmann::json::object()},
        {"role", "primary"},
        {"layout", "ec"},
        {"ec_k", 2},
        {"ec_m", 1},
        {"ec_codec", "xor"},
    };
    auto reply = fx.svc->handle(put);
    EXPECT_TRUE(reply.body.value("ok", false)) << "TCP EC ObjectPut ok";

    auto g = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
    EXPECT_TRUE(g.ok && g.data &&
               std::string(g.data->begin(), g.data->end()) == "tcp-ec-body!") << "TCP EC ObjectPut readable via api_get";
    EXPECT_TRUE(attrs_are_ec(g.attrs) && g.attrs.count(kLayoutAttr) &&
               g.attrs.at(kLayoutAttr) == "ec") << "TCP EC layout attrs";
  }

  // HTTP headers round-trip on replica-default cluster
  {
    MixedLayoutFixture fx;
    const int port_num = 18600 + static_cast<int>(::getpid() % 400);
    fx.cfg.http_listen = "127.0.0.1:" + std::to_string(port_num);
    const std::string port = std::to_string(port_num);

    boost::asio::io_context ioc;
    HttpServer http(ioc, fx.cfg, *fx.svc, fx.membership);
    http.start();
    std::thread th([&] { ioc.run(); });

    const std::string host = "127.0.0.1";
    const std::string& key = fx.cfg.cluster_key;

    {
      auto r = http_request(host, port, "PUT", "/o/http-repl", {}, "plain", key);
      EXPECT_TRUE(r.status == 204) << "HTTP replica PUT";
    }
    {
      std::unordered_map<std::string, std::string> h = {
          {"x-aios-layout", "ec"},
          {"x-aios-ec-k", "2"},
          {"x-aios-ec-m", "1"},
          {"x-aios-ec-codec", "xor"},
      };
      auto r = http_request(host, port, "PUT", "/o/http-ec", h, "striped!", key);
      EXPECT_TRUE(r.status == 204) << "HTTP EC layout PUT";
    }
    {
      auto g = http_request(host, port, "GET", "/o/http-repl", {}, "", key);
      EXPECT_TRUE(g.status == 200 && g.body == "plain") << "HTTP replica GET";
      EXPECT_TRUE(g.headers.count("x-aios-attr-aios.layout") &&
                 g.headers["x-aios-attr-aios.layout"] == "replica") << "HTTP replica layout attr";
    }
    {
      auto g = http_request(host, port, "GET", "/o/http-ec", {}, "", key);
      EXPECT_TRUE(g.status == 200 && g.body == "striped!") << "HTTP EC GET";
      EXPECT_TRUE(g.headers.count("x-aios-attr-aios.layout") &&
                 g.headers["x-aios-attr-aios.layout"] == "ec") << "HTTP EC layout attr";
      EXPECT_TRUE(g.headers.count("x-aios-attr-aios.ec.k")) << "HTTP EC k attr";
    }

    ioc.stop();
    th.join();
  }

  }
