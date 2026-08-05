#pragma once

#include "cluster/cluster_map.hpp"
#include "config.hpp"
#include "object/object_service.hpp"
#include "store/local_stores.hpp"

#include <cstddef>
#include <string>

namespace aios {

class BackupPolicyStore;

struct BackupStats {
  std::size_t rules_run{0};
  std::size_t snaps_created{0};
  std::size_t oids_copied{0};
  std::size_t bags_sealed{0};
  std::size_t drained{0};
  std::size_t pruned{0};
  std::size_t failed{0};
};

// Create posix/{vol}/.snap/{id}/ by freezing the volume, copying live oids, then unfreezing.
// path empty or "/" = whole volume; otherwise volume-relative subtree (e.g. "/home/alice").
// policy_id is recorded in the manifest when non-empty (for GFS prune filtering).
bool backup_snapshot_posix(ObjectService& svc, const std::string& volume, const std::string& path,
                           std::string& snap_id_out, std::string& err,
                           std::size_t* oids_copied = nullptr,
                           const std::string& policy_id = {});

// Clone VBD header to dest, materialize parent data objects, clear parent (seal).
bool backup_snapshot_vbd(ObjectService& svc, const std::string& pool, const std::string& name,
                         const std::string& dest_name, std::string& err,
                         std::size_t* oids_copied = nullptr);

// Snapshot (posix/vbd) → pack snap prefix → drain → prune old snaps.
// YAML backup_rules always run. Live policies run when due (or always if force_live).
BackupStats run_backup(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                       LocalStores& stores, ObjectService& svc, std::size_t max_oids_per_store,
                       BackupPolicyStore* policies = nullptr, bool force_live = false);

// GFS retention: keep snaps within keep_days, plus newest per UTC month for keep_monthly months.
// Returns number of oids deleted. Only considers manifests matching policy_id (and path).
std::size_t backup_prune_gfs(ObjectService& svc, const std::string& volume,
                             const std::string& path, const std::string& policy_id, int keep_days,
                             int keep_monthly);

}  // namespace aios
