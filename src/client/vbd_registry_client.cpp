#include "client/vbd_registry_client.hpp"

#include <algorithm>
#include <chrono>

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

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool load(Session& session, std::vector<VbdVolume>& out, std::uint64_t& cas, std::string& err) {
  out.clear();
  cas = 0;
  auto snap = session.get_object(VbdRegistryStore::oid());
  if (!snap.exists) return true;
  cas = cas_from_attrs(snap.attrs);
  try {
    auto j = nlohmann::json::parse(snap.body);
    if (!j.contains("volumes") || !j["volumes"].is_array()) {
      err = "bad vd/registry document";
      return false;
    }
    for (const auto& jv : j["volumes"]) {
      VbdVolume v;
      if (!VbdRegistryStore::from_json(jv, v, err)) return false;
      if (!VbdRegistryStore::validate(v, err)) return false;
      out.push_back(std::move(v));
    }
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
  return true;
}

bool save(Session& session, const std::vector<VbdVolume>& volumes, std::uint64_t expected_cas,
          std::string& err) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& v : volumes) arr.push_back(VbdRegistryStore::to_json(v));
  nlohmann::json doc{{"aios_vbd_registry", 1}, {"volumes", arr}};
  try {
    session.put_bytes(VbdRegistryStore::oid(), doc.dump(), {}, expected_cas);
    return true;
  } catch (const client_error& e) {
    err = e.what();
    if (err.empty()) err = e.code();
    return false;
  }
}

std::size_t find_index(const std::vector<VbdVolume>& vols, const std::string& pool,
                       const std::string& name) {
  for (std::size_t i = 0; i < vols.size(); ++i) {
    if (vols[i].pool == pool && vols[i].name == name) return i;
  }
  return vols.size();
}

}  // namespace

bool vbd_registry_upsert(Session& session, VbdVolume v, std::string& err) {
  if (!VbdRegistryStore::validate(v, err)) return false;
  if (v.created_ms == 0) v.created_ms = now_ms();
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::vector<VbdVolume> cur;
    std::uint64_t cas = 0;
    if (!load(session, cur, cas, err)) return false;
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
    if (save(session, cur, cas, err)) return true;
    if (err.find("conflict") == std::string::npos && err.find("cas") == std::string::npos &&
        err.find("precondition") == std::string::npos) {
      return false;
    }
    err.clear();
  }
  err = "cas conflict";
  return false;
}

bool vbd_registry_rename(Session& session, const std::string& old_pool,
                         const std::string& old_name, VbdVolume neu, std::string& err) {
  if (!VbdRegistryStore::validate(neu, err)) return false;
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::vector<VbdVolume> cur;
    std::uint64_t cas = 0;
    if (!load(session, cur, cas, err)) return false;
    const auto old_idx = find_index(cur, old_pool, old_name);
    if (old_idx >= cur.size()) {
      err = "not found";
      return false;
    }
    if (neu.created_ms == 0) neu.created_ms = cur[old_idx].created_ms;
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
    if (save(session, cur, cas, err)) return true;
    if (err.find("conflict") == std::string::npos && err.find("cas") == std::string::npos &&
        err.find("precondition") == std::string::npos) {
      return false;
    }
    err.clear();
  }
  err = "cas conflict";
  return false;
}

}  // namespace aios
