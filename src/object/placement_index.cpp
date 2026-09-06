#include "object/placement_index.hpp"

#include "util/log.hpp"

namespace aios {

void write_object_placement(ObjectStore* store, const std::string& oid, const Placement& p,
                            bool verified) {
  if (!store || oid.empty()) return;
  std::string err;
  if (!store->set_placement(oid, acting_target_keys(p), verified, err) && !err.empty()) {
    AIOS_LOG_WARN("set_placement ", oid, ": ", err);
  }
}

}  // namespace aios
