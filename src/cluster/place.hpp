#pragma once

#include "cluster/cluster_map.hpp"

#include <string>
#include <vector>

namespace aios {

// Ordered acting set for an object. acting_set[0] is the primary.
struct Placement {
  std::uint64_t epoch{0};
  std::string storage_class;
  std::vector<StorageTarget> acting_set;
  // Failure domains actually covered by acting_set. A value below acting_set.size()
  // means the class ran out of domains and copies (or EC shards) share hardware, so
  // losing one node takes out more than one of them.
  std::size_t distinct_nodes{0};
  std::size_t distinct_racks{0};
};

// Consistent-hash placement with virtual nodes on the class-scoped ring.
// Walks the vnode ring clockwise from sha256(oid), preferring distinct racks,
// then distinct node_ids, then any unused mounts. Only LifecycleState::Up
// targets enter the ring. Empty acting set if the class has fewer than `n`
// up targets.
Placement place(const std::string& oid, const ClusterMap& map, int n,
                const std::string& storage_class);

// place(oid, map, storage_class) == place(oid, map, map.replica_count, storage_class).
Placement place(const std::string& oid, const ClusterMap& map,
                const std::string& storage_class);

bool is_primary_for(const std::string& oid, const ClusterMap& map,
                    const std::string& storage_class, const std::string& node_id,
                    const std::string& aios_path);

bool in_acting_set(const std::string& oid, const ClusterMap& map,
                   const std::string& storage_class, const std::string& node_id,
                   const std::string& aios_path);

}  // namespace aios
