#pragma once

#include "cluster/lifecycle.hpp"
#include "fs/fs_table.hpp"
#include "membership.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aios {

// Virtual-node counts for consistent-hash rings (per storage class).
struct PlacementConfig {
  int vnodes_per_target{128};
  int min_vnodes{16};
  int max_vnodes{1024};
};

// One usable object-storage target on an alive node (placement OSD).
struct StorageTarget {
  std::string node_id;
  std::string addr;       // host:port for dialing the owning node (TCP++)
  std::string http_addr;  // host:port HTTP API (may be empty)
  std::string aios_path;  // absolute .../aios path on that node
  std::string mount;
  std::string storage_class;
  int weight{1};
  LifecycleState state{LifecycleState::Up};  // up or drain (off never appears)
  std::uint64_t bavail{0};
};

// Snapshot of placement-relevant cluster state. Epoch is a content hash of the
// sorted target list + replica_count + placement knobs so peers with the same
// view share an epoch.
struct ClusterMap {
  std::uint64_t epoch{0};
  int replica_count{3};
  PlacementConfig placement{};
  std::vector<StorageTarget> targets;  // sorted by (storage_class, node_id, aios_path)

  bool empty() const { return targets.empty(); }

  // Targets belonging to `storage_class` (preserves map sort order).
  std::vector<StorageTarget> targets_for_class(const std::string& storage_class) const;

  nlohmann::json to_json() const;
  static ClusterMap from_json(const nlohmann::json& j);

  // Build from current membership + fs_table. Only Alive members and usable
  // FsEntry rows with a non-empty storage_class are included.
  static ClusterMap build(const MembershipTable& membership, const FsTable& fs_table,
                          int replica_count, const PlacementConfig& placement = {});
};

// Stable identity string for a target (node_id + path).
std::string target_key(const StorageTarget& t);

// Look up storage_class for a target; empty if unknown.
std::string storage_class_of_target(const ClusterMap& map, const std::string& node_id,
                                    const std::string& aios_path);

}  // namespace aios
