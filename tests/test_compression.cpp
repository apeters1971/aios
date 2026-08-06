#include "cluster/cluster_map.hpp"
#include <gtest/gtest.h>
#include "config.hpp"
#include "fs/aios_scan.hpp"
#include "fs/fs_table.hpp"
#include "membership.hpp"
#include "object/object_service.hpp"
#include "store/local_stores.hpp"
#include "util/compression.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;


TEST(Compression, Basic) {
  using namespace aios;
  std::string cerr;
  std::vector<std::uint8_t> tiny{1, 2, 3};
  std::vector<std::uint8_t> out;
  if (!zstd_available()) {
    EXPECT_TRUE(!zstd_compress(tiny.data(), tiny.size(), 3, out, cerr)) << "compress unavailable";
    GTEST_SKIP() << "test_compression SKIP (no libzstd)\n";
  }

  // Unit: round-trip compressible payload.
  std::vector<std::uint8_t> plain(4096, 'A');
  EXPECT_TRUE(zstd_compress(plain.data(), plain.size(), 3, out, cerr)) << "zstd_compress";
  EXPECT_TRUE(out.size() < plain.size()) << "compress shrinks";
  std::vector<std::uint8_t> back;
  EXPECT_TRUE(zstd_decompress(out.data(), out.size(), plain.size(), back, cerr)) << "zstd_decompress";
  EXPECT_TRUE(back == plain) << "round-trip";

  // Ops ratio accounting.
  OpsRegistry ops;
  ops.note_compress(1000, 250);
  auto admin = ops.to_admin_json();
  EXPECT_TRUE(admin.contains("compression")) << "compression block";
  EXPECT_TRUE(admin["compression"].value("ratio", 0.0) == 4.0) << "ratio 4x";
  EXPECT_TRUE(admin["compression"].value("logical_bytes", 0ull) == 1000) << "logical";
  EXPECT_TRUE(admin["compression"].value("stored_bytes", 0ull) == 250) << "stored";

  // ObjectService: put/get with compression=zstd.
  const auto root = fs::temp_directory_path() / ("aios-comp-" + std::to_string(::getpid()));
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
  cfg.compression = "zstd";
  cfg.compression_level = 3;
  cfg.compression_min_bytes = 64;

  ClusterMap map = ClusterMap::build(membership, fs_table, cfg.replica_count, PlacementConfig{});
  LocalStores stores;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  stores.sync_paths({p1, p2}, opts);

  ObjectService svc(cfg, map, stores);
  svc.set_advertise("127.0.0.1:7400");

  const std::string oid = "comp/blob";
  auto put = svc.api_put(oid, plain.data(), plain.size(), {}, true, {});
  EXPECT_TRUE(put.ok) << "api_put compressed";
  EXPECT_TRUE(put.info && put.info->size == plain.size()) << "put logical size";
  EXPECT_TRUE(attrs_are_compressed(put.attrs)) << "compression attr";
  EXPECT_TRUE(svc.ops().total().compress_puts.load() >= 1) << "compress_puts";

  auto got = svc.api_get(oid, std::nullopt, std::nullopt, {});
  EXPECT_TRUE(got.ok && got.data && *got.data == plain) << "get decompress";
  EXPECT_TRUE(got.info && got.info->size == plain.size()) << "get logical size";

  auto ranged = svc.api_get(oid, 10, 19, {});
  EXPECT_TRUE(ranged.ok && ranged.data && ranged.data->size() == 10) << "logical range len";
  EXPECT_TRUE(std::string(ranged.data->begin(), ranged.data->end()) == std::string(10, 'A')) << "logical range bytes";

  auto pr = svc.api_put_range(oid, 0, reinterpret_cast<const std::uint8_t*>("x"), 1, {}, false,
                              {});
  EXPECT_TRUE(!pr.ok && pr.code == "bad_request") << "put_range rejected";

  auto ap = svc.api_append(oid, reinterpret_cast<const std::uint8_t*>("y"), 1, {}, false, {});
  EXPECT_TRUE(!ap.ok && ap.code == "bad_request") << "append rejected";

  const auto ratio = svc.ops().to_admin_json()["compression"].value("ratio", 0.0);
  EXPECT_TRUE(ratio > 1.0) << "overall ratio > 1";

  fs::remove_all(root);
  }
