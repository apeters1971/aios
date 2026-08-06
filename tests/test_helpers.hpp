#pragma once

#include "cluster/cluster_map.hpp"
#include "config.hpp"
#include "fs/aios_scan.hpp"
#include "fs/fs_table.hpp"
#include "membership.hpp"
#include "object/object_service.hpp"
#include "store/local_stores.hpp"
#include "store/object_store.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace aios::test {

inline int& failures() {
  static int f = 0;
  return f;
}

inline void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures();
  }
}

inline void expect(bool cond, const std::string& msg) { expect(cond, msg.c_str()); }

inline std::filesystem::path temp_root(const char* prefix) {
  auto p = std::filesystem::temp_directory_path() /
           (std::string(prefix) + "-" + std::to_string(::getpid()));
  std::filesystem::remove_all(p);
  std::filesystem::create_directories(p);
  return p;
}

inline AiosTarget make_target(const std::string& path, const std::string& storage_class = "nvme",
                              int weight = 1, LifecycleState state = LifecycleState::Up,
                              const std::string& rack = {}) {
  AiosTarget t;
  t.mount = path;
  t.target_path = path;
  t.aios_path = path;
  t.storage_class = storage_class;
  t.weight = weight;
  t.state = state;
  if (!rack.empty()) {
    t.rack = rack;
    t.rack_explicit = true;
  }
  t.usable = true;
  t.bavail = 1000;
  return t;
}

struct DualStoreFixture {
  std::filesystem::path root;
  std::string p1;
  std::string p2;
  MembershipTable membership;
  FsTable fs_table;
  Config cfg;
  ClusterMap map;
  LocalStores stores;
  std::unique_ptr<ObjectService> svc;

  DualStoreFixture(const char* prefix, int replica_count = 2, int write_quorum = 2,
                   const std::string& storage_class = "nvme") {
    root = temp_root(prefix);
    std::filesystem::create_directories(root / "t1" / "aios");
    std::filesystem::create_directories(root / "t2" / "aios");
    p1 = (root / "t1" / "aios").string();
    p2 = (root / "t2" / "aios").string();

    membership.set_local("node-a", "127.0.0.1:7400");
    std::vector<AiosTarget> local{make_target(p1, storage_class),
                                  make_target(p2, storage_class)};
    fs_table.set_local("node-a", local);

    cfg.node_id = "node-a";
    cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
    cfg.replica_count = replica_count;
    cfg.write_quorum = write_quorum;
    cfg.max_versions = 16;
    cfg.clone_required = false;
    cfg.default_storage_class = storage_class;
    cfg.vnodes_per_target = 32;
    cfg.min_vnodes = 8;
    cfg.max_vnodes = 256;

    PlacementConfig pc;
    pc.vnodes_per_target = cfg.vnodes_per_target;
    pc.min_vnodes = cfg.min_vnodes;
    pc.max_vnodes = cfg.max_vnodes;
    map = ClusterMap::build(membership, fs_table, cfg.replica_count, pc);
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    stores.sync_paths({p1, p2}, opts);
    svc = std::make_unique<ObjectService>(cfg, map, stores);
    svc->set_advertise("127.0.0.1:7400");
  }

  ~DualStoreFixture() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

inline ObjectStoreOptions default_opts() {
  ObjectStoreOptions o;
  o.shard_count = 4;
  o.inline_max_bytes = 256;
  o.max_versions = 8;
  o.clone_required = false;
  return o;
}

}  // namespace aios::test
