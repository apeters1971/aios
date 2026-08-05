#include "cluster/cluster_map.hpp"
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

namespace {

int failures = 0;
void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL compression: " << msg << "\n";
    ++failures;
  }
}

}  // namespace

int test_compression() {
  using namespace aios;
  failures = 0;

  std::string cerr;
  std::vector<std::uint8_t> tiny{1, 2, 3};
  std::vector<std::uint8_t> out;
  if (!zstd_available()) {
    expect(!zstd_compress(tiny.data(), tiny.size(), 3, out, cerr), "compress unavailable");
    std::cout << "test_compression SKIP (no libzstd)\n";
    return 0;
  }

  // Unit: round-trip compressible payload.
  std::vector<std::uint8_t> plain(4096, 'A');
  expect(zstd_compress(plain.data(), plain.size(), 3, out, cerr), "zstd_compress");
  expect(out.size() < plain.size(), "compress shrinks");
  std::vector<std::uint8_t> back;
  expect(zstd_decompress(out.data(), out.size(), plain.size(), back, cerr), "zstd_decompress");
  expect(back == plain, "round-trip");

  // Ops ratio accounting.
  OpsRegistry ops;
  ops.note_compress(1000, 250);
  auto admin = ops.to_admin_json();
  expect(admin.contains("compression"), "compression block");
  expect(admin["compression"].value("ratio", 0.0) == 4.0, "ratio 4x");
  expect(admin["compression"].value("logical_bytes", 0ull) == 1000, "logical");
  expect(admin["compression"].value("stored_bytes", 0ull) == 250, "stored");

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
  expect(put.ok, "api_put compressed");
  expect(put.info && put.info->size == plain.size(), "put logical size");
  expect(attrs_are_compressed(put.attrs), "compression attr");
  expect(svc.ops().total().compress_puts.load() >= 1, "compress_puts");

  auto got = svc.api_get(oid, std::nullopt, std::nullopt, {});
  expect(got.ok && got.data && *got.data == plain, "get decompress");
  expect(got.info && got.info->size == plain.size(), "get logical size");

  auto ranged = svc.api_get(oid, 10, 19, {});
  expect(ranged.ok && ranged.data && ranged.data->size() == 10, "logical range len");
  expect(std::string(ranged.data->begin(), ranged.data->end()) == std::string(10, 'A'),
         "logical range bytes");

  auto pr = svc.api_put_range(oid, 0, reinterpret_cast<const std::uint8_t*>("x"), 1, {}, false,
                              {});
  expect(!pr.ok && pr.code == "bad_request", "put_range rejected");

  auto ap = svc.api_append(oid, reinterpret_cast<const std::uint8_t*>("y"), 1, {}, false, {});
  expect(!ap.ok && ap.code == "bad_request", "append rejected");

  const auto ratio = svc.ops().to_admin_json()["compression"].value("ratio", 0.0);
  expect(ratio > 1.0, "overall ratio > 1");

  fs::remove_all(root);
  if (failures == 0) std::cout << "test_compression OK\n";
  return failures;
}
