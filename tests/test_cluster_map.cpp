#include "cluster/cluster_map.hpp"
#include "cluster/place.hpp"
#include "fs/aios_scan.hpp"
#include "fs/fs_table.hpp"
#include "membership.hpp"
#include "util/base64.hpp"

#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
}

}  // namespace

int test_cluster_map() {
  using namespace aios;

  {
    const std::string raw("hello world");
    auto enc = base64_encode(raw);
    std::vector<std::uint8_t> dec;
    std::string err;
    expect(base64_decode(enc, dec, err), "base64 decode");
    expect(std::string(dec.begin(), dec.end()) == raw, "base64 roundtrip");
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
  expect(map.targets.size() == 3, "three usable targets");
  expect(map.replica_count == 3, "replica_count");
  expect(map.epoch != 0, "non-zero epoch");

  auto map2 = ClusterMap::build(membership, fs, 3, pc);
  expect(map.epoch == map2.epoch, "epoch stable");

  auto map_r2 = ClusterMap::build(membership, fs, 2, pc);
  expect(map_r2.epoch != map.epoch, "epoch changes with replica_count");

  auto back = ClusterMap::from_json(map.to_json());
  expect(back.epoch == map.epoch, "json epoch");
  expect(back.targets.size() == map.targets.size(), "json targets");
  expect(back.targets[0].storage_class == "nvme", "json storage_class");

  // Age node-b past dead_after → excluded from map.
  membership.age(1000 + 20000, 5000, 15000);
  auto map_dead = ClusterMap::build(membership, fs, 3, pc);
  expect(map_dead.targets.size() == 2, "dead node targets excluded");

  // Rebuild fresh map with both alive for place tests.
  membership.mark_alive("node-b", "127.0.0.1:7401", 50000);
  map = ClusterMap::build(membership, fs, 3, pc);

  auto p = place("obj-1", map, "nvme");
  expect(p.acting_set.size() == 3, "acting set size 3");
  expect(p.storage_class == "nvme", "placement class");
  expect(p.epoch == map.epoch, "placement epoch");
  expect(is_primary_for("obj-1", map, "nvme", p.acting_set[0].node_id, p.acting_set[0].aios_path),
         "primary helper");
  expect(in_acting_set("obj-1", map, "nvme", p.acting_set[1].node_id, p.acting_set[1].aios_path),
         "replica in set");

  std::unordered_set<std::string> nodes;
  for (const auto& t : p.acting_set) nodes.insert(t.node_id);
  expect(nodes.size() >= 2, "at least two nodes in acting set");

  auto p2 = place("obj-1", map, "nvme");
  expect(p.acting_set[0].aios_path == p2.acting_set[0].aios_path, "place stable");

  expect(!place("obj-other", map, "nvme").acting_set.empty(), "other oid placed");
  expect(place("obj-1", map, "hdd").acting_set.empty(), "empty other class");

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
    expect(map3.targets.size() == 4, "four targets after add");
    int moved = 0;
    const int N = 200;
    for (int i = 0; i < N; ++i) {
      const std::string oid = "ch-" + std::to_string(i);
      auto a = place(oid, map, 3, "nvme");
      auto b = place(oid, map3, 3, "nvme");
      if (a.acting_set.empty() || b.acting_set.empty()) continue;
      if (a.acting_set[0].aios_path != b.acting_set[0].aios_path) ++moved;
    }
    expect(moved > 0, "some oids remapped on join");
    expect(moved < N / 2, "fewer than half remapped (consistent hash)");
  }

  return failures;
}
