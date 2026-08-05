#include "object/object_io.hpp"

#include "ec/codec_factory.hpp"
#include "ec/ec_attrs.hpp"
#include "net/object_client.hpp"
#include "object/object_layout.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <optional>

namespace aios {

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

bool install_replica_version(const Config& cfg, const std::string& advertise,
                             const ClusterMap& map, LocalStores& stores, const Placement& dest,
                             const std::string& oid, const std::vector<std::uint8_t>& data,
                             const std::unordered_map<std::string, std::string>& attrs) {
  if (dest.acting_set.empty() || dest.acting_set[0].node_id != cfg.node_id) return false;
  auto* primary = stores.get(dest.acting_set[0].aios_path);
  if (!primary) return false;
  std::string err;
  PreparedVersion pv;
  if (!primary->prepare_put(oid, data.data(), data.size(), attrs, true, std::nullopt, pv, err)) {
    AIOS_LOG_WARN("install_replica prepare ", oid, ": ", err);
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
    AIOS_LOG_WARN("install_replica publish ", oid, ": ", err);
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

}  // namespace aios
