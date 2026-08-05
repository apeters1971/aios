#include "http/backup_policy.hpp"

#include "util/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <sstream>

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

std::int64_t utc_ms(int year, int mon, int mday, int hour, int minute) {
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = mon - 1;
  tm.tm_mday = mday;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = 0;
#if defined(_WIN32)
  return static_cast<std::int64_t>(_mkgmtime(&tm)) * 1000;
#else
  return static_cast<std::int64_t>(timegm(&tm)) * 1000;
#endif
}

}  // namespace

BackupPolicyStore::BackupPolicyStore(Config cfg, ObjectService& objects)
    : cfg_(std::move(cfg)), objects_(objects) {}

std::string BackupPolicyStore::oid() { return "backup/policies"; }

std::string BackupPolicyStore::make_id() {
  static thread_local std::mt19937_64 rng{
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
  std::ostringstream oss;
  oss << std::hex << rng();
  return oss.str();
}

bool BackupPolicyStore::parse_at(const std::string& at, int& hour, int& minute, std::string& err) {
  int h = 0, m = 0;
  if (std::sscanf(at.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
    err = "schedule.at must be HH:MM";
    return false;
  }
  hour = h;
  minute = m;
  return true;
}

bool BackupPolicyStore::validate(const BackupPolicy& p, std::string& err) {
  if (p.kind != "posix" && p.kind != "vbd") {
    err = "kind must be posix or vbd";
    return false;
  }
  if (p.kind == "posix" && p.volume.empty()) {
    err = "posix policy requires volume";
    return false;
  }
  if (p.kind == "vbd" && (p.pool.empty() || p.name.empty())) {
    err = "vbd policy requires pool and name";
    return false;
  }
  if (p.tz != "UTC") {
    err = "tz must be UTC";
    return false;
  }
  int h = 0, m = 0;
  if (!parse_at(p.at, h, m, err)) return false;
  if (p.keep_days < 0 || p.keep_monthly < 0) {
    err = "keep_days/keep_monthly must be >= 0";
    return false;
  }
  const auto comp = p.bag_compression.empty() ? "none" : p.bag_compression;
  const auto enc = p.bag_encryption.empty() ? "none" : p.bag_encryption;
  if (comp != "none" && comp != "zstd") {
    err = "bag_compression must be none or zstd";
    return false;
  }
  if (enc != "none" && enc != "aes-256-gcm") {
    err = "bag_encryption must be none or aes-256-gcm";
    return false;
  }
  return true;
}

nlohmann::json BackupPolicyStore::to_json(const BackupPolicy& p) {
  return {{"id", p.id},
          {"enabled", p.enabled},
          {"kind", p.kind},
          {"volume", p.volume},
          {"path", p.path.empty() ? "/" : p.path},
          {"pool", p.pool},
          {"name", p.name},
          {"schedule", {{"at", p.at}, {"tz", p.tz}}},
          {"retain", {{"keep_days", p.keep_days}, {"keep_monthly", p.keep_monthly}}},
          {"from", p.from},
          {"staging_class", p.staging_class},
          {"max_bag_bytes", p.max_bag_bytes},
          {"max_members", p.max_members},
          {"tape_sink", p.tape_sink},
          {"tape_root", p.tape_root},
          {"tape_uri_prefix", p.tape_uri_prefix},
          {"tape_bin", p.tape_bin},
          {"tape_s3_endpoint", p.tape_s3_endpoint},
          {"tape_put_cmd", p.tape_put_cmd},
          {"tape_get_cmd", p.tape_get_cmd},
          {"bag_compression", p.bag_compression},
          {"bag_compression_level", p.bag_compression_level},
          {"bag_encryption", p.bag_encryption},
          {"last_run_ms", p.last_run_ms}};
}

bool BackupPolicyStore::from_json(const nlohmann::json& j, BackupPolicy& p, std::string& err) {
  if (!j.is_object()) {
    err = "policy must be object";
    return false;
  }
  p = BackupPolicy{};
  p.id = j.value("id", "");
  p.enabled = j.value("enabled", true);
  p.kind = j.value("kind", "posix");
  p.volume = j.value("volume", "");
  p.path = j.value("path", "/");
  p.pool = j.value("pool", "");
  p.name = j.value("name", "");
  if (j.contains("schedule") && j["schedule"].is_object()) {
    p.at = j["schedule"].value("at", "00:00");
    p.tz = j["schedule"].value("tz", "UTC");
  } else {
    p.at = j.value("at", "00:00");
    p.tz = j.value("tz", "UTC");
  }
  if (j.contains("retain") && j["retain"].is_object()) {
    p.keep_days = j["retain"].value("keep_days", 7);
    p.keep_monthly = j["retain"].value("keep_monthly", 12);
  } else {
    p.keep_days = j.value("keep_days", 7);
    p.keep_monthly = j.value("keep_monthly", 12);
  }
  p.from = j.value("from", "");
  p.staging_class = j.value("staging_class", "archive");
  p.max_bag_bytes = j.value("max_bag_bytes", static_cast<std::uint64_t>(0));
  p.max_members = j.value("max_members", 0);
  p.tape_sink = j.value("tape_sink", "");
  p.tape_root = j.value("tape_root", "");
  p.tape_uri_prefix = j.value("tape_uri_prefix", "");
  p.tape_bin = j.value("tape_bin", "");
  p.tape_s3_endpoint = j.value("tape_s3_endpoint", "");
  p.tape_put_cmd = j.value("tape_put_cmd", "");
  p.tape_get_cmd = j.value("tape_get_cmd", "");
  p.bag_compression = j.value("bag_compression", "none");
  if (p.bag_compression.empty()) p.bag_compression = "none";
  p.bag_compression_level = j.value("bag_compression_level", 0);
  p.bag_encryption = j.value("bag_encryption", "none");
  if (p.bag_encryption.empty()) p.bag_encryption = "none";
  p.last_run_ms = j.value("last_run_ms", static_cast<std::int64_t>(0));
  return validate(p, err);
}

bool BackupPolicyStore::is_due(const BackupPolicy& p, std::int64_t now_ms) {
  if (!p.enabled) return false;
  int hour = 0, minute = 0;
  std::string err;
  if (!parse_at(p.at, hour, minute, err)) return false;
  const auto now_tm = gm_from_ms(now_ms);
  std::int64_t trigger = utc_ms(now_tm.tm_year + 1900, now_tm.tm_mon + 1, now_tm.tm_mday, hour,
                                minute);
  if (trigger > now_ms) {
    // Today's trigger is still in the future — use yesterday's.
    trigger -= 24ll * 60 * 60 * 1000;
  }
  return trigger > p.last_run_ms;
}

bool BackupPolicyStore::load_locked(std::vector<BackupPolicy>& out, std::uint64_t& cas,
                                    std::string& err) {
  out.clear();
  auto r = objects_.api_get(oid(), std::nullopt, std::nullopt, {});
  if (!r.ok || !r.data) {
    cas = 0;
    return true;
  }
  cas = cas_from_attrs(r.attrs);
  try {
    auto j = nlohmann::json::parse(
        std::string(reinterpret_cast<const char*>(r.data->data()), r.data->size()));
    if (j.contains("policies") && j["policies"].is_array()) {
      for (const auto& item : j["policies"]) {
        BackupPolicy p;
        std::string perr;
        if (!from_json(item, p, perr)) {
          AIOS_LOG_WARN("backup policy skip: ", perr);
          continue;
        }
        out.push_back(std::move(p));
      }
    }
  } catch (const std::exception& e) {
    err = std::string("bad backup policies json: ") + e.what();
    return false;
  }
  return true;
}

bool BackupPolicyStore::save_locked(const std::vector<BackupPolicy>& policies,
                                    std::uint64_t expected_cas, std::string& err) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& p : policies) arr.push_back(to_json(p));
  nlohmann::json doc{{"aios_backup_policies", 1}, {"policies", arr}};
  return put_cas(objects_, oid(), doc.dump(), expected_cas, err);
}

std::vector<BackupPolicy> BackupPolicyStore::list() {
  std::lock_guard lock(mu_);
  std::vector<BackupPolicy> out;
  std::uint64_t cas = 0;
  std::string err;
  load_locked(out, cas, err);
  return out;
}

std::optional<BackupPolicy> BackupPolicyStore::get(const std::string& id) {
  for (const auto& p : list()) {
    if (p.id == id) return p;
  }
  return std::nullopt;
}

nlohmann::json BackupPolicyStore::list_json() {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& p : list()) arr.push_back(to_json(p));
  return {{"policies", arr}, {"oid", oid()}};
}

std::optional<BackupPolicy> BackupPolicyStore::upsert(BackupPolicy p, std::string& err) {
  if (!validate(p, err)) return std::nullopt;
  if (p.path.empty()) p.path = "/";
  std::lock_guard lock(mu_);
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::vector<BackupPolicy> cur;
    std::uint64_t cas = 0;
    if (!load_locked(cur, cas, err)) return std::nullopt;
    if (p.id.empty()) p.id = make_id();
    bool found = false;
    for (auto& existing : cur) {
      if (existing.id == p.id) {
        const auto keep_last = existing.last_run_ms;
        existing = p;
        existing.last_run_ms = keep_last;
        p = existing;
        found = true;
        break;
      }
    }
    if (!found) {
      p.last_run_ms = 0;
      cur.push_back(p);
    }
    if (!save_locked(cur, cas, err)) {
      if (err.find("conflict") == std::string::npos && err.find("cas") == std::string::npos &&
          err.find("precondition") == std::string::npos) {
        return std::nullopt;
      }
      continue;
    }
    return p;
  }
  err = "policy cas retry exhausted";
  return std::nullopt;
}

bool BackupPolicyStore::remove(const std::string& id, std::string& err) {
  if (id.empty()) {
    err = "id required";
    return false;
  }
  std::lock_guard lock(mu_);
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::vector<BackupPolicy> cur;
    std::uint64_t cas = 0;
    if (!load_locked(cur, cas, err)) return false;
    const auto before = cur.size();
    cur.erase(std::remove_if(cur.begin(), cur.end(),
                             [&](const BackupPolicy& p) { return p.id == id; }),
              cur.end());
    if (cur.size() == before) {
      err = "not found";
      return false;
    }
    if (save_locked(cur, cas, err)) return true;
    if (err.find("conflict") == std::string::npos && err.find("cas") == std::string::npos &&
        err.find("precondition") == std::string::npos) {
      return false;
    }
  }
  err = "policy cas retry exhausted";
  return false;
}

bool BackupPolicyStore::touch_last_run(const std::string& id, std::int64_t last_run_ms,
                                       std::string& err) {
  std::lock_guard lock(mu_);
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::vector<BackupPolicy> cur;
    std::uint64_t cas = 0;
    if (!load_locked(cur, cas, err)) return false;
    bool found = false;
    for (auto& p : cur) {
      if (p.id == id) {
        p.last_run_ms = last_run_ms;
        found = true;
        break;
      }
    }
    if (!found) {
      err = "not found";
      return false;
    }
    if (save_locked(cur, cas, err)) return true;
    if (err.find("conflict") == std::string::npos && err.find("cas") == std::string::npos &&
        err.find("precondition") == std::string::npos) {
      return false;
    }
  }
  err = "policy cas retry exhausted";
  return false;
}

}  // namespace aios
