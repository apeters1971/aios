#pragma once

#include "cluster/cluster_map.hpp"
#include "config.hpp"
#include "store/local_stores.hpp"

#include <cstddef>
#include <string>

namespace aios {

struct TransitionStats {
  std::size_t oids_scanned{0};
  std::size_t matched{0};
  std::size_t migrated{0};
  std::size_t failed{0};
  std::size_t drained{0};
};

// Scan local tips; when this node is primary for the destination class of a
// matching transition_rule, copy/re-encode the tip onto the destination ring
// (new version) and mark transition attrs. A later pass clears
// storage_class_prev once the destination acting set is complete.
TransitionStats run_transitions(const Config& cfg, const std::string& advertise,
                                const ClusterMap& map, LocalStores& stores,
                                std::size_t max_oids_per_store);

}  // namespace aios
