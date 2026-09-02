#pragma once

#include "cluster/cluster_map.hpp"
#include "cluster/place.hpp"
#include "config.hpp"
#include "store/local_stores.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

bool load_object_bytes(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                       LocalStores& stores, Placement src_placement, const std::string& oid,
                       std::vector<std::uint8_t>& out,
                       std::unordered_map<std::string, std::string>& attrs_out);

// expected_prev_tip: when set, the primary refuses to prepare the new version if
// the oid's tip is no longer that seq (a client wrote in between); the function
// then returns false and sets *tip_moved_out (if given) to true.
// new_seq_out: receives the published seq on success.
bool install_replica_version(const Config& cfg, const std::string& advertise,
                             const ClusterMap& map, LocalStores& stores, const Placement& dest,
                             const std::string& oid, const std::vector<std::uint8_t>& data,
                             const std::unordered_map<std::string, std::string>& attrs,
                             std::optional<std::uint64_t> expected_prev_tip = std::nullopt,
                             std::uint64_t* new_seq_out = nullptr,
                             bool* tip_moved_out = nullptr);

}  // namespace aios
