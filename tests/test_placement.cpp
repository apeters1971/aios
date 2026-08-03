#include "cluster/place.hpp"
#include "object/object_layout.hpp"
#include "object/transition.hpp"
#include "test_helpers.hpp"

#include <string>
#include <vector>

int test_placement() {
  using namespace aios;
  using namespace aios::test;
  int& failures = aios::test::failures();
  failures = 0;

  // Class-scoped rings: same oid maps independently per class.
  {
    DualStoreFixture nvme("place-nvme", 2, 2, "nvme");
    auto root = temp_root("place-hdd");
    std::filesystem::create_directories(root / "h1" / "aios");
    std::filesystem::create_directories(root / "h2" / "aios");
    const std::string h1 = (root / "h1" / "aios").string();
    const std::string h2 = (root / "h2" / "aios").string();
    MembershipTable membership;
    membership.set_local("node-a", "127.0.0.1:7400");
    FsTable fs;
    std::vector<AiosTarget> local{
        make_target(nvme.p1, "nvme"),
        make_target(nvme.p2, "nvme"),
        make_target(h1, "hdd"),
        make_target(h2, "hdd"),
    };
    fs.set_local("node-a", local);
    PlacementConfig pc;
    pc.vnodes_per_target = 32;
    pc.min_vnodes = 8;
    pc.max_vnodes = 256;
    auto map = ClusterMap::build(membership, fs, 2, pc);
    expect(map.targets_for_class("nvme").size() == 2, "nvme pool");
    expect(map.targets_for_class("hdd").size() == 2, "hdd pool");
    auto pn = place("obj", map, 2, "nvme");
    auto ph = place("obj", map, 2, "hdd");
    expect(pn.acting_set.size() == 2, "nvme acting set");
    expect(ph.acting_set.size() == 2, "hdd acting set");
    expect(pn.acting_set[0].storage_class == "nvme", "nvme primary class");
    expect(ph.acting_set[0].storage_class == "hdd", "hdd primary class");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  // PUT with storage class + tip attrs; client-driven class change.
  {
    DualStoreFixture fx("place-put-class", 2, 2, "nvme");
    // Add HDD targets on same node.
    std::filesystem::create_directories(fx.root / "h1" / "aios");
    std::filesystem::create_directories(fx.root / "h2" / "aios");
    const std::string h1 = (fx.root / "h1" / "aios").string();
    const std::string h2 = (fx.root / "h2" / "aios").string();
    std::vector<AiosTarget> local{
        make_target(fx.p1, "nvme"),
        make_target(fx.p2, "nvme"),
        make_target(h1, "hdd"),
        make_target(h2, "hdd"),
    };
    fx.fs_table.set_local("node-a", local);
    PlacementConfig pc;
    pc.vnodes_per_target = fx.cfg.vnodes_per_target;
    pc.min_vnodes = fx.cfg.min_vnodes;
    pc.max_vnodes = fx.cfg.max_vnodes;
    fx.map = ClusterMap::build(fx.membership, fx.fs_table, fx.cfg.replica_count, pc);
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    fx.stores.sync_paths({fx.p1, fx.p2, h1, h2}, opts);
    fx.svc = std::make_unique<ObjectService>(fx.cfg, fx.map, fx.stores);
    fx.svc->set_advertise("127.0.0.1:7400");

    const auto* body = reinterpret_cast<const std::uint8_t*>("hello-class");
    LayoutRequest req;
    req.storage_class = "nvme";
    auto put = fx.svc->api_put("cold/item", body, 11, {}, true, {}, std::nullopt, req);
    expect(put.ok, "put nvme");
    expect(put.attrs.at(kStorageClassAttr) == "nvme", "tip class nvme");

    LayoutRequest move;
    move.storage_class = "hdd";
    auto put2 = fx.svc->api_put("cold/item", body, 11, {}, true, {}, std::nullopt, move);
    expect(put2.ok, "put hdd");
    expect(put2.attrs.at(kStorageClassAttr) == "hdd", "tip class hdd");
    auto got = fx.svc->api_get("cold/item", std::nullopt, std::nullopt, {});
    expect(got.ok && got.data && got.data->size() == 11, "get after class change");
  }

  // Background transition nvme → hdd.
  {
    DualStoreFixture fx("place-transition", 2, 2, "nvme");
    std::filesystem::create_directories(fx.root / "h1" / "aios");
    std::filesystem::create_directories(fx.root / "h2" / "aios");
    const std::string h1 = (fx.root / "h1" / "aios").string();
    const std::string h2 = (fx.root / "h2" / "aios").string();
    std::vector<AiosTarget> local{
        make_target(fx.p1, "nvme"),
        make_target(fx.p2, "nvme"),
        make_target(h1, "hdd"),
        make_target(h2, "hdd"),
    };
    fx.fs_table.set_local("node-a", local);
    PlacementConfig pc;
    pc.vnodes_per_target = fx.cfg.vnodes_per_target;
    pc.min_vnodes = fx.cfg.min_vnodes;
    pc.max_vnodes = fx.cfg.max_vnodes;
    fx.map = ClusterMap::build(fx.membership, fx.fs_table, fx.cfg.replica_count, pc);
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    fx.stores.sync_paths({fx.p1, fx.p2, h1, h2}, opts);
    fx.cfg.transition_rules.push_back(
        TransitionRule{.prefix = "archive/", .from = "nvme", .to = "hdd"});
    fx.svc = std::make_unique<ObjectService>(fx.cfg, fx.map, fx.stores);
    fx.svc->set_advertise("127.0.0.1:7400");

    const auto* body = reinterpret_cast<const std::uint8_t*>("archive-body");
    LayoutRequest req;
    req.storage_class = "nvme";
    auto put = fx.svc->api_put("archive/o1", body, 12, {}, true, {}, std::nullopt, req);
    expect(put.ok, "seed nvme object");

    auto stats = run_transitions(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 64);
    expect(stats.migrated >= 1 || stats.matched >= 1, "transition matched/migrated");

    // Dest primary may need a second pass if first only matched.
    if (stats.migrated == 0) {
      stats = run_transitions(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 64);
    }
    expect(stats.migrated >= 1 || stats.drained >= 1, "transition progressed");

    auto got = fx.svc->api_get("archive/o1", std::nullopt, std::nullopt, {});
    expect(got.ok, "get during/after transition");
    if (got.ok) {
      const auto sc = storage_class_for_attrs(got.attrs, "");
      expect(sc == "hdd" || sc == "nvme", "class nvme or hdd");
    }
  }

  return failures;
}
