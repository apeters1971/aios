#pragma once

#include "cluster/cluster_map.hpp"

#include <string>
#include <vector>

namespace aios {

// Ordered acting set for an object. acting_set[0] is the primary.
struct Placement {
  std::uint64_t epoch{0};
  std::vector<StorageTarget> acting_set;
};

// Deterministic placement: SHA-256(oid) picks a start index into the sorted
// target ring, then walks preferring distinct node_ids until `n` targets are
// chosen (falling back to same-node mounts if needed).
// If n > map.targets.size() (or n < 1), returns an empty acting set — callers
// treat that as no_targets (no silent under-protection).
Placement place(const std::string& oid, const ClusterMap& map, int n);

// Compat: place(oid, map) == place(oid, map, map.replica_count).
Placement place(const std::string& oid, const ClusterMap& map);

// True if `candidate` is acting_set[0] under the given map/oid.
bool is_primary_for(const std::string& oid, const ClusterMap& map,
                    const std::string& node_id, const std::string& aios_path);

// True if candidate appears anywhere in the acting set.
bool in_acting_set(const std::string& oid, const ClusterMap& map,
                   const std::string& node_id, const std::string& aios_path);

}  // namespace aios
