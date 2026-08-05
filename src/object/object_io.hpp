#pragma once

#include "cluster/cluster_map.hpp"
#include "cluster/place.hpp"
#include "config.hpp"
#include "store/local_stores.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

bool load_object_bytes(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                       LocalStores& stores, Placement src_placement, const std::string& oid,
                       std::vector<std::uint8_t>& out,
                       std::unordered_map<std::string, std::string>& attrs_out);

bool install_replica_version(const Config& cfg, const std::string& advertise,
                             const ClusterMap& map, LocalStores& stores, const Placement& dest,
                             const std::string& oid, const std::vector<std::uint8_t>& data,
                             const std::unordered_map<std::string, std::string>& attrs);

}  // namespace aios
