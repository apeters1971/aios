#include "object/backup.hpp"

#include "http/backup_policy.hpp"
#include "object/archive_bag.hpp"
#include "object/archive_pack.hpp"
#include "object/archive_tape.hpp"
#include "object/object_layout.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <queue>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aios {
namespace {

constexpr std::int64_t kDayMs = 24ll * 60 * 60 * 1000;

std::string make_snap_id() {
  static thread_local std::mt19937_64 rng{
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
  std::ostringstream oss;
  oss << std::hex << rng() << rng();
  return oss.str();
}

bool oid_is_under_snap(const std::string& oid, const std::string& vol_prefix) {
  const std::string marker = vol_prefix + ".snap/";
  return oid.rfind(marker, 0) == 0;
}

std::string normalize_path(std::string path) {
  if (path.empty()) return "/";
  if (path.front() != '/') path = "/" + path;
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  return path;
}

bool is_whole_volume(const std::string& path) {
  const auto p = normalize_path(path);
  return p == "/";
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

ArchiveRule archive_rule_for_policy(const BackupPolicy& bp, const std::string& prefix,
                                    const Config& cfg) {
  ArchiveRule ar;
  ar.prefix = prefix;
  ar.from = bp.from.empty() ? cfg.default_storage_class : bp.from;
  ar.staging_class = bp.staging_class.empty() ? "archive" : bp.staging_class;
  ar.min_age_days = 0;
  ar.min_bag_bytes = 1;
  ar.max_bag_bytes = bp.max_bag_bytes > 0 ? bp.max_bag_bytes : 0;
  ar.max_members = bp.max_members;
  ar.max_open_ms = 0;
  ar.tape_sink = bp.tape_sink;
  ar.tape_root = bp.tape_root;
  ar.tape_uri_prefix = bp.tape_uri_prefix;
  ar.tape_bin = bp.tape_bin;
  ar.tape_s3_endpoint = bp.tape_s3_endpoint;
  ar.tape_put_cmd = bp.tape_put_cmd;
  ar.tape_get_cmd = bp.tape_get_cmd;
  return ar;
}

bool copy_oid(ObjectService& svc, const std::string& src, const std::string& dst,
              std::string& err) {
  auto g = svc.api_get(src, std::nullopt, std::nullopt, {});
  if (!g.ok) {
    if (g.code == "not_found") return true;
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
  for (const auto& oid : list_all(svc, snap_prefix)) {
    if (is_archive_bag_oid(oid)) continue;
    // Manifest is bookkeeping; pack may leave it unfrozen while member tips are frozen.
    if (oid.size() >= 9 && oid.compare(oid.size() - 9, 9, "/manifest") == 0) continue;
    auto st = svc.api_head(oid, {});
    if (!st.ok || !st.info || st.info->is_delete) continue;
    if (!attrs_are_frozen(st.attrs)) return false;
  }
  return true;
}

bool get_json(ObjectService& svc, const std::string& oid, nlohmann::json& j) {
  auto g = svc.api_get(oid, std::nullopt, std::nullopt, {});
  if (!g.ok || !g.data) return false;
  try {
    j = nlohmann::json::parse(std::string(g.data->begin(), g.data->end()));
    return true;
  } catch (...) {
    return false;
  }
}

// Minimal POSIX dir changelog decode (same framing as client/changelog; avoids aios_client link).
bool decode_dir_log(const std::string& buf, std::uint64_t snapshot_op,
                    std::unordered_map<std::string, std::uint64_t>& entries) {
  constexpr std::uint32_t kMagic = 0x6b504f41u;
  auto rd32 = [](const std::string& b, std::size_t& i, std::uint32_t& v) -> bool {
    if (i + 4 > b.size()) return false;
    std::memcpy(&v, b.data() + i, 4);
    i += 4;
    return true;
  };
  auto rd64 = [](const std::string& b, std::size_t& i, std::uint64_t& v) -> bool {
    if (i + 8 > b.size()) return false;
    std::memcpy(&v, b.data() + i, 8);
    i += 8;
    return true;
  };
  auto rdstr = [](const std::string& b, std::size_t& i, std::string& s) -> bool {
    std::uint32_t n = 0;
    if (i + 4 > b.size()) return false;
    std::memcpy(&n, b.data() + i, 4);
    i += 4;
    if (i + n > b.size()) return false;
    s.assign(b.data() + i, n);
    i += n;
    return true;
  };
  std::size_t i = 0;
  while (i + 8 <= buf.size()) {
    const std::size_t start = i;
    std::uint32_t magic = 0, header_len = 0;
    if (!rd32(buf, i, magic) || magic != kMagic) return false;
    if (!rd32(buf, i, header_len)) return true;
    if (i + header_len > buf.size()) return true;
    std::size_t h = i;
    std::uint64_t op_id = 0;
    std::uint32_t op_u = 0, payload_len = 0;
    if (!rd64(buf, h, op_id) || !rd32(buf, h, op_u) || !rd32(buf, h, payload_len)) return false;
    i += header_len;
    if (i + payload_len > buf.size()) {
      i = start;
      return true;
    }
    const std::string payload = buf.substr(i, payload_len);
    i += payload_len;
    if (op_id <= snapshot_op) continue;
    std::vector<std::string> args;
    std::size_t p = 0;
    while (p < payload.size()) {
      std::string s;
      if (!rdstr(payload, p, s)) return false;
      args.push_back(std::move(s));
    }
    if (op_u == 1 && args.size() >= 2) {
      entries[args[0]] = static_cast<std::uint64_t>(std::stoull(args[1]));
    } else if (op_u == 2 && args.size() >= 1) {
      entries.erase(args[0]);
    } else if (op_u == 3 && args.size() >= 2) {
      auto it = entries.find(args[0]);
      if (it != entries.end()) {
        const auto child = it->second;
        entries.erase(it);
        entries[args[1]] = child;
      }
    }
  }
  return true;
}

bool load_dir_entries(ObjectService& svc, const std::string& volume, std::uint64_t ino,
                      std::unordered_map<std::string, std::uint64_t>& entries, std::string& err) {
  entries.clear();
  const std::string meta_oid = "posix/" + volume + "/dir/" + std::to_string(ino) + "/meta";
  const std::string snap_oid = "posix/" + volume + "/dir/" + std::to_string(ino) + "/snap";
  const std::string log_oid = "posix/" + volume + "/dir/" + std::to_string(ino) + "/log";
  nlohmann::json meta;
  if (!get_json(svc, meta_oid, meta)) return true;  // empty dir
  const std::uint64_t log_bytes = meta.value("log_bytes", static_cast<std::uint64_t>(0));
  const std::uint64_t snapshot_op = meta.value("snapshot_op", static_cast<std::uint64_t>(0));
  if (snapshot_op > 0) {
    nlohmann::json sj;
    if (get_json(svc, snap_oid, sj) && sj.contains("entries") && sj["entries"].is_object()) {
      for (auto it = sj["entries"].begin(); it != sj["entries"].end(); ++it) {
        entries[it.key()] = it.value().get<std::uint64_t>();
      }
    }
  }
  if (log_bytes == 0) return true;
  auto log = svc.api_get(log_oid, 0, log_bytes - 1, {});
  if (!log.ok || !log.data || log.data->empty()) return true;
  const std::string body(log.data->begin(), log.data->end());
  if (!decode_dir_log(body, snapshot_op, entries)) {
    err = "bad dir log";
    return false;
  }
  return true;
}

bool resolve_path_ino(ObjectService& svc, const std::string& volume, const std::string& path,
                      std::uint64_t& root_ino, std::string& err) {
  const auto norm = normalize_path(path);
  root_ino = 1;
  if (norm == "/") return true;
  std::string rest = norm.substr(1);
  while (!rest.empty()) {
    auto slash = rest.find('/');
    const std::string comp = slash == std::string::npos ? rest : rest.substr(0, slash);
    rest = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    if (comp.empty() || comp == ".") continue;
    if (comp == "..") {
      err = "path must not contain ..";
      return false;
    }
    std::unordered_map<std::string, std::uint64_t> ents;
    if (!load_dir_entries(svc, volume, root_ino, ents, err)) return false;
    auto it = ents.find(comp);
    if (it == ents.end()) {
      err = "path not found: " + norm;
      return false;
    }
    root_ino = it->second;
  }
  return true;
}

bool load_inode(ObjectService& svc, const std::string& volume, std::uint64_t ino,
                nlohmann::json& j, std::string& err) {
  const std::string oid = "posix/" + volume + "/ino/" + std::to_string(ino);
  if (!get_json(svc, oid, j)) {
    err = "inode missing: " + std::to_string(ino);
    return false;
  }
  return true;
}

bool collect_subtree_oids(ObjectService& svc, const std::string& volume, std::uint64_t root_ino,
                          std::vector<std::string>& oids, std::string& err) {
  oids.clear();
  oids.push_back("posix/" + volume + "/super");
  std::queue<std::uint64_t> q;
  std::unordered_set<std::uint64_t> seen;
  q.push(root_ino);
  seen.insert(root_ino);
  while (!q.empty()) {
    const auto ino = q.front();
    q.pop();
    nlohmann::json ij;
    if (!load_inode(svc, volume, ino, ij, err)) return false;
    oids.push_back("posix/" + volume + "/ino/" + std::to_string(ino));
    const std::uint32_t mode = ij.value("mode", 0u);
    if (S_ISDIR(mode)) {
      for (const char* part : {"meta", "log", "snap"}) {
        oids.push_back("posix/" + volume + "/dir/" + std::to_string(ino) + "/" + part);
      }
      std::unordered_map<std::string, std::uint64_t> ents;
      if (!load_dir_entries(svc, volume, ino, ents, err)) return false;
      for (const auto& [name, child] : ents) {
        (void)name;
        if (seen.insert(child).second) q.push(child);
      }
    } else if (S_ISREG(mode)) {
      const std::uint64_t size = ij.value("size", static_cast<std::uint64_t>(0));
      const std::uint64_t stripe =
          ij.value("stripe_unit", static_cast<std::uint64_t>(1024ull * 1024ull));
      if (stripe == 0) {
        err = "bad stripe_unit";
        return false;
      }
      const std::uint64_t nchunk = size == 0 ? 0 : (size + stripe - 1) / stripe;
      for (std::uint64_t c = 0; c < nchunk; ++c) {
        oids.push_back("posix/" + volume + "/data/" + std::to_string(ino) + "/c/" +
                       std::to_string(c));
      }
    }
  }
  return true;
}

std::tm gm_from_ms(std::int64_t ms) {
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  return tm;
}

struct SnapInfo {
  std::string id;
  std::int64_t created_ms{0};
  std::string path;
  std::string policy_id;
};

std::vector<SnapInfo> load_snap_manifests(ObjectService& svc, const std::string& volume) {
  std::vector<SnapInfo> out;
  for (const auto& id : list_snap_ids(svc, volume)) {
    const std::string man_oid = "posix/" + volume + "/.snap/" + id + "/manifest";
    nlohmann::json j;
    if (!get_json(svc, man_oid, j)) continue;
    SnapInfo s;
    s.id = id;
    s.created_ms = j.value("created_ms", static_cast<std::int64_t>(0));
    s.path = normalize_path(j.value("path", "/"));
    s.policy_id = j.value("policy_id", "");
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace

bool backup_snapshot_posix(ObjectService& svc, const std::string& volume, const std::string& path,
                           std::string& snap_id_out, std::string& err, std::size_t* oids_copied,
                           const std::string& policy_id) {
  if (volume.empty() || volume.find('/') != std::string::npos) {
    err = "invalid volume";
    return false;
  }
  const std::string norm_path = normalize_path(path);
  const std::string live_prefix = "posix/" + volume + "/";
  if (!set_posix_frozen(svc, volume, true, err)) return false;

  bool ok = false;
  snap_id_out = make_snap_id();
  const std::string snap_prefix = live_prefix + ".snap/" + snap_id_out + "/";
  std::size_t copied = 0;
  std::uint64_t root_ino = 1;
  do {
    std::vector<std::string> src_oids;
    if (is_whole_volume(norm_path)) {
      for (const auto& oid : list_all(svc, live_prefix)) {
        if (oid_is_under_snap(oid, live_prefix)) continue;
        src_oids.push_back(oid);
      }
      root_ino = 1;
    } else {
      if (!resolve_path_ino(svc, volume, norm_path, root_ino, err)) goto done;
      if (!collect_subtree_oids(svc, volume, root_ino, src_oids, err)) goto done;
    }
    for (const auto& oid : src_oids) {
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
                       {"path", norm_path},
                       {"root_ino", root_ino},
                       {"created_ms", now_ms()},
                       {"oids", copied}};
    if (!policy_id.empty()) man["policy_id"] = policy_id;
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
    if (!sg.ok) continue;
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

std::size_t backup_prune_gfs(ObjectService& svc, const std::string& volume, const std::string& path,
                             const std::string& policy_id, int keep_days, int keep_monthly) {
  const auto norm_path = normalize_path(path);
  const auto now = now_ms();
  auto snaps = load_snap_manifests(svc, volume);
  std::vector<SnapInfo> mine;
  for (auto& s : snaps) {
    if (!policy_id.empty() && s.policy_id != policy_id) continue;
    if (normalize_path(s.path) != norm_path) continue;
    mine.push_back(std::move(s));
  }
  std::unordered_set<std::string> keep;
  for (const auto& s : mine) {
    if (keep_days < 0 || (now - s.created_ms) <= static_cast<std::int64_t>(keep_days) * kDayMs) {
      keep.insert(s.id);
    }
  }
  // Newest snap per UTC calendar month; keep the most recent keep_monthly months.
  std::unordered_map<int, SnapInfo> best_by_month;  // key = year*12+mon
  for (const auto& s : mine) {
    if (s.created_ms <= 0) continue;
    const auto tm = gm_from_ms(s.created_ms);
    const int key = (tm.tm_year + 1900) * 12 + (tm.tm_mon + 1);
    auto it = best_by_month.find(key);
    if (it == best_by_month.end() || s.created_ms > it->second.created_ms) {
      best_by_month[key] = s;
    }
  }
  std::vector<std::pair<int, SnapInfo>> months(best_by_month.begin(), best_by_month.end());
  std::sort(months.begin(), months.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  for (int i = 0; i < static_cast<int>(months.size()) && i < keep_monthly; ++i) {
    keep.insert(months[static_cast<std::size_t>(i)].second.id);
  }

  std::size_t pruned = 0;
  for (const auto& s : mine) {
    if (keep.count(s.id)) continue;
    const std::string pref = "posix/" + volume + "/.snap/" + s.id + "/";
    if (!snap_fully_drained(svc, pref) && (keep_days > 0 || keep_monthly > 0)) continue;
    for (const auto& oid : list_all(svc, pref)) {
      if (delete_oid_best_effort(svc, oid)) ++pruned;
    }
  }
  return pruned;
}

BackupStats run_backup(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                       LocalStores& stores, ObjectService& svc, std::size_t max_oids_per_store,
                       BackupPolicyStore* policies, bool force_live) {
  BackupStats stats;

  auto pack_drain = [&](const ArchiveRule& ar) {
    auto ars = run_archive_with_rules(cfg, advertise, map, stores, max_oids_per_store, {ar});
    stats.bags_sealed += ars.bags_sealed;
    stats.failed += ars.failed;
    auto drain = run_archive_drain(cfg, advertise, map, stores, max_oids_per_store);
    stats.drained += drain.drained;
  };

  for (const auto& br : cfg.backup_rules) {
    ++stats.rules_run;
    std::string err;
    std::string pack_prefix;
    std::size_t copied = 0;

    if (br.kind == "posix") {
      std::string snap_id;
      if (!backup_snapshot_posix(svc, br.volume, "/", snap_id, err, &copied, {})) {
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

    pack_drain(archive_rule_for_backup(br, pack_prefix, cfg));

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

  if (policies) {
    const auto now = now_ms();
    for (const auto& bp : policies->list()) {
      if (!bp.enabled) continue;
      if (!force_live && !BackupPolicyStore::is_due(bp, now)) continue;
      ++stats.rules_run;
      std::string err;
      std::string pack_prefix;
      std::size_t copied = 0;
      bool ok = false;
      if (bp.kind == "posix") {
        std::string snap_id;
        ok = backup_snapshot_posix(svc, bp.volume, bp.path, snap_id, err, &copied, bp.id);
        if (ok) {
          ++stats.snaps_created;
          stats.oids_copied += copied;
          pack_prefix = "posix/" + bp.volume + "/.snap/" + snap_id + "/";
        }
      } else if (bp.kind == "vbd") {
        const std::string dest = bp.name + "-snap-" + make_snap_id().substr(0, 12);
        ok = backup_snapshot_vbd(svc, bp.pool, bp.name, dest, err, &copied);
        if (ok) {
          ++stats.snaps_created;
          stats.oids_copied += copied;
          pack_prefix = "vd/" + bp.pool + "/" + dest + "/";
        }
      }
      if (!ok) {
        AIOS_LOG_WARN("backup policy ", bp.id, ": ", err);
        ++stats.failed;
        continue;
      }
      pack_drain(archive_rule_for_policy(bp, pack_prefix, cfg));
      if (bp.kind == "posix") {
        stats.pruned +=
            backup_prune_gfs(svc, bp.volume, bp.path, bp.id, bp.keep_days, bp.keep_monthly);
      }
      std::string terr;
      if (!policies->touch_last_run(bp.id, now, terr)) {
        AIOS_LOG_WARN("backup policy touch ", bp.id, ": ", terr);
      }
    }
  }

  return stats;
}

}  // namespace aios
