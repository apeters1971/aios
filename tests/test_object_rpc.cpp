#include "test_helpers.hpp"
#include "cluster/cluster_map.hpp"
#include <gtest/gtest.h>
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
#include "util/crc32c.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;


TEST(ObjectRpc, Basic) {
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
  EXPECT_TRUE(map.targets.size() == 2) << "two local targets";

  LocalStores stores;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  stores.sync_paths({p1, p2}, opts);
  EXPECT_TRUE(stores.get(p1) != nullptr) << "store1 open";
  EXPECT_TRUE(stores.get(p2) != nullptr) << "store2 open";

  ObjectService svc(cfg, map, stores);
  svc.set_advertise("127.0.0.1:7400");

  const std::string oid = "test-oid-1";
  auto placement = place(oid, map, "nvme");
  EXPECT_TRUE(placement.acting_set.size() == 2) << "acting set 2";
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
  EXPECT_TRUE(put_reply.type == MsgType::ObjectReply) << "put reply type";
  EXPECT_TRUE(put_reply.body.value("ok", false)) << "put ok";
  EXPECT_TRUE(put_reply.body.value("replicas", 0) == 2) << "two replicas written";

  // Both stores should have the object.
  std::string err;
  EXPECT_TRUE(stores.get(p1)->stat(oid, err).has_value() ||
             stores.get(p2)->stat(oid, err).has_value()) << "object on a store";
  int copies = 0;
  err.clear();
  if (stores.get(p1)->stat(oid, err)) ++copies;
  err.clear();
  if (stores.get(p2)->stat(oid, err)) ++copies;
  EXPECT_TRUE(copies == 2) << "object on both local targets";

  Frame get;
  get.type = MsgType::ObjectGet;
  get.body = {
      {"epoch", map.epoch},
      {"aios_path", primary.aios_path},
      {"oid", oid},
  };
  auto get_reply = svc.handle(get);
  EXPECT_TRUE(get_reply.body.value("ok", false)) << "get ok";
  auto data = aios::test::rpc_payload(get_reply);
  EXPECT_TRUE((get_reply.flags & aios::kFlagRawBody) != 0) << "get uses raw body";
  EXPECT_TRUE(std::string(data.begin(), data.end()) == "payload-data") << "get data";

  Frame raw_put;
  raw_put.type = MsgType::ObjectPut;
  raw_put.flags = kFlagRawBody;
  const std::string raw_payload = "raw-put-payload";
  raw_put.raw.assign(raw_payload.begin(), raw_payload.end());
  raw_put.body = {
      {"epoch", map.epoch},
      {"aios_path", primary.aios_path},
      {"oid", "raw-oid"},
      {"size", raw_payload.size()},
      {"crc32c", crc32c(reinterpret_cast<const std::uint8_t*>(raw_payload.data()),
                        raw_payload.size())},
      {"role", "primary"},
  };
  EXPECT_TRUE(!raw_put.body.contains("data_b64")) << "raw put has no data_b64";
  auto raw_reply = svc.handle(raw_put);
  EXPECT_TRUE(raw_reply.body.value("ok", false)) << "raw put ok";
  Frame raw_get;
  raw_get.type = MsgType::ObjectGet;
  raw_get.body = {{"epoch", map.epoch}, {"aios_path", primary.aios_path}, {"oid", "raw-oid"}};
  auto raw_got = svc.handle(raw_get);
  auto raw_data = aios::test::rpc_payload(raw_got);
  EXPECT_TRUE(raw_got.body.value("ok", false) &&
              std::string(raw_data.begin(), raw_data.end()) == raw_payload)
      << "raw put roundtrip";

  Frame st;
  st.type = MsgType::ObjectStat;
  st.body = {{"epoch", map.epoch}, {"aios_path", primary.aios_path}, {"oid", oid}};
  auto st_reply = svc.handle(st);
  EXPECT_TRUE(st_reply.body.value("ok", false)) << "stat ok";
  EXPECT_TRUE(st_reply.body.value("size", 0u) == 12) << "stat size";

  // Delete one replica to exercise repair.
  EXPECT_TRUE(stores.get(placement.acting_set[1].aios_path)->del(oid, err)) << "del secondary";
  auto stats = run_repair(cfg, "127.0.0.1:7400", map, stores, 100);
  EXPECT_TRUE(stats.under_replicated >= 1) << "saw under-replicated";
  EXPECT_TRUE(stats.repaired >= 1) << "repaired";
  copies = 0;
  err.clear();
  if (stores.get(p1)->stat(oid, err)) ++copies;
  err.clear();
  if (stores.get(p2)->stat(oid, err)) ++copies;
  EXPECT_TRUE(copies == 2) << "both copies after repair";

  Frame del;
  del.type = MsgType::ObjectDel;
  del.body = {
      {"epoch", map.epoch},
      {"aios_path", primary.aios_path},
      {"oid", oid},
      {"role", "primary"},
  };
  auto del_reply = svc.handle(del);
  EXPECT_TRUE(del_reply.body.value("ok", false)) << "del ok";

  // Epoch mismatch
  Frame bad;
  bad.type = MsgType::ObjectStat;
  bad.body = {{"epoch", map.epoch + 1}, {"aios_path", primary.aios_path}, {"oid", oid}};
  auto bad_reply = svc.handle(bad);
  EXPECT_TRUE(!bad_reply.body.value("ok", true)) << "epoch mismatch fails";
  EXPECT_TRUE(bad_reply.body.value("code", "") == "epoch_mismatch") << "epoch code";

  std::error_code ec;
  fs::remove_all(root, ec);
  }
