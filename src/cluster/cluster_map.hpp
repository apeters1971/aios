#pragma once

#include "fs/fs_table.hpp"
#include "membership.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aios {

// One usable object-storage target on an alive node (placement OSD).
struct StorageTarget {
  std::string node_id;
  std::string addr;       // host:port for dialing the owning node (TCP++)
  std::string http_addr;  // host:port HTTP API (may be empty)
  std::string aios_path;  // absolute .../aios path on that node
  std::string mount;
  std::uint64_t bavail{0};
};

// Snapshot of placement-relevant cluster state. Epoch is a content hash of the
// sorted target list + replica_count so peers with the same view share an epoch.
struct ClusterMap {
  std::uint64_t epoch{0};
  int replica_count{3};
  std::vector<StorageTarget> targets;  // sorted by (node_id, aios_path)

  bool empty() const { return targets.empty(); }

  nlohmann::json to_json() const;
  static ClusterMap from_json(const nlohmann::json& j);

  // Build from current membership + fs_table. Only Alive members and usable
  // FsEntry rows are included.
  static ClusterMap build(const MembershipTable& membership, const FsTable& fs_table,
                          int replica_count);
};

// Stable identity string for a target (node_id + path).
std::string target_key(const StorageTarget& t);

}  // namespace aios
