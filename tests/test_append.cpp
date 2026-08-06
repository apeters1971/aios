#include "test_helpers.hpp"

#include "client/session.hpp"
#include "http/http_server.hpp"
#include "object/object_layout.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using aios::test::DualStoreFixture;
using aios::test::expect;
using aios::test::failures;

struct HttpFixture {
  DualStoreFixture fx;
  int port_num;
  std::string host{"127.0.0.1"};
  std::string port;
  boost::asio::io_context ioc;
  std::unique_ptr<aios::HttpServer> http;
  std::thread th;

  explicit HttpFixture(const char* prefix)
      : fx(prefix), port_num(19150 + static_cast<int>(::getpid() % 200)) {
    port = std::to_string(port_num);
    fx.cfg.http_listen = host + ":" + port;
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership);
    http->start();
    th = std::thread([this] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  ~HttpFixture() {
    ioc.stop();
    if (th.joinable()) th.join();
  }

  aios::SessionConfig cfg() const {
    return aios::SessionConfig{.endpoint = host + ":" + port,
                               .cluster_key = fx.cfg.cluster_key};
  }
};

}  // namespace

int test_append() {
  using namespace aios;
  failures() = 0;

  // Concurrent appends: both payloads present, sizes add up, distinct offsets.
  {
    DualStoreFixture fx("aios-append-conc");
    const std::string oid = "log/conc";
    const std::string a(64, 'A');
    const std::string b(64, 'B');
    std::atomic<int> ok{0};
    AppendResult ra, rb;
    std::thread t1([&] {
      auto r = fx.svc->api_append(oid, reinterpret_cast<const std::uint8_t*>(a.data()), a.size(),
                                  {}, false, {}, {}, std::nullopt);
      if (r.ok && r.json_body) {
        ra.offset = (*r.json_body)["offset"].get<std::uint64_t>();
        ra.size = (*r.json_body)["size"].get<std::uint64_t>();
        ok.fetch_add(1);
      }
    });
    std::thread t2([&] {
      auto r = fx.svc->api_append(oid, reinterpret_cast<const std::uint8_t*>(b.data()), b.size(),
                                  {}, false, {}, {}, std::nullopt);
      if (r.ok && r.json_body) {
        rb.offset = (*r.json_body)["offset"].get<std::uint64_t>();
        rb.size = (*r.json_body)["size"].get<std::uint64_t>();
        ok.fetch_add(1);
      }
    });
    t1.join();
    t2.join();
    expect(ok.load() == 2, "concurrent append both ok");
    expect(ra.offset != rb.offset, "distinct offsets");
    expect((ra.offset == 0 && rb.offset == a.size()) || (rb.offset == 0 && ra.offset == b.size()),
           "offsets are sequential");
    auto got = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
    expect(got.ok && got.data && got.data->size() == a.size() + b.size(), "concat size");
    if (got.data) {
      const std::string body(got.data->begin(), got.data->end());
      expect(body.find(a) != std::string::npos && body.find(b) != std::string::npos,
             "both payloads present");
    }
  }

  // Lock held without token → 409 / lock_held
  {
    DualStoreFixture fx("aios-append-lock");
    const std::string oid = "log/locked";
    auto lk = fx.svc->api_lock_acquire(oid, 30000);
    expect(lk.ok, "lock acquire");
    auto denied = fx.svc->api_append(oid, reinterpret_cast<const std::uint8_t*>("x"), 1, {}, false,
                                     {}, {}, std::nullopt);
    expect(!denied.ok && denied.code == "lock_held", "append without token denied");
    const std::string token = (*lk.json_body)["token"].get<std::string>();
    auto ok =
        fx.svc->api_append(oid, reinterpret_cast<const std::uint8_t*>("x"), 1, {}, false, {}, {},
                           token);
    expect(ok.ok, "append with token");
  }

  // Wrong primary → not_primary (HTTP maps to 307)
  {
    DualStoreFixture fx("aios-append-np");
    fx.cfg.node_id = "not-primary-node";
    fx.svc = std::make_unique<ObjectService>(fx.cfg, fx.map, fx.stores);
    fx.svc->set_advertise("127.0.0.1:7400");
    auto r = fx.svc->api_append("log/np", reinterpret_cast<const std::uint8_t*>("z"), 1, {}, false,
                                {}, {}, std::nullopt);
    expect(!r.ok && r.code == "not_primary", "append not_primary");
  }

  // EC tip → append rejected
  {
    namespace fs = std::filesystem;
    auto root = aios::test::temp_root("aios-append-ec");
    std::string p1, p2, p3;
    for (auto name : {"t1", "t2", "t3"}) {
      fs::create_directories(root / name / "aios");
    }
    p1 = (root / "t1" / "aios").string();
    p2 = (root / "t2" / "aios").string();
    p3 = (root / "t3" / "aios").string();

    MembershipTable membership;
    membership.set_local("node-a", "127.0.0.1:7400");
    FsTable fs_table;
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

    Config cfg;
    cfg.node_id = "node-a";
    cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
    cfg.durability = "replica";
    cfg.replica_count = 3;
    cfg.write_quorum = 2;
    cfg.ec_k = 2;
    cfg.ec_m = 1;
    cfg.clone_required = false;
    cfg.max_versions = 16;

    ClusterMap map = ClusterMap::build(membership, fs_table, cfg.replica_count, PlacementConfig{});
    LocalStores stores;
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    stores.sync_paths({p1, p2, p3}, opts);
    ObjectService svc(cfg, map, stores);
    svc.set_advertise("127.0.0.1:7400");

    LayoutRequest layout;
    layout.layout = "ec";
    const std::string body = "abcdefgh";
    auto put = svc.api_put("ecobj", reinterpret_cast<const std::uint8_t*>(body.data()),
                           body.size(), {}, true, {}, std::nullopt, layout);
    expect(put.ok, "ec put for append reject test");
    auto ap = svc.api_append("ecobj", reinterpret_cast<const std::uint8_t*>("z"), 1, {}, false,
                             {}, {}, std::nullopt);
    expect(!ap.ok && ap.code == "bad_request", "append rejected on EC tip");
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  // Session HTTP append + 307 follow path (same node)
  {
    HttpFixture hf("aios-append-http");
    Session s(hf.cfg());
    auto r1 = s.append("http-log", "hello");
    expect(r1.offset == 0 && r1.size == 5, "session append create");
    auto r2 = s.append("http-log", "!");
    expect(r2.offset == 5 && r2.size == 6, "session append extend");
    auto snap = s.get_object("http-log");
    expect(snap.exists && snap.body == "hello!", "session get after append");
  }

  return failures();
}
