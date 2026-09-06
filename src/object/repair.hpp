#pragma once

#include "cluster/cluster_map.hpp"
#include "config.hpp"
#include "store/local_stores.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace aios {

struct RepairStats {
  std::size_t oids_scanned{0};
  std::size_t oids_skipped{0};
  std::size_t oids_stated{0};
  std::size_t under_replicated{0};
  std::size_t repaired{0};
  std::size_t failed{0};
};

enum class RepairSelect {
  All,         // every local tip (startup / topology-wide remap / scrub)
  Unverified,  // new writes not yet confirmed on the acting set
  Departed,    // oids whose stored acting set included a target that left the ring
};

struct RepairHint {
  RepairSelect select{RepairSelect::All};
  std::vector<std::string> departed_keys;
  bool scrub{false};  // ignore verified fingerprints and re-stat
};

// Compare consecutive maps. Unchanged content → Unverified (or All+scrub).
// Targets leaving the Up ring with no other topology change → Departed.
// Adds, weight/rack/vnode/replica_count changes → All.
RepairHint plan_repair(const ClusterMap* prev, const ClusterMap& now, bool scrub_due);

// Scan local objects; when this node holds the primary (or is the
// lexicographically lowest alive acting-set member that has the object),
// push missing copies to under-replicated secondaries.
RepairStats run_repair(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                       LocalStores& stores, std::size_t max_oids_per_store,
                       RepairHint hint = {});

}  // namespace aios
