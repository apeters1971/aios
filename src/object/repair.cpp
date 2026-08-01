#include "object/repair.hpp"

#include "cluster/place.hpp"
#include "net/object_client.hpp"
#include "util/log.hpp"

#include <unordered_map>
#include <vector>

namespace aios {
namespace {

bool target_has_object(const Config& cfg, const std::string& advertise,
                       const ClusterMap& map, LocalStores& stores,
                       const StorageTarget& t, const std::string& oid) {
  if (t.node_id == cfg.node_id) {
    auto* store = stores.get(t.aios_path);
    if (!store) return false;
    std::string err;
    return store->stat(oid, err).has_value();
  }
  auto r = object_stat_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                              cfg.auth_skew_ms, map.epoch, t.aios_path, oid);
  return r.ok;
}

bool push_replica(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                  LocalStores& stores, const StorageTarget& src, const StorageTarget& dst,
                  const std::string& oid) {
  std::vector<std::uint8_t> data;
  std::unordered_map<std::string, std::string> attrs;

  if (src.node_id == cfg.node_id) {
    auto* store = stores.get(src.aios_path);
    if (!store) return false;
    std::string err;
    auto got = store->get(oid, err);
    if (!got) return false;
    data = std::move(*got);
    attrs = store->list_attrs(oid, err);
  } else {
    auto r = object_get_remote(src.addr, cfg.node_id, advertise, cfg.cluster_key,
                               cfg.auth_skew_ms, map.epoch, src.aios_path, oid);
    if (!r.ok || !r.data) return false;
    data = std::move(*r.data);
  }

  if (dst.node_id == cfg.node_id) {
    auto* store = stores.get(dst.aios_path);
    if (!store) return false;
    std::string err;
    return store->put(oid, data.data(), data.size(), attrs, true, err);
  }
  auto r = object_put_remote(dst.addr, cfg.node_id, advertise, cfg.cluster_key,
                             cfg.auth_skew_ms, map.epoch, dst.aios_path, oid, data.data(),
                             data.size(), attrs, /*as_replica=*/true);
  return r.ok;
}

// Prefer primary; else lowest node_id among acting targets that currently hold the object.
bool should_repair(const Config& cfg, const Placement& p, const std::vector<bool>& has) {
  if (p.acting_set.empty()) return false;
  if (p.acting_set[0].node_id == cfg.node_id && has[0]) return true;

  std::string best_node;
  bool best_is_local = false;
  for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
    if (!has[i]) continue;
    const auto& n = p.acting_set[i].node_id;
    if (best_node.empty() || n < best_node) {
      best_node = n;
      best_is_local = (n == cfg.node_id);
    }
  }
  return best_is_local;
}

}  // namespace

RepairStats run_repair(const Config& cfg, const std::string& advertise,
                       const ClusterMap& map, LocalStores& stores,
                       std::size_t max_oids_per_store) {
  RepairStats stats;
  if (map.targets.empty()) return stats;

  for (const auto& path : stores.paths()) {
    auto* store = stores.get(path);
    if (!store) continue;
    std::string err;
    auto oids = store->list_oids(max_oids_per_store, err);
    if (!err.empty()) {
      AIOS_LOG_WARN("repair list_oids ", path, ": ", err);
      continue;
    }
    for (const auto& oid : oids) {
      ++stats.oids_scanned;
      const auto p = place(oid, map);
      if (p.acting_set.empty()) continue;

      // Only consider oids for which this local path is in the acting set.
      bool local_in_set = false;
      for (const auto& t : p.acting_set) {
        if (t.node_id == cfg.node_id && t.aios_path == path) {
          local_in_set = true;
          break;
        }
      }
      if (!local_in_set) continue;

      std::vector<bool> has(p.acting_set.size(), false);
      int present = 0;
      for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
        has[i] = target_has_object(cfg, advertise, map, stores, p.acting_set[i], oid);
        if (has[i]) ++present;
      }
      if (present >= static_cast<int>(p.acting_set.size())) continue;

      ++stats.under_replicated;
      if (!should_repair(cfg, p, has)) continue;

      // Source: first local acting target that has the object, else primary if present.
      std::size_t src_idx = p.acting_set.size();
      for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
        if (has[i] && p.acting_set[i].node_id == cfg.node_id) {
          src_idx = i;
          break;
        }
      }
      if (src_idx == p.acting_set.size()) {
        for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
          if (has[i]) {
            src_idx = i;
            break;
          }
        }
      }
      if (src_idx == p.acting_set.size()) continue;

      bool all_ok = true;
      for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
        if (has[i]) continue;
        if (push_replica(cfg, advertise, map, stores, p.acting_set[src_idx],
                         p.acting_set[i], oid)) {
          AIOS_LOG_INFO("repaired oid=", oid, " -> ", p.acting_set[i].node_id, ":",
                        p.acting_set[i].aios_path);
        } else {
          all_ok = false;
          AIOS_LOG_WARN("repair failed oid=", oid, " -> ", p.acting_set[i].node_id);
        }
      }
      if (all_ok) {
        ++stats.repaired;
      } else {
        ++stats.failed;
      }
    }
  }
  return stats;
}

}  // namespace aios
