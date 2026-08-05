#pragma once

#include "cluster/cluster_map.hpp"
#include "config.hpp"
#include "store/local_stores.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

struct ArchiveStats {
  std::size_t oids_scanned{0};
  std::size_t matched{0};
  std::size_t packed{0};
  std::size_t bags_sealed{0};
  std::size_t failed{0};
};

// Pack matching tips into large bag objects on staging_class; stub members.
ArchiveStats run_archive(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                         LocalStores& stores, std::size_t max_oids_per_store);

// Rehydrate a frozen tip from its bag (when bag is online / bagged).
bool recall_archived_oid(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                         LocalStores& stores, const std::string& oid, std::string& err);

// Read member bytes from bag without rehydrating the tip.
bool read_frozen_member(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                        LocalStores& stores,
                        const std::unordered_map<std::string, std::string>& stub_attrs,
                        std::vector<std::uint8_t>& out, std::string& err);

}  // namespace aios
