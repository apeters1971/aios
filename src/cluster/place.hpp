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
// target ring, then walks preferring distinct node_ids until replica_count
// targets are chosen (falling back to same-node mounts if needed).
Placement place(const std::string& oid, const ClusterMap& map);

// True if `candidate` is acting_set[0] under the given map/oid.
bool is_primary_for(const std::string& oid, const ClusterMap& map,
                    const std::string& node_id, const std::string& aios_path);

// True if candidate appears anywhere in the acting set.
bool in_acting_set(const std::string& oid, const ClusterMap& map,
                   const std::string& node_id, const std::string& aios_path);

}  // namespace aios
