#pragma once

#include "cluster/cluster_map.hpp"
#include "config.hpp"
#include "store/local_stores.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

struct ArchiveDrainStats {
  std::size_t bags_scanned{0};
  std::size_t drained{0};
  std::size_t skipped{0};
  std::size_t failed{0};
};

// Copy bag body to the external/fs tape sink and reclaim staging bytes.
// Scans local archive/bag/* tips with aios.tape_sink set and a non-empty body.
ArchiveDrainStats run_archive_drain(const Config& cfg, const std::string& advertise,
                                    const ClusterMap& map, LocalStores& stores,
                                    std::size_t max_oids_per_store);

// Ensure bag body is present on staging (fetch from tape_uri if drained).
bool ensure_bag_on_staging(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                           LocalStores& stores, const std::string& bag_id,
                           std::unordered_map<std::string, std::string>& bag_attrs,
                           std::string& err);

// Low-level put/get used by drain/restore (also testable).
bool tape_put_bag(const std::unordered_map<std::string, std::string>& bag_attrs,
                  const std::string& bag_id, const std::vector<std::uint8_t>& body,
                  const ArchiveRule* rule, std::string& uri_out, std::string& err);

bool tape_get_bag(const std::unordered_map<std::string, std::string>& bag_attrs,
                  const std::string& bag_id, const ArchiveRule* rule,
                  std::vector<std::uint8_t>& body_out, std::string& err);

const ArchiveRule* find_tape_rule_for_attrs(const Config& cfg,
                                            const std::unordered_map<std::string, std::string>& attrs);

}  // namespace aios
