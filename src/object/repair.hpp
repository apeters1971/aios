#pragma once

#include "cluster/cluster_map.hpp"
#include "config.hpp"
#include "store/local_stores.hpp"

#include <cstddef>
#include <string>

namespace aios {

struct RepairStats {
  std::size_t oids_scanned{0};
  std::size_t under_replicated{0};
  std::size_t repaired{0};
  std::size_t failed{0};
};

// Scan local objects; when this node holds the primary (or is the
// lexicographically lowest alive acting-set member that has the object),
// push missing copies to under-replicated secondaries.
RepairStats run_repair(const Config& cfg, const std::string& advertise,
                       const ClusterMap& map, LocalStores& stores,
                       std::size_t max_oids_per_store);

}  // namespace aios
