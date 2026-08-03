#include "cluster/cluster_map.hpp"
#include "cluster/place.hpp"
#include "config.hpp"
#include "fs/aios_scan.hpp"
#include "fs/fs_table.hpp"
#include "membership.hpp"
#include "net/framing.hpp"
#include "object/object_service.hpp"
#include "object/repair.hpp"
#include "store/local_stores.hpp"
#include "util/base64.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
}

}  // namespace

int test_object_rpc() {
  using namespace aios;

  const auto root = fs::temp_directory_path() / ("aios-rpc-" + std::to_string(::getpid()));
  fs::create_directories(root / "t1" / "aios");
  fs::create_directories(root / "t2" / "aios");
  const std::string p1 = (root / "t1" / "aios").string();
  const std::string p2 = (root / "t2" / "aios").string();

  MembershipTable membership;
  membership.set_local("node-a", "127.0.0.1:7400");

  FsTable fs_table;
  std::vector<AiosTarget> local;
  for (const auto& path : {p1, p2}) {
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
  cfg.replica_count = 2;
  cfg.write_quorum = 2;

  ClusterMap map = ClusterMap::build(membership, fs_table, cfg.replica_count, PlacementConfig{});
  expect(map.targets.size() == 2, "two local targets");

  LocalStores stores;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  stores.sync_paths({p1, p2}, opts);
  expect(stores.get(p1) != nullptr, "store1 open");
  expect(stores.get(p2) != nullptr, "store2 open");

  ObjectService svc(cfg, map, stores);
  svc.set_advertise("127.0.0.1:7400");

  const std::string oid = "test-oid-1";
  auto placement = place(oid, map, "nvme");
  expect(placement.acting_set.size() == 2, "acting set 2");
  const auto& primary = placement.acting_set[0];

  Frame put;
  put.type = MsgType::ObjectPut;
  put.body = {
      {"epoch", map.epoch},
      {"aios_path", primary.aios_path},
      {"oid", oid},
      {"data_b64", base64_encode(std::string("payload-data"))},
      {"attrs", {{"k", "v"}}},
      {"role", "primary"},
  };
  auto put_reply = svc.handle(put);
  expect(put_reply.type == MsgType::ObjectReply, "put reply type");
  expect(put_reply.body.value("ok", false), "put ok");
  expect(put_reply.body.value("replicas", 0) == 2, "two replicas written");

  // Both stores should have the object.
  std::string err;
  expect(stores.get(p1)->stat(oid, err).has_value() ||
             stores.get(p2)->stat(oid, err).has_value(),
         "object on a store");
  int copies = 0;
  err.clear();
  if (stores.get(p1)->stat(oid, err)) ++copies;
  err.clear();
  if (stores.get(p2)->stat(oid, err)) ++copies;
  expect(copies == 2, "object on both local targets");

  Frame get;
  get.type = MsgType::ObjectGet;
  get.body = {
      {"epoch", map.epoch},
      {"aios_path", primary.aios_path},
      {"oid", oid},
  };
  auto get_reply = svc.handle(get);
  expect(get_reply.body.value("ok", false), "get ok");
  std::vector<std::uint8_t> data;
  expect(base64_decode(get_reply.body["data_b64"].get<std::string>(), data, err),
         "get decode");
  expect(std::string(data.begin(), data.end()) == "payload-data", "get data");

  Frame st;
  st.type = MsgType::ObjectStat;
  st.body = {{"epoch", map.epoch}, {"aios_path", primary.aios_path}, {"oid", oid}};
  auto st_reply = svc.handle(st);
  expect(st_reply.body.value("ok", false), "stat ok");
  expect(st_reply.body.value("size", 0u) == 12, "stat size");

  // Delete one replica to exercise repair.
  expect(stores.get(placement.acting_set[1].aios_path)->del(oid, err), "del secondary");
  auto stats = run_repair(cfg, "127.0.0.1:7400", map, stores, 100);
  expect(stats.under_replicated >= 1, "saw under-replicated");
  expect(stats.repaired >= 1, "repaired");
  copies = 0;
  err.clear();
  if (stores.get(p1)->stat(oid, err)) ++copies;
  err.clear();
  if (stores.get(p2)->stat(oid, err)) ++copies;
  expect(copies == 2, "both copies after repair");

  Frame del;
  del.type = MsgType::ObjectDel;
  del.body = {
      {"epoch", map.epoch},
      {"aios_path", primary.aios_path},
      {"oid", oid},
      {"role", "primary"},
  };
  auto del_reply = svc.handle(del);
  expect(del_reply.body.value("ok", false), "del ok");

  // Epoch mismatch
  Frame bad;
  bad.type = MsgType::ObjectStat;
  bad.body = {{"epoch", map.epoch + 1}, {"aios_path", primary.aios_path}, {"oid", oid}};
  auto bad_reply = svc.handle(bad);
  expect(!bad_reply.body.value("ok", true), "epoch mismatch fails");
  expect(bad_reply.body.value("code", "") == "epoch_mismatch", "epoch code");

  std::error_code ec;
  fs::remove_all(root, ec);
  return failures;
}
