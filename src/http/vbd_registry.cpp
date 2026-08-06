#include "http/vbd_registry.hpp"

#include "util/log.hpp"

#include <algorithm>

namespace aios {
namespace {

std::uint64_t cas_from_attrs(const std::unordered_map<std::string, std::string>& attrs) {
  auto it = attrs.find("aios.posix.cas");
  if (it == attrs.end()) return 0;
  try {
    return static_cast<std::uint64_t>(std::stoull(it->second));
  } catch (...) {
    return 0;
  }
}

bool put_cas(ObjectService& objects, const std::string& oid, const std::string& body,
             std::uint64_t expected_cas, std::string& err) {
  const std::uint64_t new_cas = expected_cas + 1;
  std::unordered_map<std::string, std::string> attrs{{"aios.posix.cas", std::to_string(new_cas)}};
  std::vector<AttrPrecondition> preds;
  if (expected_cas == 0) {
    auto head = objects.api_head(oid, {});
    if (!head.ok || !head.info) {
      preds.push_back({AttrPrecondition::Kind::MustNotExist, {}, {}});
    } else if (cas_from_attrs(head.attrs) == 0) {
      preds.push_back({AttrPrecondition::Kind::Absent, "aios.posix.cas", {}});
    } else {
      err = "cas mismatch";
      return false;
    }
  } else {
    preds.push_back(
        {AttrPrecondition::Kind::Eq, "aios.posix.cas", std::to_string(expected_cas)});
  }
  auto r = objects.api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                           attrs, true, preds);
  if (!r.ok) {
    err = r.error.empty() ? r.code : r.error;
    return false;
  }
  return true;
}

}  // namespace

VbdRegistryStore::VbdRegistryStore(Config cfg, ObjectService& objects)
    : cfg_(std::move(cfg)), objects_(objects) {}

std::string VbdRegistryStore::oid() { return "vd/registry"; }

nlohmann::json VbdRegistryStore::to_json(const VbdVolume& v) {
  nlohmann::json j{{"pool", v.pool},
                   {"name", v.name},
                   {"size", v.size},
                   {"obj_order", v.obj_order},
                   {"sealed", v.sealed},
                   {"created_ms", v.created_ms}};
  if (v.parent_pool) j["parent_pool"] = *v.parent_pool;
  else j["parent_pool"] = nullptr;
  if (v.parent_name) j["parent_name"] = *v.parent_name;
  else j["parent_name"] = nullptr;
  if (v.snapshot_of) j["snapshot_of"] = *v.snapshot_of;
  else j["snapshot_of"] = nullptr;
  return j;
}

bool VbdRegistryStore::from_json(const nlohmann::json& j, VbdVolume& v, std::string& err) {
  if (!j.is_object()) {
    err = "volume must be an object";
    return false;
  }
  v = {};
  v.pool = j.value("pool", "");
  v.name = j.value("name", "");
  v.size = j.value("size", static_cast<std::uint64_t>(0));
  v.obj_order = j.value("obj_order", 22u);
  v.sealed = j.value("sealed", false);
  v.created_ms = j.value("created_ms", static_cast<std::int64_t>(0));
  if (j.contains("parent_pool") && j["parent_pool"].is_string()) {
    v.parent_pool = j["parent_pool"].get<std::string>();
  }
  if (j.contains("parent_name") && j["parent_name"].is_string()) {
    v.parent_name = j["parent_name"].get<std::string>();
  }
  if (j.contains("snapshot_of") && j["snapshot_of"].is_string()) {
    v.snapshot_of = j["snapshot_of"].get<std::string>();
  }
  return true;
}

bool VbdRegistryStore::validate(const VbdVolume& v, std::string& err) {
  if (v.pool.empty() || v.name.empty()) {
    err = "pool and name required";
    return false;
  }
  if (v.pool.find('/') != std::string::npos || v.name.find('/') != std::string::npos) {
    err = "pool/name must not contain '/'";
    return false;
  }
  if (v.size == 0) {
    err = "size must be > 0";
    return false;
  }
  if (v.obj_order < 16 || v.obj_order > 24) {
    err = "obj_order must be 16..24";
    return false;
  }
  return true;
}

std::size_t VbdRegistryStore::find_index(const std::vector<VbdVolume>& vols, const std::string& pool,
                                         const std::string& name) {
  for (std::size_t i = 0; i < vols.size(); ++i) {
    if (vols[i].pool == pool && vols[i].name == name) return i;
  }
  return vols.size();
}

bool VbdRegistryStore::load_locked(std::vector<VbdVolume>& out, std::uint64_t& cas,
                                   std::string& err) {
  out.clear();
  cas = 0;
  auto r = objects_.api_get(oid(), std::nullopt, std::nullopt, {});
  if (!r.ok || !r.data) return true;
  cas = cas_from_attrs(r.attrs);
  try {
    const auto j = nlohmann::json::parse(
        std::string(reinterpret_cast<const char*>(r.data->data()), r.data->size()));
    if (!j.contains("volumes") || !j["volumes"].is_array()) {
      err = "bad vd/registry document";
      return false;
    }
    for (const auto& jv : j["volumes"]) {
      VbdVolume v;
      if (!from_json(jv, v, err)) return false;
      if (!validate(v, err)) return false;
      out.push_back(std::move(v));
    }
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
  return true;
}

bool VbdRegistryStore::save_locked(const std::vector<VbdVolume>& volumes,
                                   std::uint64_t expected_cas, std::string& err) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& v : volumes) arr.push_back(to_json(v));
  nlohmann::json doc{{"aios_vbd_registry", 1}, {"volumes", arr}};
  return put_cas(objects_, oid(), doc.dump(), expected_cas, err);
}

std::vector<VbdVolume> VbdRegistryStore::list() {
  std::lock_guard lock(mu_);
  std::vector<VbdVolume> out;
  std::uint64_t cas = 0;
  std::string err;
  if (!load_locked(out, cas, err)) {
    AIOS_LOG_WARN("vbd registry load: ", err);
    return {};
  }
  return out;
}

nlohmann::json VbdRegistryStore::list_json() {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& v : list()) arr.push_back(to_json(v));
  return {{"volumes", arr}};
}

std::optional<VbdVolume> VbdRegistryStore::get(const std::string& pool, const std::string& name) {
  for (auto& v : list()) {
    if (v.pool == pool && v.name == name) return v;
  }
  return std::nullopt;
}

bool VbdRegistryStore::upsert(VbdVolume v, std::string& err) {
  if (!validate(v, err)) return false;
  if (v.created_ms == 0) v.created_ms = now_ms();
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::lock_guard lock(mu_);
    std::vector<VbdVolume> cur;
    std::uint64_t cas = 0;
    if (!load_locked(cur, cas, err)) return false;
    const auto idx = find_index(cur, v.pool, v.name);
    if (idx < cur.size()) {
      if (cur[idx].created_ms != 0) v.created_ms = cur[idx].created_ms;
      cur[idx] = v;
    } else {
      cur.push_back(v);
    }
    std::sort(cur.begin(), cur.end(), [](const VbdVolume& a, const VbdVolume& b) {
      if (a.pool != b.pool) return a.pool < b.pool;
      return a.name < b.name;
    });
    if (save_locked(cur, cas, err)) return true;
    if (err != "precondition_failed" && err.find("cas") == std::string::npos &&
        err != "conflict") {
      return false;
    }
    err.clear();
  }
  err = "cas conflict";
  return false;
}

bool VbdRegistryStore::remove(const std::string& pool, const std::string& name, std::string& err) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::lock_guard lock(mu_);
    std::vector<VbdVolume> cur;
    std::uint64_t cas = 0;
    if (!load_locked(cur, cas, err)) return false;
    const auto idx = find_index(cur, pool, name);
    if (idx >= cur.size()) {
      err = "not found";
      return false;
    }
    cur.erase(cur.begin() + static_cast<std::ptrdiff_t>(idx));
    if (save_locked(cur, cas, err)) return true;
    if (err != "precondition_failed" && err.find("cas") == std::string::npos &&
        err != "conflict") {
      return false;
    }
    err.clear();
  }
  err = "cas conflict";
  return false;
}

bool VbdRegistryStore::rename(const std::string& old_pool, const std::string& old_name,
                              VbdVolume neu, std::string& err) {
  if (!validate(neu, err)) return false;
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::lock_guard lock(mu_);
    std::vector<VbdVolume> cur;
    std::uint64_t cas = 0;
    if (!load_locked(cur, cas, err)) return false;
    const auto old_idx = find_index(cur, old_pool, old_name);
    if (old_idx >= cur.size()) {
      err = "not found";
      return false;
    }
    if (neu.created_ms == 0) neu.created_ms = cur[old_idx].created_ms;
    // Update children that pointed at the old name.
    for (auto& c : cur) {
      if (c.parent_pool && c.parent_name && *c.parent_pool == old_pool &&
          *c.parent_name == old_name) {
        c.parent_pool = neu.pool;
        c.parent_name = neu.name;
      }
    }
    const auto new_idx = find_index(cur, neu.pool, neu.name);
    if (new_idx < cur.size() && new_idx != old_idx) {
      err = "destination already registered";
      return false;
    }
    cur[old_idx] = neu;
    std::sort(cur.begin(), cur.end(), [](const VbdVolume& a, const VbdVolume& b) {
      if (a.pool != b.pool) return a.pool < b.pool;
      return a.name < b.name;
    });
    if (save_locked(cur, cas, err)) return true;
    if (err != "precondition_failed" && err.find("cas") == std::string::npos &&
        err != "conflict") {
      return false;
    }
    err.clear();
  }
  err = "cas conflict";
  return false;
}

bool VbdRegistryStore::has_children(const std::string& pool, const std::string& name) {
  for (const auto& v : list()) {
    if (v.parent_pool && v.parent_name && *v.parent_pool == pool && *v.parent_name == name) {
      return true;
    }
  }
  return false;
}

bool VbdRegistryStore::delete_volume(const std::string& pool, const std::string& name,
                                     std::string& err) {
  if (pool.empty() || name.empty()) {
    err = "pool and name required";
    return false;
  }
  auto cur = get(pool, name);
  if (!cur) {
    err = "not found";
    return false;
  }
  if (cur->sealed) {
    err = "sealed volumes cannot be deleted (use backup prune)";
    return false;
  }
  if (has_children(pool, name)) {
    err = "volume has COW children";
    return false;
  }

  const std::string prefix = "vd/" + pool + "/" + name + "/";
  std::string cursor;
  for (;;) {
    auto lr = objects_.api_list(prefix, "", "", 256, cursor, false, true);
    if (!lr.ok) {
      err = lr.error.empty() ? lr.code : lr.error;
      return false;
    }
    if (lr.list.objects.empty()) break;
    for (const auto& o : lr.list.objects) {
      if (o.is_delete) continue;
      auto dr = objects_.api_del(o.oid, {});
      if (!dr.ok && dr.code != "not_found") {
        err = dr.error.empty() ? dr.code : dr.error;
        return false;
      }
    }
    if (lr.list.next_cursor.empty()) break;
    cursor = lr.list.next_cursor;
  }

  if (!remove(pool, name, err)) {
    if (err == "not found") return true;
    return false;
  }
  return true;
}

}  // namespace aios
