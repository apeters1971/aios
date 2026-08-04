#include "http/qos_admin.hpp"

#include "metrics/qos_rates.hpp"

#include <chrono>
#include <unordered_map>
#include <vector>

namespace aios {
namespace {

std::string limits_oid(const std::string& vol) { return "qos/" + vol + "/limits"; }

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

std::int64_t now_ms() {
  using clock = std::chrono::system_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
      .count();
}

void apply_limit_fields(nlohmann::json& entry, std::optional<std::uint64_t> iops,
                        std::optional<std::uint64_t> bps, bool clear) {
  if (clear) {
    entry = nlohmann::json::object();
    return;
  }
  if (!entry.is_object()) entry = nlohmann::json::object();
  if (iops) entry["iops"] = *iops;
  if (bps) entry["bps"] = *bps;
}

bool entry_empty(const nlohmann::json& e) {
  return !e.is_object() || e.empty() ||
         ((!e.contains("iops") || e["iops"].is_null()) &&
          (!e.contains("bps") || e["bps"].is_null()));
}

}  // namespace

QosAdminStore::QosAdminStore(Config cfg, ObjectService& objects)
    : cfg_(std::move(cfg)), objects_(objects) {}

std::string QosAdminStore::volume() const {
  if (!cfg_.s3_listen.empty() && !cfg_.s3_volume.empty()) return cfg_.s3_volume;
  return "default";
}

bool QosAdminStore::load_limits_json(nlohmann::json& j, std::uint64_t& cas, std::string& err) {
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

bool QosAdminStore::save_limits_json(const nlohmann::json& j, std::uint64_t cas, std::string& err) {
  for (int i = 0; i < 8; ++i) {
    nlohmann::json cur;
    std::uint64_t cur_cas = 0;
    if (!load_limits_json(cur, cur_cas, err)) return false;
    if (i > 0) cas = cur_cas;
    if (put_cas(objects_, limits_oid(volume()), j.dump(), cas, err)) return true;
    if (err.find("cas") == std::string::npos && err.find("precondition") == std::string::npos &&
        err.find("conflict") == std::string::npos) {
      return false;
    }
  }
  err = "cas retry exhausted";
  return false;
}

nlohmann::json QosAdminStore::monitoring_json() {
  const auto& ops = objects_.ops().total();
  const auto put = ops.put.load();
  const auto get = ops.get.load();
  const auto put_b = ops.put_bytes.load();
  const auto get_b = ops.get_bytes.load();
  const auto now = now_ms();
  double put_iops = 0, get_iops = 0, put_bps = 0, get_bps = 0;
  if (ops_sample_ms_ > 0 && now > ops_sample_ms_) {
    const double secs = (now - ops_sample_ms_) / 1000.0;
    if (secs > 0.05) {
      put_iops = (put - sample_put_) / secs;
      get_iops = (get - sample_get_) / secs;
      put_bps = (put_b - sample_put_bytes_) / secs;
      get_bps = (get_b - sample_get_bytes_) / secs;
    }
  }
  ops_sample_ms_ = now;
  sample_put_ = put;
  sample_get_ = get;
  sample_put_bytes_ = put_b;
  sample_get_bytes_ = get_b;

  auto by_label = objects_.ops().by_label_json();
  return {{"node",
           {{"put_iops", put_iops},
            {"get_iops", get_iops},
            {"put_bps", put_bps},
            {"get_bps", get_bps},
            {"put_total", put},
            {"get_total", get},
            {"put_bytes_total", put_b},
            {"get_bytes_total", get_b}}},
          {"ops_by_label", by_label},
          {"observed", qos_observed_rates_json(volume())}};
}

nlohmann::json QosAdminStore::show() {
  std::lock_guard lock(mu_);
  nlohmann::json lim;
  std::uint64_t cas = 0;
  std::string err;
  if (!load_limits_json(lim, cas, err)) lim = empty_limits();

  auto row = [](const nlohmann::json& e) {
    return nlohmann::json{{"limit_iops", e.contains("iops") ? e["iops"] : nullptr},
                          {"limit_bps", e.contains("bps") ? e["bps"] : nullptr}};
  };

  auto vol_uids = nlohmann::json::array();
  for (auto it = lim["volume"]["uids"].begin(); it != lim["volume"]["uids"].end(); ++it) {
    auto r = row(it.value());
    r["uid"] = std::stoul(it.key());
    vol_uids.push_back(std::move(r));
  }
  auto vol_gids = nlohmann::json::array();
  for (auto it = lim["volume"]["gids"].begin(); it != lim["volume"]["gids"].end(); ++it) {
    auto r = row(it.value());
    r["gid"] = std::stoul(it.key());
    vol_gids.push_back(std::move(r));
  }
  auto projects = nlohmann::json::array();
  for (auto it = lim["projects"].begin(); it != lim["projects"].end(); ++it) {
    auto r = row(it.value());
    r["id"] = std::stoul(it.key());
    auto uids = nlohmann::json::array();
    for (auto uit = it.value().value("uids", nlohmann::json::object()).begin();
         uit != it.value().value("uids", nlohmann::json::object()).end(); ++uit) {
      auto ur = row(uit.value());
      ur["uid"] = std::stoul(uit.key());
      uids.push_back(std::move(ur));
    }
    r["uids"] = uids;
    projects.push_back(std::move(r));
  }

  return {{"volume", volume()},
          {"volume_uids", vol_uids},
          {"volume_gids", vol_gids},
          {"projects", projects},
          {"monitoring", monitoring_json()}};
}

bool QosAdminStore::set_volume_uid(std::uint32_t uid, std::optional<std::uint64_t> iops,
                                   std::optional<std::uint64_t> bps, bool clear, std::string& err) {
  std::lock_guard lock(mu_);
  nlohmann::json j;
  std::uint64_t cas = 0;
  if (!load_limits_json(j, cas, err)) return false;
  auto key = std::to_string(uid);
  if (clear) {
    j["volume"]["uids"].erase(key);
  } else {
    apply_limit_fields(j["volume"]["uids"][key], iops, bps, false);
    if (entry_empty(j["volume"]["uids"][key])) j["volume"]["uids"].erase(key);
  }
  return save_limits_json(j, cas, err);
}

bool QosAdminStore::set_volume_gid(std::uint32_t gid, std::optional<std::uint64_t> iops,
                                   std::optional<std::uint64_t> bps, bool clear, std::string& err) {
  std::lock_guard lock(mu_);
  nlohmann::json j;
  std::uint64_t cas = 0;
  if (!load_limits_json(j, cas, err)) return false;
  auto key = std::to_string(gid);
  if (clear) {
    j["volume"]["gids"].erase(key);
  } else {
    apply_limit_fields(j["volume"]["gids"][key], iops, bps, false);
    if (entry_empty(j["volume"]["gids"][key])) j["volume"]["gids"].erase(key);
  }
  return save_limits_json(j, cas, err);
}

bool QosAdminStore::set_project(std::uint32_t project_id, std::optional<std::uint32_t> uid,
                                std::optional<std::uint64_t> iops, std::optional<std::uint64_t> bps,
                                bool clear, std::string& err) {
  if (project_id == 0) {
    err = "project_id must be > 0";
    return false;
  }
  std::lock_guard lock(mu_);
  nlohmann::json j;
  std::uint64_t cas = 0;
  if (!load_limits_json(j, cas, err)) return false;
  auto pkey = std::to_string(project_id);
  if (!j["projects"].contains(pkey)) {
    j["projects"][pkey] = {{"uids", nlohmann::json::object()}, {"gids", nlohmann::json::object()}};
  }
  auto& proj = j["projects"][pkey];
  if (uid) {
    auto ukey = std::to_string(*uid);
    if (clear) {
      proj["uids"].erase(ukey);
    } else {
      apply_limit_fields(proj["uids"][ukey], iops, bps, false);
      if (entry_empty(proj["uids"][ukey])) proj["uids"].erase(ukey);
    }
  } else {
    if (clear) {
      proj.erase("iops");
      proj.erase("bps");
    } else {
      if (iops) proj["iops"] = *iops;
      if (bps) proj["bps"] = *bps;
    }
  }
  // Drop empty project entries
  if (!proj.contains("iops") && !proj.contains("bps") &&
      proj.value("uids", nlohmann::json::object()).empty() &&
      proj.value("gids", nlohmann::json::object()).empty()) {
    j["projects"].erase(pkey);
  }
  return save_limits_json(j, cas, err);
}

}  // namespace aios
