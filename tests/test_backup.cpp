#include "object/backup.hpp"
#include "object/archive_bag.hpp"
#include "object/object_layout.hpp"
#include "posix/aios_posix.h"
#include "test_helpers.hpp"

#include "http/http_server.hpp"

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>

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
      : fx(prefix, 2, 2, "nvme"), port_num(19650 + static_cast<int>(::getpid() % 200)) {
    std::filesystem::create_directories(fx.root / "a1" / "aios");
    std::filesystem::create_directories(fx.root / "a2" / "aios");
    const std::string a1 = (fx.root / "a1" / "aios").string();
    const std::string a2 = (fx.root / "a2" / "aios").string();
    std::vector<aios::AiosTarget> local{
        aios::test::make_target(fx.p1, "nvme"),
        aios::test::make_target(fx.p2, "nvme"),
        aios::test::make_target(a1, "archive"),
        aios::test::make_target(a2, "archive"),
    };
    fx.fs_table.set_local("node-a", local);
    aios::PlacementConfig pc;
    pc.vnodes_per_target = fx.cfg.vnodes_per_target;
    pc.min_vnodes = fx.cfg.min_vnodes;
    pc.max_vnodes = fx.cfg.max_vnodes;
    fx.map = aios::ClusterMap::build(fx.membership, fx.fs_table, fx.cfg.replica_count, pc);
    aios::ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    fx.stores.sync_paths({fx.p1, fx.p2, a1, a2}, opts);
    fx.svc = std::make_unique<aios::ObjectService>(fx.cfg, fx.map, fx.stores);
    fx.svc->set_advertise("127.0.0.1:7400");

    port = std::to_string(port_num);
    fx.cfg.http_listen = host + ":" + port;
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership);
    http->start();
    th = std::thread([this] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }

  ~HttpFixture() {
    ioc.stop();
    if (th.joinable()) th.join();
  }

  std::string endpoint() const { return host + ":" + port; }
};

}  // namespace

int test_backup() {
  failures() = 0;

  // POSIX snapshot ABI + backup pack/drain of snap prefix.
  {
    HttpFixture http("aios-backup-posix");
    aios_posix_config cfg{};
    const std::string ep = http.endpoint();
    cfg.endpoint = ep.c_str();
    cfg.cluster_key = http.fx.cfg.cluster_key.c_str();
    cfg.volume = "bvol";
    cfg.stripe_unit = 4096;
    cfg.uid = 1000;
    cfg.gid = 1000;
    int err = 0;
    aios_posix_fs* fs = aios_posix_mount(&cfg, &err);
    expect(fs != nullptr, "mount");
    if (!fs) return failures();

    aios_posix_stat st{};
    expect(aios_posix_create(fs, 1, "f.txt", 0644, &st) == 0, "create");
    const char* payload = "backup-me";
    size_t wrote = 0;
    expect(aios_posix_write(fs, st.ino, 0, payload, std::strlen(payload), &wrote) == 0, "write");

    char snap_id[128]{};
    expect(aios_posix_snapshot(fs, snap_id, sizeof(snap_id)) == 0, "snapshot");
    expect(std::strlen(snap_id) > 0, "snap id");

    // Live volume writable again.
    expect(aios_posix_write(fs, st.ino, 0, "x", 1, &wrote) == 0, "write after snap");

    aios::BackupRule br;
    br.kind = "posix";
    br.volume = "bvol";
    br.retain_snaps = 2;
    br.from = "nvme";
    br.staging_class = "archive";
    br.tape_sink = "external";
    br.tape_root = (http.fx.root / "tape").string();
    std::filesystem::create_directories(br.tape_root);
    http.fx.cfg.backup_rules.push_back(br);

    // Snapshot already exists; run_backup will create another + pack.
    auto stats = aios::run_backup(http.fx.cfg, "127.0.0.1:7400", http.fx.map, http.fx.stores,
                                  *http.fx.svc, 256);
    expect(stats.snaps_created >= 1, "backup snap");
    expect(stats.bags_sealed >= 1 || stats.oids_copied >= 1, "backup packed or copied");

    aios_posix_unmount(fs);
  }

  // VBD clone+seal via ObjectService.
  {
    HttpFixture http("aios-backup-vbd");
    aios::LayoutRequest req;
    req.storage_class = "nvme";
    nlohmann::json hdr{{"aiosvd", 1},
                       {"pool", "p"},
                       {"name", "vol0"},
                       {"size", 1024ull * 1024ull},
                       {"obj_order", 20}};
    const std::string hb = hdr.dump();
    expect(http.fx.svc
               ->api_put("vd/p/vol0/header", reinterpret_cast<const std::uint8_t*>(hb.data()),
                         hb.size(), {}, true, {}, std::nullopt, req)
               .ok,
           "vbd header");
    std::string chunk(64, 'V');
    expect(http.fx.svc
               ->api_put("vd/p/vol0/data.0000000000000000",
                         reinterpret_cast<const std::uint8_t*>(chunk.data()), chunk.size(), {},
                         true, {}, std::nullopt, req)
               .ok,
           "vbd data0");

    std::string err;
    std::size_t copied = 0;
    expect(aios::backup_snapshot_vbd(*http.fx.svc, "p", "vol0", "vol0-snap", err, &copied),
           "vbd snapshot seal");
    auto h = http.fx.svc->api_get("vd/p/vol0-snap/header", std::nullopt, std::nullopt, {});
    expect(h.ok && h.data, "snap header");
    auto hj = nlohmann::json::parse(std::string(h.data->begin(), h.data->end()));
    expect(hj.value("sealed", false) == true, "sealed");
    expect(!hj.contains("parent_pool") || hj["parent_pool"].get<std::string>().empty(),
           "no parent");
    auto d = http.fx.svc->api_get("vd/p/vol0-snap/data.0000000000000000", std::nullopt,
                                  std::nullopt, {});
    expect(d.ok && d.data && std::string(d.data->begin(), d.data->end()) == chunk,
           "sealed data");
  }

  return failures();
}
