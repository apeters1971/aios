#include "object/transition.hpp"

#include "cluster/place.hpp"
#include "ec/codec_factory.hpp"
#include "ec/ec_attrs.hpp"
#include "net/object_client.hpp"
#include "object/object_layout.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace aios {
namespace {

const TransitionRule* find_transition_rule(const Config& cfg, const std::string& oid) {
  const TransitionRule* best = nullptr;
  for (const auto& rule : cfg.transition_rules) {
    if (oid.size() < rule.prefix.size()) continue;
    if (oid.compare(0, rule.prefix.size(), rule.prefix) != 0) continue;
    if (!best || rule.prefix.size() > best->prefix.size()) best = &rule;
  }
  return best;
}

bool load_object_bytes(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                       LocalStores& stores, Placement src_placement, const std::string& oid,
                       std::vector<std::uint8_t>& out,
                       std::unordered_map<std::string, std::string>& attrs_out) {
  std::string err;

  for (const auto& t : src_placement.acting_set) {
    if (t.node_id != cfg.node_id) continue;
    auto* store = stores.get(t.aios_path);
    if (!store) continue;
    auto info = store->stat(oid, err);
    if (!info || info->is_delete) continue;
    attrs_out = store->list_attrs(oid, err);
    if (attrs_are_ec(attrs_out)) break;
    auto got = store->get(oid, err);
    if (!got) continue;
    out = std::move(*got);
    return true;
  }

  if (attrs_out.empty()) {
    for (const auto& t : src_placement.acting_set) {
      if (t.node_id == cfg.node_id) continue;
      auto st = object_stat_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                                   cfg.auth_skew_ms, map.epoch, t.aios_path, oid);
      if (!st.ok) continue;
      if (st.body.contains("attrs") && st.body["attrs"].is_object()) {
        for (auto it = st.body["attrs"].begin(); it != st.body["attrs"].end(); ++it) {
          if (it.value().is_string()) attrs_out[it.key()] = it.value().get<std::string>();
        }
      }
      if (!attrs_out.empty()) break;
    }
  }

  if (attrs_are_ec(attrs_out)) {
    auto meta = parse_ec_attrs(attrs_out);
    if (!meta) return false;
    const int n = meta->k + meta->m;
    const std::string sc = storage_class_for_attrs(attrs_out, src_placement.storage_class);
    src_placement = place(oid, map, n, sc);
    auto codec = make_erasure_codec(meta->k, meta->m, meta->codec, err);
    if (!codec) return false;
    std::vector<std::optional<std::vector<std::uint8_t>>> shards(static_cast<std::size_t>(n));
    int got = 0;
    for (std::size_t i = 0; i < src_placement.acting_set.size() && i < shards.size(); ++i) {
      const auto& t = src_placement.acting_set[i];
      if (t.node_id == cfg.node_id) {
        auto* store = stores.get(t.aios_path);
        if (!store) continue;
        auto s = store->get(oid, err);
        if (!s) continue;
        shards[i] = std::move(*s);
        ++got;
      } else {
        auto r = object_get_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                                   cfg.auth_skew_ms, map.epoch, t.aios_path, oid);
        if (!r.ok || !r.data) continue;
        shards[i] = std::move(*r.data);
        ++got;
      }
    }
    if (got < meta->k) return false;
    return codec->decode(shards, static_cast<std::size_t>(meta->full_size), out, err);
  }

  for (const auto& t : src_placement.acting_set) {
    if (t.node_id == cfg.node_id) continue;
    auto r = object_get_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                               cfg.auth_skew_ms, map.epoch, t.aios_path, oid);
    if (!r.ok || !r.data) continue;
    out = std::move(*r.data);
    return true;
  }
  return false;
}

bool dest_quorum_ready(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                       LocalStores& stores, const Placement& dest, const std::string& oid) {
  int present = 0;
  for (const auto& t : dest.acting_set) {
    if (t.node_id == cfg.node_id) {
      auto* store = stores.get(t.aios_path);
      if (!store) continue;
      std::string err;
      auto info = store->stat(oid, err);
      if (info && !info->is_delete) ++present;
    } else {
      auto st = object_stat_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                                   cfg.auth_skew_ms, map.epoch, t.aios_path, oid);
      if (st.ok) ++present;
    }
  }
  const int need = cfg.write_quorum > 0
                       ? std::min(cfg.write_quorum, static_cast<int>(dest.acting_set.size()))
                       : static_cast<int>(dest.acting_set.size());
  return present >= need;
}

bool install_replica_version(const Config& cfg, const std::string& advertise,
                             const ClusterMap& map, LocalStores& stores,
                             const Placement& dest, const std::string& oid,
                             const std::vector<std::uint8_t>& data,
                             const std::unordered_map<std::string, std::string>& attrs) {
  if (dest.acting_set.empty() || dest.acting_set[0].node_id != cfg.node_id) return false;
  auto* primary = stores.get(dest.acting_set[0].aios_path);
  if (!primary) return false;
  std::string err;
  PreparedVersion pv;
  if (!primary->prepare_put(oid, data.data(), data.size(), attrs, true, std::nullopt, pv,
                            err)) {
    AIOS_LOG_WARN("transition prepare ", oid, ": ", err);
    return false;
  }
  int ok = 1;
  for (std::size_t i = 1; i < dest.acting_set.size(); ++i) {
    const auto& t = dest.acting_set[i];
    if (t.node_id == cfg.node_id) {
      auto* s = stores.get(t.aios_path);
      if (!s) continue;
      if (s->install_version(pv, data.data(), data.size(), attrs, err)) ++ok;
    } else {
      auto r = object_install_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                                     cfg.auth_skew_ms, map.epoch, t.aios_path, pv, data.data(),
                                     data.size(), attrs);
      if (r.ok) ++ok;
    }
  }
  const int need = cfg.write_quorum > 0
                       ? std::min(cfg.write_quorum, static_cast<int>(dest.acting_set.size()))
                       : static_cast<int>(dest.acting_set.size());
  if (ok < need) {
    primary->abort_version(oid, pv.seq, err);
    return false;
  }
  if (!primary->publish_tip(oid, pv.seq, err)) {
    AIOS_LOG_WARN("transition publish ", oid, ": ", err);
    return false;
  }
  for (std::size_t i = 1; i < dest.acting_set.size(); ++i) {
    const auto& t = dest.acting_set[i];
    if (t.node_id == cfg.node_id) {
      auto* s = stores.get(t.aios_path);
      if (s) s->publish_tip(oid, pv.seq, err);
    } else {
      object_publish_tip_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                                cfg.auth_skew_ms, map.epoch, t.aios_path, oid, pv.seq);
    }
  }
  return true;
}

bool migrate_one(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                 LocalStores& stores, const std::string& oid, const TransitionRule& rule) {
  LayoutRequest req;
  if (rule.layout) req.layout = *rule.layout;
  req.storage_class = rule.to;
  if (rule.ec_k) req.ec_k = *rule.ec_k;
  if (rule.ec_m) req.ec_m = *rule.ec_m;
  if (rule.ec_codec) req.ec_codec = *rule.ec_codec;

  ObjectLayout layout;
  std::string err;
  if (!resolve_object_layout(cfg, oid, req, layout, err)) {
    AIOS_LOG_WARN("transition layout ", oid, ": ", err);
    return false;
  }

  auto dest = place(oid, map, layout.n, rule.to);
  if (dest.acting_set.empty() || dest.acting_set[0].node_id != cfg.node_id) {
    return false;
  }

  std::unordered_map<std::string, std::string> tip_attrs;
  for (const auto& path : stores.paths()) {
    auto* s = stores.get(path);
    if (!s) continue;
    auto info = s->stat(oid, err);
    if (!info || info->is_delete) continue;
    tip_attrs = s->list_attrs(oid, err);
    break;
  }
  const int sn = placement_n_for_attrs(tip_attrs, map.replica_count);
  const std::string sc = storage_class_for_attrs(tip_attrs, rule.from);
  auto src = place(oid, map, sn, sc);

  std::vector<std::uint8_t> data;
  std::unordered_map<std::string, std::string> loaded_attrs;
  if (!load_object_bytes(cfg, advertise, map, stores, src, oid, data, loaded_attrs)) {
    return false;
  }
  if (!loaded_attrs.empty()) tip_attrs = loaded_attrs;

  const std::string cur = storage_class_for_attrs(tip_attrs, rule.from);
  if (cur == rule.to) return true;
  if (cur != rule.from) return false;

  std::unordered_map<std::string, std::string> new_attrs = tip_attrs;
  for (auto it = new_attrs.begin(); it != new_attrs.end();) {
    if (it->first.rfind("aios.ec.", 0) == 0) it = new_attrs.erase(it);
    else ++it;
  }
  apply_layout_attrs(new_attrs, layout);
  new_attrs[kStorageClassAttr] = rule.to;
  new_attrs[kStorageClassPrevAttr] = rule.from;
  new_attrs[kTransitionAttr] = "copying";

  if (layout.is_ec()) {
    auto codec = make_erasure_codec(layout.ec_k, layout.ec_m, layout.ec_codec, err);
    if (!codec) return false;
    std::vector<std::vector<std::uint8_t>> shards;
    if (!codec->encode(data, shards, err)) return false;
    if (shards.size() != dest.acting_set.size()) return false;

    const std::uint32_t full_crc = crc32c(data.data(), data.size());
    PreparedVersion pv;
    int ok = 0;
    for (std::size_t i = 0; i < dest.acting_set.size(); ++i) {
      auto shard_attrs = new_attrs;
      set_ec_attrs(shard_attrs, layout.ec_k, layout.ec_m, static_cast<int>(i), layout.ec_codec,
                   data.size(), full_crc);
      const auto& t = dest.acting_set[i];
      if (i == 0) {
        auto* s = stores.get(t.aios_path);
        if (!s) return false;
        if (!s->prepare_put(oid, shards[i].data(), shards[i].size(), shard_attrs, true,
                            std::nullopt, pv, err)) {
          return false;
        }
        ++ok;
        continue;
      }
      PreparedVersion shard_pv = pv;
      shard_pv.size = shards[i].size();
      shard_pv.crc32c = crc32c(shards[i].data(), shards[i].size());
      if (t.node_id == cfg.node_id) {
        auto* s = stores.get(t.aios_path);
        if (s && s->install_version(shard_pv, shards[i].data(), shards[i].size(), shard_attrs,
                                    err)) {
          ++ok;
        }
      } else {
        auto r = object_install_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                                       cfg.auth_skew_ms, map.epoch, t.aios_path, shard_pv,
                                       shards[i].data(), shards[i].size(), shard_attrs);
        if (r.ok) ++ok;
      }
    }
    if (ok < layout.ec_k) {
      auto* s = stores.get(dest.acting_set[0].aios_path);
      if (s) s->abort_version(oid, pv.seq, err);
      return false;
    }
    auto* primary = stores.get(dest.acting_set[0].aios_path);
    if (!primary || !primary->publish_tip(oid, pv.seq, err)) return false;
    for (std::size_t i = 1; i < dest.acting_set.size(); ++i) {
      const auto& t = dest.acting_set[i];
      if (t.node_id == cfg.node_id) {
        auto* s = stores.get(t.aios_path);
        if (s) s->publish_tip(oid, pv.seq, err);
      } else {
        object_publish_tip_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                                  cfg.auth_skew_ms, map.epoch, t.aios_path, oid, pv.seq);
      }
    }
    return true;
  }

  return install_replica_version(cfg, advertise, map, stores, dest, oid, data, new_attrs);
}

bool drain_one(const Config& cfg, const std::string& advertise, const ClusterMap& map,
               LocalStores& stores, const std::string& oid, const TransitionRule& rule) {
  std::string err;
  std::unordered_map<std::string, std::string> attrs;
  ObjectStore* local = nullptr;
  for (const auto& path : stores.paths()) {
    auto* s = stores.get(path);
    if (!s) continue;
    auto info = s->stat(oid, err);
    if (!info || info->is_delete) continue;
    attrs = s->list_attrs(oid, err);
    local = s;
    break;
  }
  if (!local) return false;
  if (storage_class_for_attrs(attrs, {}) != rule.to) return false;
  if (storage_class_prev_for_attrs(attrs) != rule.from) return false;
  auto tit = attrs.find(kTransitionAttr);
  if (tit != attrs.end() && tit->second == "done") return false;

  const int n = placement_n_for_attrs(attrs, map.replica_count);
  auto dest = place(oid, map, n, rule.to);
  if (dest.acting_set.empty() || dest.acting_set[0].node_id != cfg.node_id) return false;
  if (!dest_quorum_ready(cfg, advertise, map, stores, dest, oid)) return false;

  auto got = local->get(oid, err);
  if (!got) return false;
  attrs.erase(kStorageClassPrevAttr);
  attrs[kTransitionAttr] = "done";
  if (attrs_are_ec(attrs)) {
    // Attr-only drain for EC: rewrite via primary shard body already local.
    return install_replica_version(cfg, advertise, map, stores, dest, oid, *got, attrs);
  }
  return install_replica_version(cfg, advertise, map, stores, dest, oid, *got, attrs);
}

}  // namespace

TransitionStats run_transitions(const Config& cfg, const std::string& advertise,
                                const ClusterMap& map, LocalStores& stores,
                                std::size_t max_oids_per_store) {
  TransitionStats stats;
  if (cfg.transition_rules.empty() || map.targets.empty()) return stats;

  for (const auto& path : stores.paths()) {
    auto* store = stores.get(path);
    if (!store) continue;
    std::string err;
    auto oids = store->list_oids(max_oids_per_store, err);
    if (!err.empty()) continue;
    for (const auto& oid : oids) {
      ++stats.oids_scanned;
      const TransitionRule* rule = find_transition_rule(cfg, oid);
      if (!rule) continue;
      ++stats.matched;

      auto info = store->stat(oid, err);
      if (!info || info->is_delete) continue;
      auto attrs = store->list_attrs(oid, err);
      const std::string cur = storage_class_for_attrs(attrs, rule->from);

      if (cur == rule->to && !storage_class_prev_for_attrs(attrs).empty()) {
        if (drain_one(cfg, advertise, map, stores, oid, *rule)) ++stats.drained;
        else ++stats.failed;
        continue;
      }

      if (cur != rule->from) continue;
      if (migrate_one(cfg, advertise, map, stores, oid, *rule)) ++stats.migrated;
      else ++stats.failed;
    }
  }
  return stats;
}

}  // namespace aios
