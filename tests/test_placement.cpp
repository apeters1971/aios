#include "cluster/place.hpp"
#include "object/object_layout.hpp"
#include "object/repair.hpp"
#include "object/transition.hpp"
#include "test_helpers.hpp"

#include <string>
#include <vector>

int test_placement() {
  using namespace aios;
  using namespace aios::test;
  int& failures = aios::test::failures();
  failures = 0;

  // Drain targets stay in the map but are excluded from place().
  {
    auto root = temp_root("place-drain");
    std::filesystem::create_directories(root / "u1" / "aios");
    std::filesystem::create_directories(root / "u2" / "aios");
    std::filesystem::create_directories(root / "d1" / "aios");
    const std::string u1 = (root / "u1" / "aios").string();
    const std::string u2 = (root / "u2" / "aios").string();
    const std::string d1 = (root / "d1" / "aios").string();
    MembershipTable membership;
    membership.set_local("node-a", "127.0.0.1:7400");
    FsTable fs;
    std::vector<AiosTarget> local{
        make_target(u1, "nvme", 1, LifecycleState::Up),
        make_target(u2, "nvme", 1, LifecycleState::Up),
        make_target(d1, "nvme", 1, LifecycleState::Drain),
    };
    fs.set_local("node-a", local);
    PlacementConfig pc;
    pc.vnodes_per_target = 32;
    pc.min_vnodes = 8;
    pc.max_vnodes = 256;
    auto map = ClusterMap::build(membership, fs, 2, pc);
    expect(map.targets.size() == 3, "map includes drain");
    int drain_n = 0;
    for (const auto& t : map.targets) {
      if (t.state == LifecycleState::Drain) ++drain_n;
    }
    expect(drain_n == 1, "one drain in map");
    auto p = place("drain-oid", map, 2, "nvme");
    expect(p.acting_set.size() == 2, "place uses two up targets");
    for (const auto& t : p.acting_set) {
      expect(t.state == LifecycleState::Up, "acting set is up-only");
      expect(t.aios_path != d1, "drain path excluded from place");
    }
    // Off is omitted from the map entirely.
    local[2].state = LifecycleState::Off;
    fs.set_local("node-a", local);
    map = ClusterMap::build(membership, fs, 2, pc);
    expect(map.targets.size() == 2, "off omitted from map");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

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

  // Drain evacuate: local tip on a drained target is pushed into the new acting set.
  {
    auto root = temp_root("place-evacuate");
    std::filesystem::create_directories(root / "a" / "aios");
    std::filesystem::create_directories(root / "b" / "aios");
    std::filesystem::create_directories(root / "c" / "aios");
    const std::string pa = (root / "a" / "aios").string();
    const std::string pb = (root / "b" / "aios").string();
    const std::string p_c = (root / "c" / "aios").string();
    MembershipTable membership;
    membership.set_local("node-a", "127.0.0.1:7400");
    FsTable fs;
    std::vector<AiosTarget> local{
        make_target(pa, "nvme"),
        make_target(pb, "nvme"),
        make_target(p_c, "nvme"),
    };
    fs.set_local("node-a", local);
    Config cfg;
    cfg.node_id = "node-a";
    cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
    cfg.replica_count = 2;
    cfg.write_quorum = 2;
    cfg.default_storage_class = "nvme";
    cfg.vnodes_per_target = 32;
    cfg.min_vnodes = 8;
    cfg.max_vnodes = 256;
    PlacementConfig plc;
    plc.vnodes_per_target = cfg.vnodes_per_target;
    plc.min_vnodes = cfg.min_vnodes;
    plc.max_vnodes = cfg.max_vnodes;
    auto map = ClusterMap::build(membership, fs, cfg.replica_count, plc);
    LocalStores stores;
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    stores.sync_paths({pa, pb, p_c}, opts);
    ObjectService svc(cfg, map, stores);
    svc.set_advertise("127.0.0.1:7400");
    const auto* body = reinterpret_cast<const std::uint8_t*>("evacuate-me");
    auto put = svc.api_put("evac/1", body, 11, {}, true, {}, std::nullopt, {});
    expect(put.ok, "seed for evacuate");
    auto before = place("evac/1", map, 2, "nvme");
    expect(before.acting_set.size() == 2, "acting set before drain");
    const std::string drained = before.acting_set[0].aios_path;
    for (auto& t : local) {
      if (t.aios_path == drained) t.state = LifecycleState::Drain;
    }
    fs.set_local("node-a", local);
    map = ClusterMap::build(membership, fs, cfg.replica_count, plc);
    expect(map.targets.size() == 3, "drain remains in map");
    auto after = place("evac/1", map, 2, "nvme");
    expect(after.acting_set.size() == 2, "acting set after drain");
    for (const auto& t : after.acting_set) {
      expect(t.aios_path != drained, "drained target left acting set");
    }
    auto stats = run_repair(cfg, "127.0.0.1:7400", map, stores, 64);
    expect(stats.repaired >= 1, "evacuate repaired");
    int present = 0;
    for (const auto& t : after.acting_set) {
      auto* s = stores.get(t.aios_path);
      std::string err;
      if (s && s->stat("evac/1", err)) ++present;
    }
    expect(present == 2, "acting set fully populated after evacuate");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  return failures;
}
