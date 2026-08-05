#include "object/backup.hpp"

#include "object/archive_bag.hpp"
#include "object/archive_pack.hpp"
#include "object/archive_tape.hpp"
#include "object/object_layout.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace aios {
namespace {

std::string make_snap_id() {
  static thread_local std::mt19937_64 rng{
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
  std::ostringstream oss;
  oss << std::hex << rng() << rng();
  return oss.str();
}

bool oid_is_under_snap(const std::string& oid, const std::string& vol_prefix) {
  // posix/{vol}/.snap/...
  const std::string marker = vol_prefix + ".snap/";
  return oid.rfind(marker, 0) == 0;
}

ArchiveRule archive_rule_for_backup(const BackupRule& br, const std::string& prefix,
                                    const Config& cfg) {
  ArchiveRule ar;
  ar.prefix = prefix;
  ar.from = br.from.empty() ? cfg.default_storage_class : br.from;
  ar.staging_class = br.staging_class.empty() ? "archive" : br.staging_class;
  ar.min_age_days = 0;
  ar.min_bag_bytes = 1;
  ar.max_bag_bytes = br.max_bag_bytes > 0 ? br.max_bag_bytes : 0;
  ar.max_members = br.max_members;
  ar.max_open_ms = 0;
  ar.tape_sink = br.tape_sink;
  ar.tape_root = br.tape_root;
  ar.tape_uri_prefix = br.tape_uri_prefix;
  ar.tape_bin = br.tape_bin;
  ar.tape_s3_endpoint = br.tape_s3_endpoint;
  ar.tape_put_cmd = br.tape_put_cmd;
  ar.tape_get_cmd = br.tape_get_cmd;
  return ar;
}

bool copy_oid(ObjectService& svc, const std::string& src, const std::string& dst,
              std::string& err) {
  auto g = svc.api_get(src, std::nullopt, std::nullopt, {});
  if (!g.ok) {
    if (g.code == "not_found") return true;  // sparse / missing tip
    err = g.error.empty() ? g.code : g.error;
    return false;
  }
  if (!g.data) {
    err = "empty get";
    return false;
  }
  LayoutRequest req;
  auto sc = g.attrs.find("aios.storage_class");
  if (sc != g.attrs.end()) req.storage_class = sc->second;
  auto put = svc.api_put(dst, g.data->data(), g.data->size(), g.attrs, true, {}, std::nullopt,
                         req);
  if (!put.ok) {
    err = put.error.empty() ? put.code : put.error;
    return false;
  }
  return true;
}

bool set_posix_frozen(ObjectService& svc, const std::string& volume, bool frozen,
                      std::string& err) {
  const std::string oid = "posix/" + volume + "/super";
  auto g = svc.api_get(oid, std::nullopt, std::nullopt, {});
  if (!g.ok || !g.data) {
    err = "super missing";
    return false;
  }
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(std::string(g.data->begin(), g.data->end()));
  } catch (...) {
    err = "bad super json";
    return false;
  }
  j["frozen"] = frozen;
  const std::string body = j.dump();
  LayoutRequest req;
  auto put = svc.api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                         g.attrs, true, {}, std::nullopt, req);
  if (!put.ok) {
    err = put.error.empty() ? put.code : put.error;
    return false;
  }
  return true;
}

std::vector<std::string> list_all(ObjectService& svc, const std::string& prefix) {
  std::vector<std::string> out;
  std::string cursor;
  for (;;) {
    auto lr = svc.api_list(prefix, "", "", 256, cursor, false, false);
    if (!lr.ok) break;
    for (const auto& e : lr.list.objects) out.push_back(e.oid);
    if (lr.list.next_cursor.empty()) break;
    cursor = lr.list.next_cursor;
  }
  return out;
}

bool delete_oid_best_effort(ObjectService& svc, const std::string& oid) {
  auto d = svc.api_del(oid, {}, std::nullopt);
  return d.ok || d.code == "not_found";
}

std::vector<std::string> list_snap_ids(ObjectService& svc, const std::string& volume) {
  const std::string prefix = "posix/" + volume + "/.snap/";
  std::unordered_set<std::string> ids;
  for (const auto& oid : list_all(svc, prefix)) {
    if (oid.rfind(prefix, 0) != 0) continue;
    const auto rest = oid.substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0) continue;
    ids.insert(rest.substr(0, slash));
  }
  std::vector<std::string> out(ids.begin(), ids.end());
  std::sort(out.begin(), out.end());
  return out;
}

bool snap_fully_drained(ObjectService& svc, const std::string& snap_prefix) {
  // Consider drained if every non-bag tip under prefix is frozen with tape_uri on its bag,
  // or simply: no unfrozen tips with non-zero size remain (all packed).
  for (const auto& oid : list_all(svc, snap_prefix)) {
    if (is_archive_bag_oid(oid)) continue;
    auto st = svc.api_head(oid, {});
    if (!st.ok || !st.info || st.info->is_delete) continue;
    if (!attrs_are_frozen(st.attrs)) return false;
  }
  return true;
}

}  // namespace

bool backup_snapshot_posix(ObjectService& svc, const std::string& volume, std::string& snap_id_out,
                           std::string& err, std::size_t* oids_copied) {
  if (volume.empty() || volume.find('/') != std::string::npos) {
    err = "invalid volume";
    return false;
  }
  const std::string live_prefix = "posix/" + volume + "/";
  if (!set_posix_frozen(svc, volume, true, err)) return false;

  bool ok = false;
  snap_id_out = make_snap_id();
  const std::string snap_prefix = live_prefix + ".snap/" + snap_id_out + "/";
  std::size_t copied = 0;
  do {
    const auto oids = list_all(svc, live_prefix);
    for (const auto& oid : oids) {
      if (oid_is_under_snap(oid, live_prefix)) continue;
      if (oid.size() < live_prefix.size()) continue;
      const std::string rel = oid.substr(live_prefix.size());
      const std::string dst = snap_prefix + rel;
      if (!copy_oid(svc, oid, dst, err)) {
        AIOS_LOG_WARN("backup posix copy ", oid, " -> ", dst, ": ", err);
        goto done;
      }
      ++copied;
    }
    nlohmann::json man{{"aios_backup_manifest", 1},
                       {"kind", "posix"},
                       {"volume", volume},
                       {"snap_id", snap_id_out},
                       {"created_ms", now_ms()},
                       {"oids", copied}};
    const std::string mb = man.dump();
    LayoutRequest req;
    auto put = svc.api_put(snap_prefix + "manifest",
                           reinterpret_cast<const std::uint8_t*>(mb.data()), mb.size(), {}, true,
                           {}, std::nullopt, req);
    if (!put.ok) {
      err = put.error.empty() ? put.code : put.error;
      goto done;
    }
    ok = true;
  } while (false);

done:
  std::string uerr;
  if (!set_posix_frozen(svc, volume, false, uerr) && ok) {
    err = uerr;
    ok = false;
  }
  if (oids_copied) *oids_copied = copied;
  return ok;
}

bool backup_snapshot_vbd(ObjectService& svc, const std::string& pool, const std::string& name,
                         const std::string& dest_name, std::string& err,
                         std::size_t* oids_copied) {
  if (pool.empty() || name.empty() || dest_name.empty()) {
    err = "pool/name/dest required";
    return false;
  }
  const std::string src_header = "vd/" + pool + "/" + name + "/header";
  const std::string dst_header = "vd/" + pool + "/" + dest_name + "/header";
  auto g = svc.api_get(src_header, std::nullopt, std::nullopt, {});
  if (!g.ok || !g.data) {
    err = "source header missing";
    return false;
  }
  nlohmann::json hj;
  try {
    hj = nlohmann::json::parse(std::string(g.data->begin(), g.data->end()));
  } catch (...) {
    err = "bad vbd header";
    return false;
  }
  const std::uint64_t size = hj.value("size", static_cast<std::uint64_t>(0));
  const std::uint32_t obj_order = hj.value("obj_order", 22u);
  if (size == 0 || obj_order < 16 || obj_order > 24) {
    err = "invalid vbd geometry";
    return false;
  }
  hj["pool"] = pool;
  hj["name"] = dest_name;
  hj["parent_pool"] = pool;
  hj["parent_name"] = name;
  hj["sealed"] = false;
  hj["snapshot_of"] = pool + "/" + name;
  hj["created_ms"] = now_ms();
  {
    const std::string body = hj.dump();
    LayoutRequest req;
    auto put = svc.api_put(dst_header, reinterpret_cast<const std::uint8_t*>(body.data()),
                           body.size(), {}, true, {}, std::nullopt, req);
    if (!put.ok) {
      err = put.error.empty() ? put.code : put.error;
      return false;
    }
  }

  const std::uint64_t obj_size = 1ull << obj_order;
  const std::uint64_t nobj = (size + obj_size - 1) / obj_size;
  std::size_t copied = 0;
  char hex[32];
  for (std::uint64_t i = 0; i < nobj; ++i) {
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(i));
    const std::string src = "vd/" + pool + "/" + name + "/data." + hex;
    const std::string dst = "vd/" + pool + "/" + dest_name + "/data." + hex;
    auto st = svc.api_head(dst, {});
    if (st.ok && st.info && !st.info->is_delete && st.info->size > 0) continue;
    auto sg = svc.api_get(src, std::nullopt, std::nullopt, {});
    if (!sg.ok) {
      // Try parent chain one level.
      const std::string pp = hj.value("parent_pool", "");
      const std::string pn = hj.value("parent_name", "");
      if (pp.empty()) continue;  // sparse zeros
      // Source may itself be sparse; skip missing.
      continue;
    }
    if (!sg.data || sg.data->empty()) continue;
    LayoutRequest req;
    auto put = svc.api_put(dst, sg.data->data(), sg.data->size(), sg.attrs, true, {},
                           std::nullopt, req);
    if (!put.ok) {
      err = put.error.empty() ? put.code : put.error;
      return false;
    }
    ++copied;
  }

  // Seal: clear parent refs.
  hj.erase("parent_pool");
  hj.erase("parent_name");
  hj["sealed"] = true;
  {
    const std::string body = hj.dump();
    LayoutRequest req;
    auto put = svc.api_put(dst_header, reinterpret_cast<const std::uint8_t*>(body.data()),
                           body.size(), {}, true, {}, std::nullopt, req);
    if (!put.ok) {
      err = put.error.empty() ? put.code : put.error;
      return false;
    }
  }
  if (oids_copied) *oids_copied = copied + 1;
  return true;
}

BackupStats run_backup(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                       LocalStores& stores, ObjectService& svc, std::size_t max_oids_per_store) {
  BackupStats stats;
  if (cfg.backup_rules.empty()) return stats;

  for (const auto& br : cfg.backup_rules) {
    ++stats.rules_run;
    std::string err;
    std::string pack_prefix;
    std::size_t copied = 0;

    if (br.kind == "posix") {
      std::string snap_id;
      if (!backup_snapshot_posix(svc, br.volume, snap_id, err, &copied)) {
        AIOS_LOG_WARN("backup posix snapshot ", br.volume, ": ", err);
        ++stats.failed;
        continue;
      }
      ++stats.snaps_created;
      stats.oids_copied += copied;
      pack_prefix = "posix/" + br.volume + "/.snap/" + snap_id + "/";
    } else if (br.kind == "vbd") {
      const std::string dest = br.name + "-snap-" + make_snap_id().substr(0, 12);
      if (!backup_snapshot_vbd(svc, br.pool, br.name, dest, err, &copied)) {
        AIOS_LOG_WARN("backup vbd snapshot ", br.pool, "/", br.name, ": ", err);
        ++stats.failed;
        continue;
      }
      ++stats.snaps_created;
      stats.oids_copied += copied;
      pack_prefix = "vd/" + br.pool + "/" + dest + "/";
    } else {
      ++stats.failed;
      continue;
    }

    ArchiveRule ar = archive_rule_for_backup(br, pack_prefix, cfg);
    auto ars = run_archive_with_rules(cfg, advertise, map, stores, max_oids_per_store, {ar});
    stats.bags_sealed += ars.bags_sealed;
    stats.failed += ars.failed;

    auto drain = run_archive_drain(cfg, advertise, map, stores, max_oids_per_store);
    stats.drained += drain.drained;

    if (br.kind == "posix" && br.retain_snaps >= 0) {
      auto ids = list_snap_ids(svc, br.volume);
      while (static_cast<int>(ids.size()) > br.retain_snaps) {
        const std::string old = ids.front();
        ids.erase(ids.begin());
        const std::string pref = "posix/" + br.volume + "/.snap/" + old + "/";
        if (!snap_fully_drained(svc, pref) && br.retain_snaps > 0) continue;
        for (const auto& oid : list_all(svc, pref)) {
          if (delete_oid_best_effort(svc, oid)) ++stats.pruned;
        }
      }
    }
  }
  return stats;
}

}  // namespace aios
