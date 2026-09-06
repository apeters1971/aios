#pragma once

#include "cluster/place.hpp"
#include "store/object_store.hpp"

#include <string>
#include <vector>

namespace aios {

inline std::vector<std::string> acting_target_keys(const Placement& p) {
  std::vector<std::string> keys;
  keys.reserve(p.acting_set.size());
  for (const auto& t : p.acting_set) keys.push_back(target_key(t));
  return keys;
}

// Persist the acting set on a local tip so repair can skip unchanged objects
// and query oids that shared a departed target.
void write_object_placement(ObjectStore* store, const std::string& oid, const Placement& p,
                            bool verified);

}  // namespace aios
