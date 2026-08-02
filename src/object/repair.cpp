#include "object/repair.hpp"

#include "cluster/place.hpp"
#include "ec/ec_attrs.hpp"
#include "ec/xor_parity.hpp"
#include "net/object_client.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace aios {
namespace {

struct TargetObjState {
  bool present{false};
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
    auto info = store->stat(oid, err);
    if (!info) return st;
    st.present = true;
    st.size = info->size;
    st.crc32c = info->crc32c;
    st.crc_known = info->crc32c_known;
    st.seq = info->seq;
    return st;
  }
  auto r = object_stat_remote(t.addr, cfg.node_id, advertise, cfg.cluster_key,
                              cfg.auth_skew_ms, map.epoch, t.aios_path, oid);
  if (!r.ok) return st;
  st.present = true;
  st.size = r.size;
  st.crc32c = r.crc32c;
  st.crc_known = r.crc32c_known;
  st.seq = r.body.value("seq", static_cast<std::uint64_t>(0));
  return st;
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
    pv.seq = st.body.value("seq", static_cast<std::uint64_t>(1));
    pv.size = st.size;
    pv.crc32c = st.crc32c;
    pv.inline_body = false;
    pv.is_delete = false;
    // Stream large remote bodies to a temp file; small ones can stay in memory.
    if (pv.size > 256u * 1024u) {
      body_path = (std::filesystem::temp_directory_path() /
                   ("aios-repair-" + oid + "-" + std::to_string(pv.seq)))
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
      if (store->create_staging_file(oid, staging, err)) {
        std::ifstream in(body_path, std::ios::binary);
        std::ofstream out(staging, std::ios::binary | std::ios::trunc);
        if (in && out) {
          out << in.rdbuf();
          out.close();
          std::string rel;
          if (store->place_staging_as_version(oid, pv.seq, staging, rel, err)) {
            pv.fs_path = rel;
            pv.inline_body = false;
            ok = store->install_version(pv, nullptr, 0, attrs, err);
          }
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

  std::string err;
  auto codec = make_xor_parity_codec(meta->k, err);
  if (!codec || codec->m() != meta->m) return false;
  if (static_cast<int>(p.acting_set.size()) < codec->shard_count()) return false;

  std::vector<std::optional<std::vector<std::uint8_t>>> shards(
      static_cast<std::size_t>(codec->shard_count()));
  int got = 0;
  for (int i = 0; i < codec->shard_count(); ++i) {
    if (!has[static_cast<std::size_t>(i)]) continue;
    std::vector<std::uint8_t> body;
    if (!fetch_shard_body(cfg, advertise, map, stores, p.acting_set[static_cast<std::size_t>(i)],
                          oid, body)) {
      continue;
    }
    shards[static_cast<std::size_t>(i)] = std::move(body);
    ++got;
  }
  if (got < meta->k) return false;

  std::vector<std::uint8_t> full;
  if (!codec->decode(shards, static_cast<std::size_t>(meta->full_size), full, err)) {
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

      bool local_in_set = false;
      for (const auto& t : p.acting_set) {
        if (t.node_id == cfg.node_id && t.aios_path == path) {
          local_in_set = true;
          break;
        }
      }
      if (!local_in_set) continue;

      std::vector<TargetObjState> states(p.acting_set.size());
      std::vector<bool> has(p.acting_set.size(), false);
      for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
        states[i] =
            target_object_state(cfg, advertise, map, stores, p.acting_set[i], oid);
        has[i] = states[i].present;
      }

      std::size_t auth = p.acting_set.size();
      if (!p.acting_set.empty() && has[0]) auth = 0;
      else {
        for (std::size_t i = 0; i < p.acting_set.size(); ++i) {
          if (has[i]) {
            auth = i;
            break;
          }
        }
      }
      if (auth == p.acting_set.size()) continue;

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
      const bool is_ec = cfg.durability == "ec" || attrs_are_ec(auth_attrs);

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
      if (!any_fix) continue;

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
      } else {
        ++stats.failed;
      }
    }
  }
  return stats;
}

}  // namespace aios
