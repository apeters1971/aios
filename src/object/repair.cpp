#include "object/repair.hpp"

#include "cluster/lifecycle.hpp"
#include "cluster/place.hpp"
#include "ec/codec_factory.hpp"
#include "ec/ec_attrs.hpp"
#include "net/object_client.hpp"
#include "object/object_layout.hpp"
#include "object/placement_index.hpp"
#include "util/auth.hpp"
#include "util/crc32c.hpp"
#include "util/file_io.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aios {
namespace {

// Raw tip state of one target, delete markers included: `present` means the
// target has a published tip for the oid (live or marker). Tip stat() hides
// markers, which made a deleted object look like one the target never had and
// let repair resurrect it from a stale live replica.
struct TargetObjState {
  bool present{false};
  bool deleted{false};
  std::uint64_t size{0};
  std::uint32_t crc32c{0};
  bool crc_known{false};
  std::uint64_t seq{0};
};

TargetObjState target_object_state(const Config& cfg, const std::string& advertise,
                                   const ClusterMap& map, LocalStores& stores,
                                   const StorageTarget& t, const std::string& oid) {
  TargetObjState st;
  if (t.node_id == cfg.node_id) {
    auto* store = stores.get(t.aios_path);
    if (!store) return st;
    std::string err;
    std::uint64_t tip = 0;
    if (!store->tip_seq(oid, tip, err) || tip == 0) return st;
    auto info = store->stat(oid, tip, err);
    if (!info) return st;
    st.present = true;
    st.deleted = info->is_delete;
    st.size = info->size;
    st.crc32c = info->crc32c;
    st.crc_known = info->crc32c_known;
    st.seq = info->seq;
    return st;
  }
  auto r = object_stat_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                              cfg.auth_skew_ms, map.epoch, t.aios_path, oid,
                              /*include_deleted=*/true);
  if (!r.ok) return st;
  st.present = true;
  if (auto it = r.body.find("deleted"); it != r.body.end() && it->is_boolean()) {
    st.deleted = it->get<bool>();
  }
  st.size = r.size;
  st.crc32c = r.crc32c;
  st.crc_known = r.crc32c_known;
  if (auto it = r.body.find("seq"); it != r.body.end() && it->is_number_unsigned()) {
    st.seq = it->get<std::uint64_t>();
  }
  return st;
}

// Replicate a delete marker at `seq` to a target that still holds an older tip.
bool push_delete_marker(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                        LocalStores& stores, const StorageTarget& dst, const std::string& oid,
                        std::uint64_t seq) {
  PreparedVersion pv;
  pv.oid = oid;
  pv.seq = seq;
  pv.size = 0;
  pv.crc32c = crc32c(nullptr, 0);
  pv.inline_body = true;
  pv.is_delete = true;
  if (dst.node_id == cfg.node_id) {
    auto* store = stores.get(dst.aios_path);
    if (!store) return false;
    std::string err;
    std::uint64_t tip = 0;
    if (store->tip_seq(oid, tip, err) && tip >= seq) return false;
    return store->install_version(pv, nullptr, 0, {}, err) && store->publish_tip(oid, seq, err);
  }
  auto r = object_install_remote(dst.addr, cfg.node_id, advertise, cfg.cluster_key,
                                 cfg.auth_skew_ms, map.epoch, dst.aios_path, pv, nullptr, 0, {});
  if (!r.ok) return false;
  return object_publish_tip_remote(dst.addr, cfg.node_id, advertise, cfg.cluster_key,
                                   cfg.auth_skew_ms, map.epoch, dst.aios_path, oid, seq)
      .ok;
}

// Authoritative copy: highest published seq (markers included); the primary wins
// ties, then acting-set order. Returns acting_set.size() when nothing is present.
std::size_t pick_authoritative(const std::vector<TargetObjState>& states) {
  std::size_t auth = states.size();
  for (std::size_t i = 0; i < states.size(); ++i) {
    if (!states[i].present) continue;
    if (auth == states.size() || states[i].seq > states[auth].seq) auth = i;
  }
  return auth;
}

void cleanup_temp_path(bool have_file, const std::string& body_path) {
  if (!have_file || body_path.find("aios-repair-") == std::string::npos) return;
  std::error_code ec;
  std::filesystem::remove(body_path, ec);
}

bool push_replica(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                  LocalStores& stores, const StorageTarget& src, const StorageTarget& dst,
                  const std::string& oid) {
  std::unordered_map<std::string, std::string> attrs;
  PreparedVersion pv;
  std::string body_path;
  std::vector<std::uint8_t> data;
  bool have_file = false;

  if (src.node_id == cfg.node_id) {
    auto* store = stores.get(src.aios_path);
    if (!store) return false;
    std::string err;
    auto info = store->stat(oid, err);
    if (!info || info->is_delete) return false;
    attrs = store->list_attrs(oid, err);
    pv.oid = oid;
    pv.seq = info->seq;
    pv.size = info->size;
    pv.crc32c = info->crc32c;
    pv.inline_body = info->inline_body;
    pv.fs_path = info->fs_path;
    pv.is_delete = false;
    pv.redirect_oid = info->redirect_oid;
    if (!info->redirect_oid.empty() || info->is_delete) {
      // Install empty/redirect version.
    } else if (!info->inline_body) {
      if (auto p = store->fs_body_path(oid, info->seq, err)) {
        body_path = *p;
        have_file = true;
      }
    }
    if (!have_file && info->redirect_oid.empty()) {
      auto got = store->get(oid, err);
      if (!got) return false;
      data = std::move(*got);
    }
  } else {
    auto st = object_stat_remote(src.addr, cfg.node_id, advertise, cfg.cluster_key,
                                 cfg.auth_skew_ms, map.epoch, src.aios_path, oid);
    if (!st.ok) return false;
    pv.oid = oid;
    pv.seq = 1;
    if (auto it = st.body.find("seq"); it != st.body.end() && it->is_number_unsigned()) {
      pv.seq = it->get<std::uint64_t>();
    }
    pv.size = st.size;
    pv.crc32c = st.crc32c;
    pv.inline_body = false;
    pv.is_delete = false;
    // Stream large remote bodies to a temp file; small ones can stay in memory.
    if (pv.size > 256u * 1024u) {
      // oid is untrusted for path building ("/" or ".." segments); hash it.
      body_path = (std::filesystem::temp_directory_path() /
                   ("aios-repair-" + sha256_hex(oid) + "-" + std::to_string(pv.seq)))
                      .string();
      auto g = object_get_file_remote(src.addr, cfg.node_id, advertise, cfg.cluster_key,
                                      cfg.auth_skew_ms, map.epoch, src.aios_path, oid,
                                      body_path);
      if (!g.ok) return false;
      have_file = true;
    } else {
      auto g = object_get_remote(src.addr, cfg.node_id, advertise, cfg.cluster_key,
                                 cfg.auth_skew_ms, map.epoch, src.aios_path, oid);
      if (!g.ok || !g.data) return false;
      data = std::move(*g.data);
      pv.inline_body = pv.size <= 64 * 1024;
    }
  }

  if (pv.seq == 0) pv.seq = 1;

  // Tip advances are monotonic. The source is the highest-seq copy, so a newer
  // destination tip (a delete marker, or a concurrent write) means our copy is
  // stale: never push past it. Only a same-seq divergent copy (size/crc mismatch)
  // is reinstalled under a fresh seq.
  {
    std::uint64_t dst_tip = 0;
    bool dst_has_tip = false;
    if (dst.node_id == cfg.node_id) {
      if (auto* store = stores.get(dst.aios_path)) {
        std::string terr;
        dst_has_tip = store->tip_seq(oid, dst_tip, terr) && dst_tip > 0;
      }
    } else {
      auto st = object_stat_remote(dst.addr, cfg.node_id, advertise, cfg.cluster_key,
                                   cfg.auth_skew_ms, map.epoch, dst.aios_path, oid,
                                   /*include_deleted=*/true);
      if (st.ok) {
        if (auto it = st.body.find("seq"); it != st.body.end() && it->is_number_unsigned()) {
          dst_tip = it->get<std::uint64_t>();
          dst_has_tip = dst_tip > 0;
        }
      }
    }
    if (dst_has_tip && dst_tip > pv.seq) {
      cleanup_temp_path(have_file, body_path);
      return false;
    }
    if (dst_has_tip && dst_tip == pv.seq) {
      pv.seq = dst_tip + 1;
      pv.fs_path.clear();
    }
  }

  auto publish_dst = [&]() -> bool {
    if (dst.node_id == cfg.node_id) {
      auto* store = stores.get(dst.aios_path);
      if (!store) return false;
      std::string err;
      return store->publish_tip(oid, pv.seq, err);
    }
    auto r = object_publish_tip_remote(dst.addr, cfg.node_id, advertise, cfg.cluster_key,
                                       cfg.auth_skew_ms, map.epoch, dst.aios_path, oid,
                                       pv.seq);
    return r.ok;
  };

  const bool temp_download =
      have_file && body_path.find("aios-repair-") != std::string::npos;
  auto cleanup_temp = [&]() {
    if (!temp_download) return;
    std::error_code ec;
    std::filesystem::remove(body_path, ec);
  };

  if (dst.node_id == cfg.node_id) {
    auto* store = stores.get(dst.aios_path);
    if (!store) {
      cleanup_temp();
      return false;
    }
    std::string err;
    bool ok = false;
    if (!pv.redirect_oid.empty()) {
      ok = store->install_version(pv, nullptr, 0, attrs, err);
    } else if (have_file) {
      std::string staging;
      if (store->create_staging_file(oid, staging, err) && file_copy(body_path, staging, err)) {
        std::string rel;
        if (store->place_staging_as_version(oid, pv.seq, staging, rel, err)) {
          pv.fs_path = rel;
          pv.inline_body = false;
          ok = store->install_version(pv, nullptr, 0, attrs, err);
        }
      }
    } else {
      ok = store->install_version(pv, data.data(), data.size(), attrs, err);
    }
    if (ok) ok = publish_dst();
    cleanup_temp();
    return ok;
  }

  ObjectRpcResult r;
  if (have_file) {
    r = object_install_file_remote(dst.addr, cfg.node_id, advertise, cfg.cluster_key,
                                   cfg.auth_skew_ms, map.epoch, dst.aios_path, pv, attrs,
                                   body_path);
  } else {
    r = object_install_remote(dst.addr, cfg.node_id, advertise, cfg.cluster_key,
                              cfg.auth_skew_ms, map.epoch, dst.aios_path, pv, data.data(),
                              data.size(), attrs);
  }
  const bool ok = r.ok && publish_dst();
  cleanup_temp();
  return ok;
}

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

std::unordered_map<std::string, std::string> load_attrs_from_target(
    const Config& cfg, const std::string& advertise, const ClusterMap& map,
    LocalStores& stores, const StorageTarget& t, const std::string& oid) {
  std::unordered_map<std::string, std::string> attrs;
  if (t.node_id == cfg.node_id) {
    auto* store = stores.get(t.aios_path);
    if (!store) return attrs;
    std::string err;
    return store->list_attrs(oid, err);
  }
  auto st = object_stat_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                               cfg.auth_skew_ms, map.epoch, t.aios_path, oid);
  if (!st.ok || !st.body.contains("attrs") || !st.body["attrs"].is_object()) return attrs;
  for (auto it = st.body["attrs"].begin(); it != st.body["attrs"].end(); ++it) {
    if (it.value().is_string()) attrs[it.key()] = it.value().get<std::string>();
  }
  return attrs;
}

bool fetch_shard_body(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                      LocalStores& stores, const StorageTarget& t, const std::string& oid,
                      std::vector<std::uint8_t>& out) {
  if (t.node_id == cfg.node_id) {
    auto* store = stores.get(t.aios_path);
    if (!store) return false;
    std::string err;
    auto got = store->get(oid, err);
    if (!got) return false;
    out = std::move(*got);
    return true;
  }
  auto r = object_get_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                             cfg.auth_skew_ms, map.epoch, t.aios_path, oid);
  if (!r.ok || !r.data) return false;
  out = std::move(*r.data);
  return true;
}

bool push_ec_shard(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                   LocalStores& stores, const StorageTarget& dst, const std::string& oid,
                   std::uint64_t seq, const std::vector<std::uint8_t>& shard,
                   const std::unordered_map<std::string, std::string>& attrs) {
  PreparedVersion pv;
  pv.oid = oid;
  pv.seq = seq;
  pv.size = shard.size();
  pv.crc32c = crc32c(shard.data(), shard.size());
  pv.inline_body = shard.size() <= 64 * 1024;
  pv.is_delete = false;

  auto publish_dst = [&]() -> bool {
    if (dst.node_id == cfg.node_id) {
      auto* store = stores.get(dst.aios_path);
      if (!store) return false;
      std::string err;
      return store->publish_tip(oid, pv.seq, err);
    }
    return object_publish_tip_remote(dst.addr, cfg.node_id, advertise, cfg.cluster_key,
                                     cfg.auth_skew_ms, map.epoch, dst.aios_path, oid, pv.seq)
        .ok;
  };

  if (dst.node_id == cfg.node_id) {
    auto* store = stores.get(dst.aios_path);
    if (!store) return false;
    std::string err;
    if (!store->install_version(pv, shard.data(), shard.size(), attrs, err)) return false;
    return publish_dst();
  }
  auto r = object_install_remote(dst.addr, cfg.node_id, advertise, cfg.cluster_key,
                                 cfg.auth_skew_ms, map.epoch, dst.aios_path, pv, shard.data(),
                                 shard.size(), attrs);
  return r.ok && publish_dst();
}

bool repair_ec_object(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                      LocalStores& stores, const Placement& p, const std::string& oid,
                      const std::vector<bool>& has, const std::vector<bool>& needs_fix,
                      std::uint64_t auth_seq) {
  std::unordered_map<std::string, std::string> tip_attrs;
  for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
    if (!has[i]) continue;
    tip_attrs = load_attrs_from_target(cfg, advertise, map, stores, p.acting_set[i], oid);
    if (attrs_are_ec(tip_attrs)) break;
  }
  auto meta = parse_ec_attrs(tip_attrs);
  if (!meta) return false;
  // Without a known full_crc, a mixed-generation decode cannot be validated before
  // overwriting shards — refuse rather than risk permanent corruption.
  if (!meta->full_crc_known) {
    AIOS_LOG_WARN("ec repair aborted oid=", oid, ": missing aios.ec.full_crc");
    return false;
  }

  std::string err;
  auto codec = make_erasure_codec(meta->k, meta->m, meta->codec, err);
  if (!codec || codec->m() != meta->m) return false;
  if (static_cast<int>(p.acting_set.size()) < codec->shard_count()) return false;

  const auto shard_count = static_cast<std::size_t>(codec->shard_count());
  std::vector<std::optional<std::vector<std::uint8_t>>> shards(shard_count);
  int got = 0;
  // A shard's identity is the aios.ec.i attr it was stamped with, not its slot in
  // the acting set: place() reorders the set whenever the topology changes, so
  // decoding by position would feed the codec mismatched shards.
  // Also skip targets known to need_fix (stale seq) so we do not mix generations.
  for (std::size_t ti = 0; ti < p.acting_set.size(); ++ti) {
    if (!has[ti]) continue;
    if (ti < needs_fix.size() && needs_fix[ti]) continue;
    const auto st = target_object_state(cfg, advertise, map, stores, p.acting_set[ti], oid);
    if (auth_seq > 0 && st.seq > 0 && st.seq != auth_seq) continue;
    const auto shard_attrs = load_attrs_from_target(cfg, advertise, map, stores, p.acting_set[ti],
                                                    oid);
    const auto shard_meta = parse_ec_attrs(shard_attrs);
    const auto idx = (shard_meta && shard_meta->shard_i >= 0)
                         ? static_cast<std::size_t>(shard_meta->shard_i)
                         : ti;
    if (idx >= shard_count || shards[idx]) continue;
    std::vector<std::uint8_t> body;
    if (!fetch_shard_body(cfg, advertise, map, stores, p.acting_set[ti], oid, body)) continue;
    shards[idx] = std::move(body);
    ++got;
  }
  if (got < meta->k) return false;

  std::vector<std::uint8_t> full;
  if (!codec->decode(shards, static_cast<std::size_t>(meta->full_size), full, err)) {
    return false;
  }
  // Repair overwrites healthy shards with the re-encoded result, so a bad decode
  // here is unrecoverable. Verify before writing anything.
  if (crc32c(full.data(), full.size()) != meta->full_crc) {
    AIOS_LOG_WARN("ec repair aborted oid=", oid, ": decoded object crc mismatch");
    return false;
  }
  std::vector<std::vector<std::uint8_t>> encoded;
  if (!codec->encode(full, encoded, err)) return false;

  bool all_ok = true;
  for (int i = 0; i < codec->shard_count(); ++i) {
    if (!needs_fix[static_cast<std::size_t>(i)]) continue;
    auto attrs = tip_attrs;
    attrs[kEcAttrI] = std::to_string(i);
    if (!push_ec_shard(cfg, advertise, map, stores, p.acting_set[static_cast<std::size_t>(i)],
                       oid, auth_seq, encoded[static_cast<std::size_t>(i)], attrs)) {
      all_ok = false;
      AIOS_LOG_WARN("ec repair failed oid=", oid, " shard=", i);
    } else {
      AIOS_LOG_INFO("ec repaired oid=", oid, " shard=", i, " -> ",
                    p.acting_set[static_cast<std::size_t>(i)].node_id);
    }
  }
  return all_ok;
}

std::vector<std::string> up_target_keys(const ClusterMap& map) {
  std::vector<std::string> keys;
  for (const auto& t : map.targets) {
    if (t.state == LifecycleState::Up) keys.push_back(target_key(t));
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

bool placement_topology_changed(const ClusterMap& prev, const ClusterMap& now) {
  if (prev.replica_count != now.replica_count) return true;
  if (prev.placement.vnodes_per_target != now.placement.vnodes_per_target) return true;
  if (prev.placement.min_vnodes != now.placement.min_vnodes) return true;
  if (prev.placement.max_vnodes != now.placement.max_vnodes) return true;
  std::unordered_map<std::string, std::pair<int, std::string>> prev_meta;
  for (const auto& t : prev.targets) {
    if (t.state != LifecycleState::Up) continue;
    prev_meta[target_key(t)] = {t.weight, t.rack};
  }
  for (const auto& t : now.targets) {
    if (t.state != LifecycleState::Up) continue;
    auto it = prev_meta.find(target_key(t));
    if (it == prev_meta.end()) return true;  // added
    if (it->second.first != t.weight || it->second.second != t.rack) return true;
  }
  return false;
}

bool is_local_primary(const Config& cfg, const std::string& path, const Placement& p) {
  return !p.acting_set.empty() && p.acting_set[0].node_id == cfg.node_id &&
         p.acting_set[0].aios_path == path;
}

// Whether this local target should remote-stat the acting set. Replicas whose
// stored acting set still names a live primary leave the check to that primary.
bool should_stat_acting_set(const Config& cfg, const std::string& path, const Placement& p,
                            const std::optional<ObjectStore::ObjectPlacement>& stored) {
  if (is_local_primary(cfg, path, p)) return true;
  if (!stored || stored->target_keys.empty()) return true;
  const auto now_keys = acting_target_keys(p);
  if (stored->target_keys == now_keys) return false;
  const std::string prim = target_key(p.acting_set[0]);
  for (const auto& k : stored->target_keys) {
    if (k == prim) return false;
  }
  std::unordered_set<std::string> in_new(now_keys.begin(), now_keys.end());
  std::string best;
  for (const auto& k : stored->target_keys) {
    if (!in_new.count(k)) continue;
    if (best.empty() || k < best) best = k;
  }
  return best == (cfg.node_id + "\n" + path);
}

std::vector<std::string> select_repair_oids(ObjectStore& store, const RepairHint& hint,
                                            std::size_t max_oids, std::string& err) {
  if (hint.select == RepairSelect::All || hint.scrub) {
    return store.list_oids(max_oids, err);
  }
  if (hint.select == RepairSelect::Unverified) {
    return store.list_oids_unverified(max_oids, err);
  }
  return store.list_oids_for_targets(hint.departed_keys, max_oids, err);
}

}  // namespace

RepairHint plan_repair(const ClusterMap* prev, const ClusterMap& now, bool scrub_due) {
  RepairHint hint;
  if (!prev) {
    hint.select = RepairSelect::All;
    return hint;
  }
  if (prev->content_hash() == now.content_hash()) {
    if (scrub_due) {
      hint.select = RepairSelect::All;
      hint.scrub = true;
    } else {
      hint.select = RepairSelect::Unverified;
    }
    return hint;
  }
  if (placement_topology_changed(*prev, now)) {
    hint.select = RepairSelect::All;
    return hint;
  }
  const auto was = up_target_keys(*prev);
  const auto is = up_target_keys(now);
  std::vector<std::string> departed;
  std::set_difference(was.begin(), was.end(), is.begin(), is.end(),
                      std::back_inserter(departed));
  if (!departed.empty()) {
    hint.select = RepairSelect::Departed;
    hint.departed_keys = std::move(departed);
    return hint;
  }
  hint.select = RepairSelect::All;
  return hint;
}

RepairStats run_repair(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                       LocalStores& stores, std::size_t max_oids_per_store, RepairHint hint) {
  RepairStats stats;
  if (map.targets.empty()) return stats;

  for (const auto& path : stores.paths()) {
    auto* store = stores.get(path);
    if (!store) continue;
    std::string err;
    auto oids = select_repair_oids(*store, hint, max_oids_per_store, err);
    if (!err.empty()) {
      AIOS_LOG_WARN("repair list_oids ", path, ": ", err);
      continue;
    }
    for (const auto& oid : oids) {
      ++stats.oids_scanned;
      auto* local = stores.get(path);
      std::unordered_map<std::string, std::string> local_attrs;
      if (local) local_attrs = local->list_attrs(oid, err);
      const int n = placement_n_for_attrs(local_attrs, map.replica_count);
      const std::string sc =
          storage_class_for_attrs(local_attrs, cfg.default_storage_class);
      auto p = place(oid, map, n, sc);
      if (p.acting_set.empty()) {
        const std::string prev = storage_class_prev_for_attrs(local_attrs);
        if (!prev.empty()) p = place(oid, map, n, prev);
      }
      if (p.acting_set.empty()) continue;

      bool local_in_set = false;
      for (const auto& t : p.acting_set) {
        if (t.node_id == cfg.node_id && t.aios_path == path) {
          local_in_set = true;
          break;
        }
      }

      const StorageTarget* local_target = nullptr;
      for (const auto& t : map.targets) {
        if (t.node_id == cfg.node_id && t.aios_path == path) {
          local_target = &t;
          break;
        }
      }

      std::string perr;
      auto stored = local ? local->get_placement(oid, perr) : std::nullopt;
      const auto now_keys = acting_target_keys(p);
      const bool fp_match = stored && stored->target_keys == now_keys;

      if (local_in_set && !hint.scrub) {
        if (fp_match && stored->verified) {
          ++stats.oids_skipped;
          continue;
        }
        if (!should_stat_acting_set(cfg, path, p, stored)) {
          ++stats.oids_skipped;
          if (fp_match && local && stored && !stored->verified) {
            write_object_placement(local, oid, p, true);
          }
          continue;
        }
      }

      // Drain evacuate: still hold tip but no longer in acting set — push out.
      if (!local_in_set) {
        if (!local_target || local_target->state != LifecycleState::Drain) continue;
        std::string tip_err;
        auto tip = local->stat(oid, tip_err);
        if (!tip || tip->is_delete) continue;

        ++stats.oids_stated;
        std::vector<TargetObjState> states(p.acting_set.size());
        std::vector<bool> has(p.acting_set.size(), false);
        for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
          states[i] =
              target_object_state(cfg, advertise, map, stores, p.acting_set[i], oid);
          has[i] = states[i].present;
        }
        const bool is_ec = attrs_are_ec(local_attrs);
        bool any_fix = false;
        std::vector<bool> needs_fix(p.acting_set.size(), false);
        for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
          if (!has[i]) {
            needs_fix[i] = true;
            any_fix = true;
          }
        }
        if (!any_fix) {
          if (local) write_object_placement(local, oid, p, true);
          continue;
        }

        ++stats.under_replicated;
        if (!should_repair(cfg, p, has)) continue;

        bool all_ok = true;
        if (is_ec) {
          int shard_i = -1;
          if (auto it = local_attrs.find(kEcAttrI); it != local_attrs.end()) {
            try {
              shard_i = std::stoi(it->second);
            } catch (...) {
              shard_i = -1;
            }
          }
          if (shard_i >= 0 && static_cast<std::size_t>(shard_i) < p.acting_set.size() &&
              needs_fix[static_cast<std::size_t>(shard_i)]) {
            std::vector<std::uint8_t> body;
            if (fetch_shard_body(cfg, advertise, map, stores, *local_target, oid, body) &&
                push_ec_shard(cfg, advertise, map, stores,
                              p.acting_set[static_cast<std::size_t>(shard_i)], oid,
                              tip->seq, body, local_attrs)) {
              AIOS_LOG_INFO("evacuated ec oid=", oid, " shard=", shard_i, " -> ",
                            p.acting_set[static_cast<std::size_t>(shard_i)].node_id);
            } else {
              all_ok = false;
              AIOS_LOG_WARN("evacuate ec failed oid=", oid, " shard=", shard_i);
            }
          } else {
            // Not the missing shard index; leave for up-node EC repair.
            continue;
          }
        } else {
          for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
            if (!needs_fix[i]) continue;
            if (push_replica(cfg, advertise, map, stores, *local_target, p.acting_set[i],
                             oid)) {
              AIOS_LOG_INFO("evacuated oid=", oid, " -> ", p.acting_set[i].node_id, ":",
                            p.acting_set[i].aios_path);
            } else {
              all_ok = false;
              AIOS_LOG_WARN("evacuate failed oid=", oid, " -> ",
                            p.acting_set[i].node_id);
            }
          }
        }
        if (all_ok) {
          ++stats.repaired;
          if (local) write_object_placement(local, oid, p, true);
        } else {
          ++stats.failed;
        }
        continue;
      }

      ++stats.oids_stated;
      std::vector<TargetObjState> states(p.acting_set.size());
      std::vector<bool> has(p.acting_set.size(), false);
      for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
        states[i] =
            target_object_state(cfg, advertise, map, stores, p.acting_set[i], oid);
        has[i] = states[i].present;
      }

      const std::size_t auth = pick_authoritative(states);
      if (auth == p.acting_set.size()) continue;

      if (states[auth].deleted) {
        // The newest version is a delete marker: propagate it to targets still
        // holding an older live tip (or nothing). Never resurrect from them.
        bool any_fix = false;
        std::vector<bool> needs_fix(p.acting_set.size(), false);
        for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
          if (i == auth) continue;
          if (has[i] && states[i].seq >= states[auth].seq) continue;
          needs_fix[i] = true;
          any_fix = true;
        }
        if (!any_fix) {
          if (local) write_object_placement(local, oid, p, true);
          continue;
        }
        ++stats.under_replicated;
        if (!should_repair(cfg, p, has)) continue;
        bool all_ok = true;
        for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
          if (!needs_fix[i]) continue;
          if (push_delete_marker(cfg, advertise, map, stores, p.acting_set[i], oid,
                                 states[auth].seq)) {
            AIOS_LOG_INFO("repaired delete marker oid=", oid, " -> ",
                          p.acting_set[i].node_id, ":", p.acting_set[i].aios_path);
          } else {
            all_ok = false;
            AIOS_LOG_WARN("repair delete marker failed oid=", oid, " -> ",
                          p.acting_set[i].node_id);
          }
        }
        if (all_ok) {
          ++stats.repaired;
          if (local) write_object_placement(local, oid, p, true);
        } else {
          ++stats.failed;
        }
        continue;
      }

      if (p.acting_set[auth].node_id == cfg.node_id && !states[auth].crc_known) {
        auto* s = stores.get(p.acting_set[auth].aios_path);
        std::uint32_t c = 0;
        if (s && s->recompute_crc32c(oid, c, err)) {
          states[auth].crc32c = c;
          states[auth].crc_known = true;
        }
      }

      const auto auth_attrs =
          load_attrs_from_target(cfg, advertise, map, stores, p.acting_set[auth], oid);
      const bool is_ec = attrs_are_ec(auth_attrs);

      std::vector<bool> needs_fix(p.acting_set.size(), false);
      bool any_fix = false;
      for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
        if (!has[i]) {
          needs_fix[i] = true;
          any_fix = true;
          continue;
        }
        if (i == auth) continue;
        if (states[auth].seq > 0 && states[i].seq > 0 && states[i].seq != states[auth].seq) {
          needs_fix[i] = true;
          any_fix = true;
          continue;
        }
        if (is_ec) continue;  // shard size/crc legitimately differ
        if (states[i].size != states[auth].size) {
          needs_fix[i] = true;
          any_fix = true;
        } else if (states[auth].crc_known && states[i].crc_known &&
                   states[i].crc32c != states[auth].crc32c) {
          needs_fix[i] = true;
          any_fix = true;
          AIOS_LOG_WARN("crc32c mismatch oid=", oid, " target=", p.acting_set[i].node_id);
        }
      }
      if (!any_fix) {
        if (local) write_object_placement(local, oid, p, true);
        continue;
      }

      ++stats.under_replicated;
      if (!should_repair(cfg, p, has)) continue;

      bool all_ok = true;
      if (is_ec) {
        all_ok = repair_ec_object(cfg, advertise, map, stores, p, oid, has, needs_fix,
                                  states[auth].seq);
      } else {
        std::size_t src_idx = auth;
        for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
          if (!has[i] || p.acting_set[i].node_id != cfg.node_id) continue;
          if (states[i].deleted || states[i].seq != states[auth].seq) continue;
          if (!states[auth].crc_known || !states[i].crc_known ||
              (states[i].crc32c == states[auth].crc32c &&
               states[i].size == states[auth].size)) {
            src_idx = i;
            break;
          }
        }
        for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
          if (!needs_fix[i]) continue;
          if (push_replica(cfg, advertise, map, stores, p.acting_set[src_idx],
                           p.acting_set[i], oid)) {
            AIOS_LOG_INFO("repaired oid=", oid, " -> ", p.acting_set[i].node_id, ":",
                          p.acting_set[i].aios_path);
          } else {
            all_ok = false;
            AIOS_LOG_WARN("repair failed oid=", oid, " -> ", p.acting_set[i].node_id);
          }
        }
      }
      if (all_ok) {
        ++stats.repaired;
        if (local) write_object_placement(local, oid, p, true);
      } else {
        ++stats.failed;
      }
    }
  }
  return stats;
}

}  // namespace aios
