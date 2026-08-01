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
  {
    AiosTarget t;
    t.mount = "/m1";
    t.target_path = "/m1";
    t.aios_path = "/m1/aios";
    t.usable = true;
    t.bavail = 100;
    local.push_back(t);
  }
  {
    AiosTarget t;
    t.mount = "/m2";
    t.target_path = "/m2";
    t.aios_path = "/m2/aios";
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
      .bavail = 50,
      .usable = true,
      .updated_ms = 1000,
  }};
  fs.merge(remote);

  auto map = ClusterMap::build(membership, fs, 3);
  expect(map.targets.size() == 3, "three usable targets");
  expect(map.replica_count == 3, "replica_count");
  expect(map.epoch != 0, "non-zero epoch");

  auto map2 = ClusterMap::build(membership, fs, 3);
  expect(map.epoch == map2.epoch, "epoch stable");

  auto map_r2 = ClusterMap::build(membership, fs, 2);
  expect(map_r2.epoch != map.epoch, "epoch changes with replica_count");

  auto back = ClusterMap::from_json(map.to_json());
  expect(back.epoch == map.epoch, "json epoch");
  expect(back.targets.size() == map.targets.size(), "json targets");

  // Age node-b past dead_after → excluded from map.
  membership.age(1000 + 20000, 5000, 15000);
  auto map_dead = ClusterMap::build(membership, fs, 3);
  expect(map_dead.targets.size() == 2, "dead node targets excluded");

  // Rebuild fresh map with both alive for place tests.
  membership.mark_alive("node-b", "127.0.0.1:7401", 50000);
  map = ClusterMap::build(membership, fs, 3);

  auto p = place("obj-1", map);
  expect(p.acting_set.size() == 3, "acting set size 3");
  expect(p.epoch == map.epoch, "placement epoch");
  expect(is_primary_for("obj-1", map, p.acting_set[0].node_id, p.acting_set[0].aios_path),
         "primary helper");
  expect(in_acting_set("obj-1", map, p.acting_set[1].node_id, p.acting_set[1].aios_path),
         "replica in set");

  std::unordered_set<std::string> nodes;
  for (const auto& t : p.acting_set) nodes.insert(t.node_id);
  expect(nodes.size() >= 2, "at least two nodes in acting set");

  auto p2 = place("obj-1", map);
  expect(p.acting_set[0].aios_path == p2.acting_set[0].aios_path, "place stable");

  expect(!place("obj-other", map).acting_set.empty(), "other oid placed");

  return failures;
}
