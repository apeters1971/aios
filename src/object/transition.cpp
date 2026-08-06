#include "object/transition.hpp"

#include "cluster/place.hpp"
#include "ec/codec_factory.hpp"
#include "ec/ec_attrs.hpp"
#include "net/object_client.hpp"
#include "object/object_io.hpp"
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

// Finalize transition bookkeeping on an EC object. Every target keeps its own shard
// body and its own aios.ec.i index; only the attrs change. Broadcasting a single
// shard to the whole acting set (as the replicated path does) would leave every
// target holding the same shard and make the object undecodable.
bool drain_ec_one(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                  LocalStores& stores, const Placement& dest, const std::string& oid) {
  auto finalize = [](std::unordered_map<std::string, std::string> a) {
    a.erase(kStorageClassPrevAttr);
    a[kTransitionAttr] = "done";
    return a;
  };

  auto* primary = stores.get(dest.acting_set[0].aios_path);
  if (!primary) return false;
  std::string err;
  auto primary_body = primary->get(oid, err);
  if (!primary_body) return false;
  const auto primary_attrs = finalize(primary->list_attrs(oid, err));

  PreparedVersion pv;
  if (!primary->prepare_put(oid, primary_body->data(), primary_body->size(), primary_attrs, true,
                            std::nullopt, pv, err)) {
    AIOS_LOG_WARN("ec drain prepare ", oid, ": ", err);
    return false;
  }

  int ok = 1;
  for (std::size_t i = 1; i < dest.acting_set.size(); ++i) {
    const auto& t = dest.acting_set[i];
    if (t.node_id == cfg.node_id) {
      auto* s = stores.get(t.aios_path);
      if (!s) continue;
      auto body = s->get(oid, err);
      if (!body) continue;
      PreparedVersion sv = pv;
      sv.size = body->size();
      sv.crc32c = crc32c(body->data(), body->size());
      sv.fs_path.clear();
      sv.inline_body = sv.size <= 64 * 1024;
      if (s->install_version(sv, body->data(), body->size(), finalize(s->list_attrs(oid, err)),
                             err)) {
        ++ok;
      }
      continue;
    }
    auto st = object_stat_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                                 cfg.auth_skew_ms, map.epoch, t.aios_path, oid);
    if (!st.ok) continue;
    std::unordered_map<std::string, std::string> shard_attrs;
    if (st.body.contains("attrs") && st.body["attrs"].is_object()) {
      for (auto it = st.body["attrs"].begin(); it != st.body["attrs"].end(); ++it) {
        if (it.value().is_string()) shard_attrs[it.key()] = it.value().get<std::string>();
      }
    }
    auto g = object_get_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key, cfg.auth_skew_ms,
                               map.epoch, t.aios_path, oid);
    if (!g.ok || !g.data) continue;
    PreparedVersion sv = pv;
    sv.size = g.data->size();
    sv.crc32c = crc32c(g.data->data(), g.data->size());
    sv.fs_path.clear();
    sv.inline_body = sv.size <= 64 * 1024;
    auto r = object_install_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                                   cfg.auth_skew_ms, map.epoch, t.aios_path, sv, g.data->data(),
                                   g.data->size(), finalize(shard_attrs));
    if (r.ok) ++ok;
  }

  const auto meta = parse_ec_attrs(primary_attrs);
  const int k = meta ? meta->k : 0;
  if (k > 0 && ok < k) {
    primary->abort_version(oid, pv.seq, err);
    AIOS_LOG_WARN("ec drain ", oid, ": only ", ok, " of ", dest.acting_set.size(),
                  " shards rewritten, need ", k);
    return false;
  }
  if (!primary->publish_tip(oid, pv.seq, err)) {
    primary->abort_version(oid, pv.seq, err);
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

  if (attrs_are_ec(attrs)) {
    return drain_ec_one(cfg, advertise, map, stores, dest, oid);
  }

  auto got = local->get(oid, err);
  if (!got) return false;
  attrs.erase(kStorageClassPrevAttr);
  attrs[kTransitionAttr] = "done";
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
