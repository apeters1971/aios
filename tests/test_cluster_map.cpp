#include "cluster/cluster_map.hpp"
#include <gtest/gtest.h>
#include "cluster/place.hpp"
#include "fs/aios_scan.hpp"
#include "fs/fs_table.hpp"
#include "membership.hpp"
#include "util/base64.hpp"

#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>


TEST(ClusterMap, Build) {
  using namespace aios;

  {
    const std::string raw("hello world");
    auto enc = base64_encode(raw);
    std::vector<std::uint8_t> dec;
    std::string err;
    EXPECT_TRUE(base64_decode(enc, dec, err)) << "base64 decode";
    EXPECT_TRUE(std::string(dec.begin(), dec.end()) == raw) << "base64 roundtrip";
  }

  {
    EXPECT_TRUE(member_state_from_string("online") == MemberState::Online) << "online";
    EXPECT_TRUE(member_state_from_string("alive") == MemberState::Online) << "legacy alive";
    EXPECT_TRUE(member_state_from_string("suspect") == MemberState::Suspect) << "suspect";
    EXPECT_TRUE(member_state_from_string("offline") == MemberState::Offline) << "offline";
    EXPECT_TRUE(member_state_from_string("dead") == MemberState::Offline) << "legacy dead";
    EXPECT_TRUE(std::string(member_state_name(MemberState::Online)) == "online") << "name online";
    EXPECT_TRUE(std::string(member_state_name(MemberState::Offline)) == "offline") << "name offline";
  }

  MembershipTable membership;
  membership.set_local("node-a", "127.0.0.1:7400");
  membership.mark_alive("node-b", "127.0.0.1:7401", 1000);

  FsTable fs;
  std::vector<AiosTarget> local;
  PlacementConfig pc;
  pc.vnodes_per_target = 32;
  pc.min_vnodes = 8;
  pc.max_vnodes = 256;

  {
    AiosTarget t;
    t.mount = "/m1";
    t.target_path = "/m1";
    t.aios_path = "/m1/aios";
    t.storage_class = "nvme";
    t.usable = true;
    t.bavail = 100;
    local.push_back(t);
  }
  {
    AiosTarget t;
    t.mount = "/m2";
    t.target_path = "/m2";
    t.aios_path = "/m2/aios";
    t.storage_class = "nvme";
    t.usable = true;
    t.bavail = 200;
    local.push_back(t);
  }
  fs.set_local("node-a", local);

  std::vector<FsEntry> remote = {{
      .node_id = "node-b",
      .mount = "/d",
      .target_path = "/d",
      .aios_path = "/d/aios",
      .storage_class = "nvme",
      .weight = 1,
      .bavail = 50,
      .usable = true,
      .updated_ms = 1000,
  }};
  fs.merge(remote);

  auto map = ClusterMap::build(membership, fs, 3, pc);
  EXPECT_TRUE(map.targets.size() == 3) << "three usable targets";
  EXPECT_TRUE(map.replica_count == 3) << "replica_count";
  EXPECT_TRUE(map.epoch != 0) << "non-zero epoch";

  auto map2 = ClusterMap::build(membership, fs, 3, pc);
  EXPECT_TRUE(map.epoch == map2.epoch) << "epoch stable";

  auto map_r2 = ClusterMap::build(membership, fs, 2, pc);
  EXPECT_TRUE(map_r2.epoch != map.epoch) << "epoch changes with replica_count";

  auto back = ClusterMap::from_json(map.to_json());
  EXPECT_TRUE(back.epoch == map.epoch) << "json epoch";
  EXPECT_TRUE(back.targets.size() == map.targets.size()) << "json targets";
  EXPECT_TRUE(back.targets[0].storage_class == "nvme") << "json storage_class";

  // Age node-b past dead_after → excluded from map.
  membership.age(1000 + 20000, 5000, 15000);
  auto map_dead = ClusterMap::build(membership, fs, 3, pc);
  EXPECT_TRUE(map_dead.targets.size() == 2) << "offline node targets excluded";

  // Rebuild fresh map with both online for place tests.
  membership.mark_alive("node-b", "127.0.0.1:7401", 50000);
  map = ClusterMap::build(membership, fs, 3, pc);

  auto p = place("obj-1", map, "nvme");
  EXPECT_TRUE(p.acting_set.size() == 3) << "acting set size 3";
  EXPECT_TRUE(p.storage_class == "nvme") << "placement class";
  EXPECT_TRUE(p.epoch == map.epoch) << "placement epoch";
  EXPECT_TRUE(is_primary_for("obj-1", map, "nvme", p.acting_set[0].node_id, p.acting_set[0].aios_path)) << "primary helper";
  EXPECT_TRUE(in_acting_set("obj-1", map, "nvme", p.acting_set[1].node_id, p.acting_set[1].aios_path)) << "replica in set";

  std::unordered_set<std::string> nodes;
  for (const auto& t : p.acting_set) nodes.insert(t.node_id);
  EXPECT_TRUE(nodes.size() >= 2) << "at least two nodes in acting set";

  auto p2 = place("obj-1", map, "nvme");
  EXPECT_TRUE(p.acting_set[0].aios_path == p2.acting_set[0].aios_path) << "place stable";

  EXPECT_TRUE(!place("obj-other", map, "nvme").acting_set.empty()) << "other oid placed";
  EXPECT_TRUE(place("obj-1", map, "hdd").acting_set.empty()) << "empty other class";

  // Consistent hashing: adding a target remaps only a fraction of oids.
  {
    AiosTarget t;
    t.mount = "/m3";
    t.target_path = "/m3";
    t.aios_path = "/m3/aios";
    t.storage_class = "nvme";
    t.usable = true;
    t.bavail = 100;
    local.push_back(t);
    fs.set_local("node-a", local);
    auto map3 = ClusterMap::build(membership, fs, 3, pc);
    EXPECT_TRUE(map3.targets.size() == 4) << "four targets after add";
    int moved = 0;
    const int N = 200;
    for (int i = 0; i < N; ++i) {
      const std::string oid = "ch-" + std::to_string(i);
      auto a = place(oid, map, 3, "nvme");
      auto b = place(oid, map3, 3, "nvme");
      if (a.acting_set.empty() || b.acting_set.empty()) continue;
      if (a.acting_set[0].aios_path != b.acting_set[0].aios_path) ++moved;
    }
    EXPECT_TRUE(moved > 0) << "some oids remapped on join";
    EXPECT_TRUE(moved < N / 2) << "fewer than half remapped (consistent hash)";
  }

  }
