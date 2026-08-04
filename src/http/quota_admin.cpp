#include "http/quota_admin.hpp"

#include "util/log.hpp"

#include <algorithm>

namespace aios {
namespace {

std::string limits_oid(const std::string& vol) { return "quota/" + vol + "/limits"; }
std::string usage_oid(const std::string& vol) { return "quota/" + vol + "/usage"; }
std::string ino_oid(const std::string& vol, std::uint64_t ino) {
  return "posix/" + vol + "/ino/" + std::to_string(ino);
}

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

nlohmann::json empty_limits() {
  return {{"volume", {{"uids", nlohmann::json::object()}, {"gids", nlohmann::json::object()}}},
          {"projects", nlohmann::json::object()}};
}

nlohmann::json empty_usage() {
  return {{"epoch", 0},
          {"volume", {{"uids", nlohmann::json::object()}, {"gids", nlohmann::json::object()}}},
          {"projects", nlohmann::json::object()}};
}

}  // namespace

QuotaAdminStore::QuotaAdminStore(Config cfg, ObjectService& objects)
    : cfg_(std::move(cfg)), objects_(objects) {}

std::string QuotaAdminStore::volume() const {
  if (!cfg_.s3_listen.empty() && !cfg_.s3_volume.empty()) return cfg_.s3_volume;
  return "default";
}

bool QuotaAdminStore::load_limits_json(nlohmann::json& j, std::uint64_t& cas, std::string& err) {
  auto r = objects_.api_get(limits_oid(volume()), std::nullopt, std::nullopt, {});
  if (!r.ok || !r.data) {
    j = empty_limits();
    cas = 0;
    return true;
  }
  cas = cas_from_attrs(r.attrs);
  try {
    j = nlohmann::json::parse(
        std::string(reinterpret_cast<const char*>(r.data->data()), r.data->size()));
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
  return true;
}

bool QuotaAdminStore::save_limits_json(const nlohmann::json& j, std::uint64_t cas,
                                       std::string& err) {
  for (int i = 0; i < 8; ++i) {
    nlohmann::json cur;
    std::uint64_t cur_cas = 0;
    if (!load_limits_json(cur, cur_cas, err)) return false;
    if (i > 0) cas = cur_cas;
    if (put_cas(objects_, limits_oid(volume()), j.dump(), cas, err)) return true;
    if (err.find("conflict") == std::string::npos && err.find("cas") == std::string::npos &&
        err.find("precondition") == std::string::npos) {
      return false;
    }
  }
  err = "limits cas retry exhausted";
  return false;
}

bool QuotaAdminStore::load_usage_json(nlohmann::json& j, std::uint64_t& cas, std::string& err) {
  auto r = objects_.api_get(usage_oid(volume()), std::nullopt, std::nullopt, {});
  if (!r.ok || !r.data) {
    j = empty_usage();
    cas = 0;
    return true;
  }
  cas = cas_from_attrs(r.attrs);
  try {
    j = nlohmann::json::parse(
        std::string(reinterpret_cast<const char*>(r.data->data()), r.data->size()));
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
  return true;
}

bool QuotaAdminStore::save_usage_json(const nlohmann::json& j, std::uint64_t cas,
                                      std::string& err) {
  for (int i = 0; i < 8; ++i) {
    nlohmann::json cur;
    std::uint64_t cur_cas = 0;
    if (!load_usage_json(cur, cur_cas, err)) return false;
    if (i > 0) cas = cur_cas;
    if (put_cas(objects_, usage_oid(volume()), j.dump(), cas, err)) return true;
    if (err.find("conflict") == std::string::npos && err.find("cas") == std::string::npos &&
        err.find("precondition") == std::string::npos) {
      return false;
    }
  }
  err = "usage cas retry exhausted";
  return false;
}

bool QuotaAdminStore::set_inode_project(std::uint64_t ino, std::uint32_t project_id,
                                        std::string& err) {
  const auto oid = ino_oid(volume(), ino);
  auto r = objects_.api_get(oid, std::nullopt, std::nullopt, {});
  if (!r.ok || !r.data) {
    err = "inode not found";
    return false;
  }
  try {
    auto j = nlohmann::json::parse(
        std::string(reinterpret_cast<const char*>(r.data->data()), r.data->size()));
    const auto mode = j.value("mode", 0u);
    if ((mode & 0170000) != 0040000) {  // S_IFDIR
      err = "project root must be a directory";
      return false;
    }
    if (j.value("project_id", 0u) != 0 && j.value("project_id", 0u) != project_id) {
      err = "directory already belongs to another project (nested projects unsupported)";
      return false;
    }
    j["project_id"] = project_id;
    const auto body = j.dump();
    const auto cas = cas_from_attrs(r.attrs);
    return put_cas(objects_, oid, body, cas, err);
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

bool QuotaAdminStore::clear_inode_project_if(std::uint64_t ino, std::uint32_t project_id,
                                             std::string& err) {
  const auto oid = ino_oid(volume(), ino);
  auto r = objects_.api_get(oid, std::nullopt, std::nullopt, {});
  if (!r.ok || !r.data) return true;
  try {
    auto j = nlohmann::json::parse(
        std::string(reinterpret_cast<const char*>(r.data->data()), r.data->size()));
    if (j.value("project_id", 0u) != project_id) return true;
    j["project_id"] = 0;
    return put_cas(objects_, oid, j.dump(), cas_from_attrs(r.attrs), err);
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

nlohmann::json QuotaAdminStore::show() {
  std::lock_guard lock(mu_);
  std::string err;
  nlohmann::json lim = empty_limits();
  nlohmann::json use = empty_usage();
  std::uint64_t cas = 0;
  load_limits_json(lim, cas, err);
  load_usage_json(use, cas, err);

  auto vol_uids = nlohmann::json::array();
  auto lim_uids = lim["volume"].value("uids", nlohmann::json::object());
  auto use_uids = use["volume"].value("uids", nlohmann::json::object());
  for (auto it = lim_uids.begin(); it != lim_uids.end(); ++it) {
    std::int64_t used = 0;
    if (use_uids.contains(it.key())) used = use_uids[it.key()].get<std::int64_t>();
    auto bytes = it.value().contains("bytes") ? it.value()["bytes"] : nullptr;
    vol_uids.push_back({{"uid", std::stoul(it.key())}, {"limit_bytes", bytes}, {"used_bytes", used}});
  }
  auto vol_gids = nlohmann::json::array();
  auto lim_gids = lim["volume"].value("gids", nlohmann::json::object());
  auto use_gids = use["volume"].value("gids", nlohmann::json::object());
  for (auto it = lim_gids.begin(); it != lim_gids.end(); ++it) {
    std::int64_t used = 0;
    if (use_gids.contains(it.key())) used = use_gids[it.key()].get<std::int64_t>();
    auto bytes = it.value().contains("bytes") ? it.value()["bytes"] : nullptr;
    vol_gids.push_back({{"gid", std::stoul(it.key())}, {"limit_bytes", bytes}, {"used_bytes", used}});
  }
  auto projects = nlohmann::json::array();
  auto lim_proj = lim.value("projects", nlohmann::json::object());
  auto use_proj = use.value("projects", nlohmann::json::object());
  for (auto it = lim_proj.begin(); it != lim_proj.end(); ++it) {
    std::int64_t used = 0;
    if (use_proj.contains(it.key()) && use_proj[it.key()].contains("bytes")) {
      used = use_proj[it.key()]["bytes"].get<std::int64_t>();
    }
    projects.push_back({{"id", std::stoul(it.key())},
                        {"name", it.value().value("name", "")},
                        {"root_ino", it.value().value("root_ino", 0)},
                        {"limit_bytes", it.value().contains("bytes") ? it.value()["bytes"] : nullptr},
                        {"used_bytes", used}});
  }
  return {{"volume", volume()},
          {"volume_uids", vol_uids},
          {"volume_gids", vol_gids},
          {"projects", projects}};
}

bool QuotaAdminStore::set_volume_uid_limit(std::uint32_t uid, std::optional<std::uint64_t> bytes,
                                           std::string& err) {
  std::lock_guard lock(mu_);
  nlohmann::json j;
  std::uint64_t cas = 0;
  if (!load_limits_json(j, cas, err)) return false;
  auto key = std::to_string(uid);
  if (!bytes) {
    j["volume"]["uids"].erase(key);
  } else {
    j["volume"]["uids"][key] = {{"bytes", *bytes}};
  }
  return save_limits_json(j, cas, err);
}

bool QuotaAdminStore::set_volume_gid_limit(std::uint32_t gid, std::optional<std::uint64_t> bytes,
                                           std::string& err) {
  std::lock_guard lock(mu_);
  nlohmann::json j;
  std::uint64_t cas = 0;
  if (!load_limits_json(j, cas, err)) return false;
  auto key = std::to_string(gid);
  if (!bytes) {
    j["volume"]["gids"].erase(key);
  } else {
    j["volume"]["gids"][key] = {{"bytes", *bytes}};
  }
  return save_limits_json(j, cas, err);
}

bool QuotaAdminStore::create_project(const std::string& name, std::uint64_t root_ino,
                                     std::optional<std::uint64_t> bytes, std::uint32_t& id_out,
                                     std::string& err) {
  if (name.empty() || root_ino == 0) {
    err = "name and root_ino required";
    return false;
  }
  std::lock_guard lock(mu_);
  nlohmann::json j;
  std::uint64_t cas = 0;
  if (!load_limits_json(j, cas, err)) return false;
  std::uint32_t next = 1;
  for (auto it = j["projects"].begin(); it != j["projects"].end(); ++it) {
    next = std::max(next, static_cast<std::uint32_t>(std::stoul(it.key()) + 1));
    if (it.value().value("root_ino", 0ull) == root_ino) {
      err = "project already exists on this inode";
      return false;
    }
  }
  if (!set_inode_project(root_ino, next, err)) return false;
  nlohmann::json pj{{"name", name},
                    {"root_ino", root_ino},
                    {"uids", nlohmann::json::object()},
                    {"gids", nlohmann::json::object()}};
  if (bytes) pj["bytes"] = *bytes;
  j["projects"][std::to_string(next)] = pj;
  if (!save_limits_json(j, cas, err)) {
    clear_inode_project_if(root_ino, next, err);
    return false;
  }
  id_out = next;
  return true;
}

bool QuotaAdminStore::set_project_uid_limit(std::uint32_t project_id, std::uint32_t uid,
                                            std::optional<std::uint64_t> bytes, std::string& err) {
  std::lock_guard lock(mu_);
  nlohmann::json j;
  std::uint64_t cas = 0;
  if (!load_limits_json(j, cas, err)) return false;
  auto key = std::to_string(project_id);
  if (!j["projects"].contains(key)) {
    err = "project not found";
    return false;
  }
  auto ukey = std::to_string(uid);
  if (!bytes) j["projects"][key]["uids"].erase(ukey);
  else j["projects"][key]["uids"][ukey] = {{"bytes", *bytes}};
  return save_limits_json(j, cas, err);
}

bool QuotaAdminStore::delete_project(std::uint32_t project_id, std::string& err) {
  std::lock_guard lock(mu_);
  nlohmann::json j;
  std::uint64_t cas = 0;
  if (!load_limits_json(j, cas, err)) return false;
  auto key = std::to_string(project_id);
  if (!j["projects"].contains(key)) {
    err = "not found";
    return false;
  }
  const auto root_ino = j["projects"][key].value("root_ino", 0ull);
  j["projects"].erase(key);
  if (!save_limits_json(j, cas, err)) return false;
  if (root_ino) clear_inode_project_if(root_ino, project_id, err);
  return true;
}

bool QuotaAdminStore::reconcile(std::string& err) {
  std::lock_guard lock(mu_);
  const auto prefix = "posix/" + volume() + "/ino/";
  auto listed = objects_.api_list(prefix, "", "", 10000, "", false);
  if (!listed.ok) {
    err = listed.error.empty() ? listed.code : listed.error;
    return false;
  }
  nlohmann::json use = empty_usage();
  use["epoch"] = 1;
  for (const auto& obj : listed.list.objects) {
    auto r = objects_.api_get(obj.oid, std::nullopt, std::nullopt, {});
    if (!r.ok || !r.data) continue;
    try {
      auto j = nlohmann::json::parse(
          std::string(reinterpret_cast<const char*>(r.data->data()), r.data->size()));
      const auto mode = j.value("mode", 0u);
      if ((mode & 0170000) != 0100000) continue;  // regular files only for size
      const auto size = static_cast<std::int64_t>(j.value("size", 0ull));
      if (size <= 0) continue;
      const auto uid = j.value("uid", 0u);
      const auto gid = j.value("gid", 0u);
      const auto pid = j.value("project_id", 0u);
      auto& vu = use["volume"]["uids"];
      auto uk = std::to_string(uid);
      vu[uk] = vu.value(uk, 0ll) + size;
      auto& vg = use["volume"]["gids"];
      auto gk = std::to_string(gid);
      vg[gk] = vg.value(gk, 0ll) + size;
      if (pid != 0) {
        auto pk = std::to_string(pid);
        if (!use["projects"].contains(pk)) {
          use["projects"][pk] = {{"bytes", 0},
                                 {"uids", nlohmann::json::object()},
                                 {"gids", nlohmann::json::object()}};
        }
        use["projects"][pk]["bytes"] = use["projects"][pk].value("bytes", 0ll) + size;
        use["projects"][pk]["uids"][uk] = use["projects"][pk]["uids"].value(uk, 0ll) + size;
        use["projects"][pk]["gids"][gk] = use["projects"][pk]["gids"].value(gk, 0ll) + size;
      }
    } catch (...) {
    }
  }
  nlohmann::json cur;
  std::uint64_t cas = 0;
  if (!load_usage_json(cur, cas, err)) return false;
  use["epoch"] = cur.value("epoch", 0ull) + 1;
  return save_usage_json(use, cas, err);
}

}  // namespace aios
