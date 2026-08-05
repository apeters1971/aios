#include "object/backup.hpp"
#include "http/backup_policy.hpp"
#include "object/archive_bag.hpp"
#include "object/object_layout.hpp"
#include "posix/aios_posix.h"
#include "test_helpers.hpp"
#include "util/log.hpp"

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
  std::shared_ptr<aios::BackupPolicyStore> policies;
  std::thread th;

  explicit HttpFixture(const char* prefix, bool with_policies = false)
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
    fx.cfg.admin = true;
    if (with_policies) {
      policies = std::make_shared<aios::BackupPolicyStore>(fx.cfg, *fx.svc);
    }
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership, nullptr, nullptr,
                                              nullptr, policies);
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

bool oid_exists(aios::ObjectService& svc, const std::string& oid) {
  auto h = svc.api_head(oid, {});
  return h.ok && h.info && !h.info->is_delete;
}

void put_manifest(aios::ObjectService& svc, const std::string& volume, const std::string& id,
                  const std::string& path, const std::string& policy_id, std::int64_t created_ms) {
  nlohmann::json man{{"aios_backup_manifest", 1},
                     {"kind", "posix"},
                     {"volume", volume},
                     {"snap_id", id},
                     {"path", path},
                     {"root_ino", 1},
                     {"created_ms", created_ms},
                     {"oids", 0}};
  if (!policy_id.empty()) man["policy_id"] = policy_id;
  const std::string body = man.dump();
  aios::LayoutRequest req;
  const std::string oid = "posix/" + volume + "/.snap/" + id + "/manifest";
  expect(svc.api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), {}, true,
                     {}, std::nullopt, req)
             .ok,
         "put manifest");
  // Dummy tip so prune sees something to delete; mark frozen for drain gate.
  const std::string tip = "posix/" + volume + "/.snap/" + id + "/ino/1";
  std::unordered_map<std::string, std::string> attrs{{"aios.frozen", "1"},
                                                     {"aios.bag_oid", "bag/x"}};
  expect(svc.api_put(tip, reinterpret_cast<const std::uint8_t*>("x"), 1, attrs, true, {},
                     std::nullopt, req)
             .ok,
         "put tip");
}

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

    auto stats = aios::run_backup(http.fx.cfg, "127.0.0.1:7400", http.fx.map, http.fx.stores,
                                  *http.fx.svc, 256);
    expect(stats.snaps_created >= 1, "backup snap");
    expect(stats.bags_sealed >= 1 || stats.oids_copied >= 1, "backup packed or copied");

    aios_posix_unmount(fs);
  }

  // Subtree snapshot excludes siblings outside path.
  {
    HttpFixture http("aios-backup-subtree");
    aios_posix_config cfg{};
    const std::string ep = http.endpoint();
    cfg.endpoint = ep.c_str();
    cfg.cluster_key = http.fx.cfg.cluster_key.c_str();
    cfg.volume = "svol";
    cfg.stripe_unit = 4096;
    cfg.uid = 1000;
    cfg.gid = 1000;
    int err = 0;
    aios_posix_fs* fs = aios_posix_mount(&cfg, &err);
    expect(fs != nullptr, "subtree mount");
    if (!fs) return failures();

    aios_posix_stat home{}, other{}, inside{}, outside{};
    expect(aios_posix_mkdir(fs, 1, "home", 0755, &home) == 0, "mkdir home");
    expect(aios_posix_mkdir(fs, 1, "other", 0755, &other) == 0, "mkdir other");
    expect(aios_posix_create(fs, home.ino, "in.txt", 0644, &inside) == 0, "create in");
    expect(aios_posix_create(fs, other.ino, "out.txt", 0644, &outside) == 0, "create out");
    size_t wrote = 0;
    expect(aios_posix_write(fs, inside.ino, 0, "IN", 2, &wrote) == 0, "write in");
    expect(aios_posix_write(fs, outside.ino, 0, "OUT", 3, &wrote) == 0, "write out");

    char snap_id[128]{};
    expect(aios_posix_snapshot_at(fs, "/home", snap_id, sizeof(snap_id)) == 0, "subtree snap");

    const std::string pref = "posix/svol/.snap/" + std::string(snap_id) + "/";
    expect(oid_exists(*http.fx.svc, pref + "ino/" + std::to_string(inside.ino)), "has inside ino");
    expect(!oid_exists(*http.fx.svc, pref + "ino/" + std::to_string(outside.ino)),
           "no outside ino");
    auto man = http.fx.svc->api_get(pref + "manifest", std::nullopt, std::nullopt, {});
    expect(man.ok && man.data, "manifest");
    auto mj = nlohmann::json::parse(std::string(man.data->begin(), man.data->end()));
    expect(mj.value("path", "") == "/home", "manifest path");
    expect(mj.value("root_ino", 0ull) == home.ino, "manifest root");

    // ObjectService path API
    std::string sid2, serr;
    std::size_t copied = 0;
    expect(aios::backup_snapshot_posix(*http.fx.svc, "svol", "/home", sid2, serr, &copied),
           "svc subtree snap");
    expect(copied > 0, "svc copied");
    expect(!oid_exists(*http.fx.svc, "posix/svol/.snap/" + sid2 + "/ino/" +
                                         std::to_string(outside.ino)),
           "svc no outside");

    aios_posix_unmount(fs);
  }

  // GFS prune keeps recent + monthly.
  {
    HttpFixture http("aios-backup-gfs");
    const std::string vol = "gvol";
    // Ensure volume super exists via a tiny snap path setup: put super only.
    aios::LayoutRequest req;
    nlohmann::json super{{"aios_posix_super", 1}, {"next_ino", 2}, {"frozen", false}};
    const std::string sb = super.dump();
    expect(http.fx.svc
               ->api_put("posix/" + vol + "/super",
                         reinterpret_cast<const std::uint8_t*>(sb.data()), sb.size(), {}, true, {},
                         std::nullopt, req)
               .ok,
           "super");

    const auto now = aios::now_ms();
    const std::int64_t day = 24ll * 60 * 60 * 1000;
    put_manifest(*http.fx.svc, vol, "recent", "/home", "pol1", now - day);
    put_manifest(*http.fx.svc, vol, "old_month_a", "/home", "pol1", now - 40 * day);
    put_manifest(*http.fx.svc, vol, "old_month_b", "/home", "pol1",
                 now - 40 * day + 3600 * 1000);  // newer in same month
    put_manifest(*http.fx.svc, vol, "ancient", "/home", "pol1", now - 400 * day);

    const auto pruned =
        aios::backup_prune_gfs(*http.fx.svc, vol, "/home", "pol1", /*keep_days=*/7,
                               /*keep_monthly=*/2);
    expect(pruned > 0, "pruned some");
    expect(oid_exists(*http.fx.svc, "posix/" + vol + "/.snap/recent/manifest"), "kept recent");
    expect(oid_exists(*http.fx.svc, "posix/" + vol + "/.snap/old_month_b/manifest"),
           "kept newest in month");
    expect(!oid_exists(*http.fx.svc, "posix/" + vol + "/.snap/old_month_a/manifest"),
           "dropped older in month");
    expect(!oid_exists(*http.fx.svc, "posix/" + vol + "/.snap/ancient/manifest"),
           "dropped ancient");
  }

  // Policy store CAS create/list/delete + due check.
  {
    HttpFixture http("aios-backup-pol", true);
    expect(http.policies != nullptr, "policies store");
    aios::BackupPolicy p;
    p.kind = "posix";
    p.volume = "default";
    p.path = "/";
    p.at = "00:00";
    p.tz = "UTC";
    p.keep_days = 7;
    p.keep_monthly = 12;
    p.enabled = true;
    std::string err;
    auto stored = http.policies->upsert(p, err);
    expect(stored.has_value(), "upsert");
    expect(!stored->id.empty(), "id assigned");
    expect(http.policies->list().size() == 1, "list1");
    expect(http.policies->get(stored->id).has_value(), "get");

    // Due when last_run is old and now past today's trigger (or yesterday's).
    aios::BackupPolicy due = *stored;
    due.last_run_ms = 0;
    expect(aios::BackupPolicyStore::is_due(due, aios::now_ms()), "is due");
    due.last_run_ms = aios::now_ms();
    expect(!aios::BackupPolicyStore::is_due(due, aios::now_ms()), "not due after touch");

    expect(http.policies->remove(stored->id, err), "remove");
    expect(http.policies->list().empty(), "empty after rm");
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
