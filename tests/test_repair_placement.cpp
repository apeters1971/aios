#include "cluster/place.hpp"
#include "object/placement_index.hpp"
#include "object/repair.hpp"
#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

using namespace aios;
using namespace aios::test;

TEST(RepairPlan, UnchangedMapIsUnverifiedUnlessScrub) {
  ClusterMap m;
  m.replica_count = 2;
  StorageTarget t;
  t.node_id = "n1";
  t.addr = "127.0.0.1:1";
  t.aios_path = "/n1/aios";
  t.mount = "/n1/aios";
  t.storage_class = "nvme";
  t.weight = 1;
  t.state = LifecycleState::Up;
  m.targets = {t};
  m.epoch = m.content_hash();

  auto skip = plan_repair(&m, m, false);
  EXPECT_EQ(skip.select, RepairSelect::Unverified);
  EXPECT_FALSE(skip.scrub);

  auto scrub = plan_repair(&m, m, true);
  EXPECT_EQ(scrub.select, RepairSelect::All);
  EXPECT_TRUE(scrub.scrub);

  auto first = plan_repair(nullptr, m, false);
  EXPECT_EQ(first.select, RepairSelect::All);
}

TEST(RepairPlan, DepartedUpTargetIsTargeted) {
  auto mk = [](const char* node, const char* path) {
    StorageTarget t;
    t.node_id = node;
    t.addr = "127.0.0.1:1";
    t.aios_path = path;
    t.mount = path;
    t.storage_class = "nvme";
    t.weight = 1;
    t.state = LifecycleState::Up;
    return t;
  };
  ClusterMap prev;
  prev.replica_count = 2;
  prev.targets = {mk("n1", "/a"), mk("n2", "/b"), mk("n3", "/c")};
  ClusterMap now = prev;
  now.targets[2].state = LifecycleState::Drain;
  auto hint = plan_repair(&prev, now, false);
  EXPECT_EQ(hint.select, RepairSelect::Departed);
  ASSERT_EQ(hint.departed_keys.size(), 1u);
  EXPECT_EQ(hint.departed_keys[0], target_key(prev.targets[2]));
}

TEST(RepairPlan, AddedTargetIsFullScan) {
  auto mk = [](const char* node, const char* path) {
    StorageTarget t;
    t.node_id = node;
    t.addr = "127.0.0.1:1";
    t.aios_path = path;
    t.mount = path;
    t.storage_class = "nvme";
    t.weight = 1;
    t.state = LifecycleState::Up;
    return t;
  };
  ClusterMap prev;
  prev.replica_count = 2;
  prev.targets = {mk("n1", "/a"), mk("n2", "/b")};
  ClusterMap now = prev;
  now.targets.push_back(mk("n3", "/c"));
  auto hint = plan_repair(&prev, now, false);
  EXPECT_EQ(hint.select, RepairSelect::All);
}

TEST(RepairPlacement, PutRecordsActingSetAndSecondPassSkipsStats) {
  DualStoreFixture fx("repair-skip", 2, 2, "nvme");
  const auto* body = reinterpret_cast<const std::uint8_t*>("skip-me");
  auto put = fx.svc->api_put("skip/1", body, 7, {}, true, {}, std::nullopt, {});
  ASSERT_TRUE(put.ok) << put.error;
  auto* store = fx.stores.get(fx.p1);
  ASSERT_TRUE(store);
  std::string err;
  auto pl = store->get_placement("skip/1", err);
  if (!pl) {
    store = fx.stores.get(fx.p2);
    ASSERT_TRUE(store);
    pl = store->get_placement("skip/1", err);
  }
  ASSERT_TRUE(pl.has_value()) << err;
  EXPECT_EQ(pl->target_keys.size(), 2u);
  EXPECT_FALSE(pl->verified);

  auto first = run_repair(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 64);
  EXPECT_GE(first.oids_stated, 1u);
  auto second = run_repair(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 64);
  EXPECT_EQ(second.oids_stated, 0u);
  EXPECT_GE(second.oids_skipped, 1u);
}

TEST(RepairPlacement, DeadTargetQueryDoesNotWalkUnrelatedOids) {
  auto root = temp_root("repair-departed");
  std::vector<std::string> paths;
  for (const char* d : {"a", "b", "c", "d"}) {
    std::filesystem::create_directories(root / d / "aios");
    paths.push_back((root / d / "aios").string());
  }
  MembershipTable membership;
  membership.set_local("node-a", "127.0.0.1:7400");
  FsTable fs;
  std::vector<AiosTarget> local;
  for (const auto& p : paths) local.push_back(make_target(p));
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
  PlacementConfig pc;
  pc.vnodes_per_target = cfg.vnodes_per_target;
  pc.min_vnodes = cfg.min_vnodes;
  pc.max_vnodes = cfg.max_vnodes;
  auto map = ClusterMap::build(membership, fs, cfg.replica_count, pc);
  LocalStores stores;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.clone_required = false;
  opts.max_versions = 16;
  stores.sync_paths(paths, opts);
  ObjectService svc(cfg, map, stores);
  svc.set_advertise("127.0.0.1:7400");

  const auto* body = reinterpret_cast<const std::uint8_t*>("x");
  constexpr int N = 40;
  for (int i = 0; i < N; ++i) {
    auto put = svc.api_put("dep/" + std::to_string(i), body, 1, {}, true, {}, std::nullopt, {});
    ASSERT_TRUE(put.ok) << put.error;
  }

  const std::string dead = paths.back();
  const std::string dead_key = "node-a\n" + dead;
  std::unordered_set<std::string> affected;
  for (int i = 0; i < N; ++i) {
    const std::string oid = "dep/" + std::to_string(i);
    for (const auto& p : paths) {
      auto* s = stores.get(p);
      std::string err;
      auto pl = s ? s->get_placement(oid, err) : std::nullopt;
      if (!pl) continue;
      for (const auto& k : pl->target_keys) {
        if (k == dead_key) affected.insert(oid);
      }
    }
  }
  ASSERT_FALSE(affected.empty());
  ASSERT_LT(affected.size(), static_cast<std::size_t>(N));

  local.pop_back();
  fs.set_local("node-a", local);
  auto now = ClusterMap::build(membership, fs, cfg.replica_count, pc);
  auto hint = plan_repair(&map, now, false);
  ASSERT_EQ(hint.select, RepairSelect::Departed);
  ASSERT_EQ(hint.departed_keys.size(), 1u);

  auto stats = run_repair(cfg, "127.0.0.1:7400", now, stores, 256, hint);
  EXPECT_GE(stats.oids_scanned, affected.size());
  EXPECT_LE(stats.oids_scanned, affected.size() * 3);
  EXPECT_LT(stats.oids_scanned, static_cast<std::size_t>(N) * 2);

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}
