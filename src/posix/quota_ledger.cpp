#include "posix/quota_ledger.hpp"

#include "util/log.hpp"

#include <algorithm>
#include <chrono>

namespace aios {
namespace posix {
namespace {

constexpr std::int64_t kCacheTtlMs = 5000;
constexpr std::int64_t kFlushIntervalMs = 2000;
constexpr std::int64_t kFlushBytesThreshold = 4 * 1024 * 1024;

std::int64_t now_ms() {
  using clock = std::chrono::system_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
      .count();
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

void add_map(std::unordered_map<std::uint32_t, std::int64_t>& m, std::uint32_t k, std::int64_t d) {
  if (d == 0) return;
  m[k] += d;
}

void merge_project(QuotaProjectUsage& dst, const QuotaProjectUsage& src) {
  dst.bytes += src.bytes;
  for (const auto& [k, v] : src.uids) add_map(dst.uids, k, v);
  for (const auto& [k, v] : src.gids) add_map(dst.gids, k, v);
}

std::unordered_map<std::uint32_t, QuotaIdLimits> parse_id_limits(const nlohmann::json& j) {
  std::unordered_map<std::uint32_t, QuotaIdLimits> out;
  if (!j.is_object()) return out;
  for (auto it = j.begin(); it != j.end(); ++it) {
    try {
      auto id = static_cast<std::uint32_t>(std::stoul(it.key()));
      QuotaIdLimits lim;
      if (it.value().is_object() && it.value().contains("bytes") &&
          !it.value()["bytes"].is_null()) {
        lim.bytes = it.value()["bytes"].get<std::uint64_t>();
      } else if (it.value().is_number_unsigned() || it.value().is_number_integer()) {
        lim.bytes = it.value().get<std::uint64_t>();
      }
      out[id] = lim;
    } catch (...) {
    }
  }
  return out;
}

nlohmann::json id_limits_to_json(const std::unordered_map<std::uint32_t, QuotaIdLimits>& m) {
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [id, lim] : m) {
    if (lim.bytes) j[std::to_string(id)] = {{"bytes", *lim.bytes}};
  }
  return j;
}

std::unordered_map<std::uint32_t, std::int64_t> parse_usage_map(const nlohmann::json& j) {
  std::unordered_map<std::uint32_t, std::int64_t> out;
  if (!j.is_object()) return out;
  for (auto it = j.begin(); it != j.end(); ++it) {
    try {
      out[static_cast<std::uint32_t>(std::stoul(it.key()))] = it.value().get<std::int64_t>();
    } catch (...) {
    }
  }
  return out;
}

nlohmann::json usage_map_to_json(const std::unordered_map<std::uint32_t, std::int64_t>& m) {
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [id, v] : m) {
    if (v != 0) j[std::to_string(id)] = v;
  }
  return j;
}

bool put_cas(Session& session, const std::string& oid, const std::string& body,
             std::uint64_t expected_cas, std::uint64_t& new_cas_out, std::string& err,
             bool& conflict_out) {
  conflict_out = false;
  try {
    const std::uint64_t new_cas = expected_cas + 1;
    std::unordered_map<std::string, std::string> attrs{{"aios.posix.cas", std::to_string(new_cas)}};
    std::optional<std::uint64_t> expect = expected_cas;
    if (expected_cas == 0) {
      auto head = session.head_object(oid);
      if (!head.exists) {
        expect = 0;
      } else if (cas_from_attrs(head.attrs) == 0) {
        expect = 0;
      } else {
        err = "cas mismatch";
        conflict_out = true;
        return false;
      }
    }
    new_cas_out = session.put_bytes(oid, body, attrs, expect);
    if (new_cas_out == 0) new_cas_out = new_cas;
    return true;
  } catch (const client_error& e) {
    err = e.code() + ": " + e.what();
    conflict_out = e.code() == "conflict";
    return false;
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

}  // namespace

std::string quota_limits_oid(const std::string& volume) {
  return "quota/" + (volume.empty() ? std::string("default") : volume) + "/limits";
}

std::string quota_usage_oid(const std::string& volume) {
  return "quota/" + (volume.empty() ? std::string("default") : volume) + "/usage";
}

QuotaLimits parse_quota_limits(const std::string& body, std::uint64_t cas) {
  QuotaLimits lim;
  lim.cas = cas;
  if (body.empty()) return lim;
  auto j = nlohmann::json::parse(body);
  if (j.contains("volume") && j["volume"].is_object()) {
    lim.volume_uids = parse_id_limits(j["volume"].value("uids", nlohmann::json::object()));
    lim.volume_gids = parse_id_limits(j["volume"].value("gids", nlohmann::json::object()));
  }
  if (j.contains("projects") && j["projects"].is_object()) {
    for (auto it = j["projects"].begin(); it != j["projects"].end(); ++it) {
      try {
        auto pid = static_cast<std::uint32_t>(std::stoul(it.key()));
        if (pid == 0) continue;
        QuotaProjectLimits p;
        p.name = it.value().value("name", "");
        p.root_ino = it.value().value("root_ino", static_cast<std::uint64_t>(0));
        if (it.value().contains("bytes") && !it.value()["bytes"].is_null()) {
          p.bytes = it.value()["bytes"].get<std::uint64_t>();
        }
        p.uids = parse_id_limits(it.value().value("uids", nlohmann::json::object()));
        p.gids = parse_id_limits(it.value().value("gids", nlohmann::json::object()));
        lim.projects[pid] = std::move(p);
      } catch (...) {
      }
    }
  }
  return lim;
}

QuotaUsage parse_quota_usage(const std::string& body, std::uint64_t cas) {
  QuotaUsage u;
  u.cas = cas;
  if (body.empty()) return u;
  auto j = nlohmann::json::parse(body);
  u.epoch = j.value("epoch", static_cast<std::uint64_t>(0));
  if (j.contains("volume") && j["volume"].is_object()) {
    u.volume_uids = parse_usage_map(j["volume"].value("uids", nlohmann::json::object()));
    u.volume_gids = parse_usage_map(j["volume"].value("gids", nlohmann::json::object()));
  }
  if (j.contains("projects") && j["projects"].is_object()) {
    for (auto it = j["projects"].begin(); it != j["projects"].end(); ++it) {
      try {
        auto pid = static_cast<std::uint32_t>(std::stoul(it.key()));
        QuotaProjectUsage pu;
        pu.bytes = it.value().value("bytes", static_cast<std::int64_t>(0));
        pu.uids = parse_usage_map(it.value().value("uids", nlohmann::json::object()));
        pu.gids = parse_usage_map(it.value().value("gids", nlohmann::json::object()));
        u.projects[pid] = std::move(pu);
      } catch (...) {
      }
    }
  }
  return u;
}

std::string serialize_quota_limits(const QuotaLimits& lim) {
  nlohmann::json projects = nlohmann::json::object();
  for (const auto& [pid, p] : lim.projects) {
    nlohmann::json pj{{"name", p.name},
                      {"root_ino", p.root_ino},
                      {"uids", id_limits_to_json(p.uids)},
                      {"gids", id_limits_to_json(p.gids)}};
    if (p.bytes) pj["bytes"] = *p.bytes;
    projects[std::to_string(pid)] = pj;
  }
  nlohmann::json j{{"volume",
                    {{"uids", id_limits_to_json(lim.volume_uids)},
                     {"gids", id_limits_to_json(lim.volume_gids)}}},
                   {"projects", projects}};
  return j.dump();
}

std::string serialize_quota_usage(const QuotaUsage& u) {
  nlohmann::json projects = nlohmann::json::object();
  for (const auto& [pid, p] : u.projects) {
    projects[std::to_string(pid)] = {{"bytes", p.bytes},
                                     {"uids", usage_map_to_json(p.uids)},
                                     {"gids", usage_map_to_json(p.gids)}};
  }
  nlohmann::json j{{"epoch", u.epoch},
                   {"volume",
                    {{"uids", usage_map_to_json(u.volume_uids)},
                     {"gids", usage_map_to_json(u.volume_gids)}}},
                   {"projects", projects}};
  return j.dump();
}

QuotaLedger::QuotaLedger(Session& session, std::string volume)
    : session_(session), volume_(std::move(volume)) {}

void QuotaLedger::invalidate() {
  std::lock_guard lock(mu_);
  loaded_ = false;
}

bool QuotaLedger::ensure_loaded_locked(std::string& err) {
  const auto now = now_ms();
  if (loaded_ && now - cache_loaded_ms_ < kCacheTtlMs) return true;
  try {
    auto lim_snap = session_.get_object(quota_limits_oid(volume_));
    if (lim_snap.exists) {
      limits_ = parse_quota_limits(lim_snap.body, cas_from_attrs(lim_snap.attrs));
    } else {
      limits_ = QuotaLimits{};
    }
    auto use_snap = session_.get_object(quota_usage_oid(volume_));
    if (use_snap.exists) {
      // Keep pending; refresh base usage.
      usage_ = parse_quota_usage(use_snap.body, cas_from_attrs(use_snap.attrs));
    } else if (!loaded_) {
      usage_ = QuotaUsage{};
    }
    loaded_ = true;
    cache_loaded_ms_ = now;
    return true;
  } catch (const client_error& e) {
    err = e.code() + ": " + e.what();
    if (!loaded_) {
      limits_ = QuotaLimits{};
      usage_ = QuotaUsage{};
      loaded_ = true;
      cache_loaded_ms_ = now;
      return true;  // soft: empty limits
    }
    return false;
  } catch (const std::exception& e) {
    err = e.what();
    return loaded_;
  }
}

std::int64_t QuotaLedger::effective_uid(std::uint32_t project_id, std::uint32_t uid) const {
  std::int64_t v = 0;
  if (auto it = usage_.volume_uids.find(uid); it != usage_.volume_uids.end()) v += it->second;
  if (auto it = pend_vol_uids_.find(uid); it != pend_vol_uids_.end()) v += it->second;
  if (project_id != 0) {
    if (auto pit = usage_.projects.find(project_id); pit != usage_.projects.end()) {
      if (auto it = pit->second.uids.find(uid); it != pit->second.uids.end()) v += it->second;
    }
    if (auto pit = pend_projects_.find(project_id); pit != pend_projects_.end()) {
      if (auto it = pit->second.uids.find(uid); it != pit->second.uids.end()) v += it->second;
    }
  }
  // Volume uid effective for volume check is only volume maps; project uid is separate.
  // This helper is used for project-uid check when project_id!=0 — fix callers.
  return v;
}

std::int64_t QuotaLedger::effective_gid(std::uint32_t project_id, std::uint32_t gid) const {
  std::int64_t v = 0;
  if (project_id == 0) {
    if (auto it = usage_.volume_gids.find(gid); it != usage_.volume_gids.end()) v += it->second;
    if (auto it = pend_vol_gids_.find(gid); it != pend_vol_gids_.end()) v += it->second;
  } else {
    if (auto pit = usage_.projects.find(project_id); pit != usage_.projects.end()) {
      if (auto it = pit->second.gids.find(gid); it != pit->second.gids.end()) v += it->second;
    }
    if (auto pit = pend_projects_.find(project_id); pit != pend_projects_.end()) {
      if (auto it = pit->second.gids.find(gid); it != pit->second.gids.end()) v += it->second;
    }
  }
  return v;
}

std::int64_t QuotaLedger::effective_project_total(std::uint32_t project_id) const {
  std::int64_t v = 0;
  if (auto it = usage_.projects.find(project_id); it != usage_.projects.end()) v += it->second.bytes;
  if (auto it = pend_projects_.find(project_id); it != pend_projects_.end()) v += it->second.bytes;
  return v;
}

bool QuotaLedger::may_grow(std::uint32_t project_id, std::uint32_t uid, std::uint32_t gid,
                           std::int64_t delta) {
  if (delta <= 0) return true;
  std::lock_guard lock(mu_);
  std::string err;
  ensure_loaded_locked(err);

  auto check_id = [&](const std::unordered_map<std::uint32_t, QuotaIdLimits>& lims,
                      std::uint32_t id, std::int64_t cur) -> bool {
    auto it = lims.find(id);
    if (it == lims.end() || !it->second.bytes) return true;
    return static_cast<std::uint64_t>(cur + delta) <= *it->second.bytes;
  };

  std::int64_t vol_uid = 0;
  if (auto it = usage_.volume_uids.find(uid); it != usage_.volume_uids.end()) vol_uid += it->second;
  if (auto it = pend_vol_uids_.find(uid); it != pend_vol_uids_.end()) vol_uid += it->second;
  if (!check_id(limits_.volume_uids, uid, vol_uid)) return false;

  std::int64_t vol_gid = 0;
  if (auto it = usage_.volume_gids.find(gid); it != usage_.volume_gids.end()) vol_gid += it->second;
  if (auto it = pend_vol_gids_.find(gid); it != pend_vol_gids_.end()) vol_gid += it->second;
  if (!check_id(limits_.volume_gids, gid, vol_gid)) return false;

  if (project_id != 0) {
    auto pit = limits_.projects.find(project_id);
    if (pit != limits_.projects.end()) {
      if (pit->second.bytes &&
          static_cast<std::uint64_t>(effective_project_total(project_id) + delta) >
              *pit->second.bytes) {
        return false;
      }
      std::int64_t puid = 0;
      if (auto uit = usage_.projects.find(project_id); uit != usage_.projects.end()) {
        if (auto it = uit->second.uids.find(uid); it != uit->second.uids.end()) puid += it->second;
      }
      if (auto uit = pend_projects_.find(project_id); uit != pend_projects_.end()) {
        if (auto it = uit->second.uids.find(uid); it != uit->second.uids.end()) puid += it->second;
      }
      if (!check_id(pit->second.uids, uid, puid)) return false;

      std::int64_t pgid = 0;
      if (auto uit = usage_.projects.find(project_id); uit != usage_.projects.end()) {
        if (auto it = uit->second.gids.find(gid); it != uit->second.gids.end()) pgid += it->second;
      }
      if (auto uit = pend_projects_.find(project_id); uit != pend_projects_.end()) {
        if (auto it = uit->second.gids.find(gid); it != uit->second.gids.end()) pgid += it->second;
      }
      if (!check_id(pit->second.gids, gid, pgid)) return false;
    }
  }
  return true;
}

void QuotaLedger::note_delta(std::uint32_t project_id, std::uint32_t uid, std::uint32_t gid,
                             std::int64_t delta) {
  if (delta == 0) return;
  std::lock_guard lock(mu_);
  std::string err;
  ensure_loaded_locked(err);
  add_map(pend_vol_uids_, uid, delta);
  add_map(pend_vol_gids_, gid, delta);
  if (project_id != 0) {
    auto& p = pend_projects_[project_id];
    p.bytes += delta;
    add_map(p.uids, uid, delta);
    add_map(p.gids, gid, delta);
  }
  pending_bytes_ += std::abs(delta);
  const auto now = now_ms();
  if (pending_bytes_ >= kFlushBytesThreshold || now - last_flush_ms_ >= kFlushIntervalMs) {
    flush_locked(err);
  }
}

void QuotaLedger::note_chown(std::uint32_t project_id, std::uint32_t old_uid,
                             std::uint32_t old_gid, std::uint32_t new_uid, std::uint32_t new_gid,
                             std::uint64_t size) {
  if (old_uid == new_uid && old_gid == new_gid) return;
  if (size == 0) return;
  const auto sz = static_cast<std::int64_t>(size);
  std::lock_guard lock(mu_);
  std::string err;
  ensure_loaded_locked(err);
  if (old_uid != new_uid) {
    add_map(pend_vol_uids_, old_uid, -sz);
    add_map(pend_vol_uids_, new_uid, sz);
    if (project_id != 0) {
      add_map(pend_projects_[project_id].uids, old_uid, -sz);
      add_map(pend_projects_[project_id].uids, new_uid, sz);
    }
  }
  if (old_gid != new_gid) {
    add_map(pend_vol_gids_, old_gid, -sz);
    add_map(pend_vol_gids_, new_gid, sz);
    if (project_id != 0) {
      add_map(pend_projects_[project_id].gids, old_gid, -sz);
      add_map(pend_projects_[project_id].gids, new_gid, sz);
    }
  }
  pending_bytes_ += sz;
}

void QuotaLedger::note_reproject(std::uint32_t old_proj, std::uint32_t new_proj, std::uint32_t uid,
                                 std::uint32_t gid, std::uint64_t size) {
  if (old_proj == new_proj || size == 0) return;
  const auto sz = static_cast<std::int64_t>(size);
  std::lock_guard lock(mu_);
  std::string err;
  ensure_loaded_locked(err);
  if (old_proj != 0) {
    auto& p = pend_projects_[old_proj];
    p.bytes -= sz;
    add_map(p.uids, uid, -sz);
    add_map(p.gids, gid, -sz);
  }
  if (new_proj != 0) {
    auto& p = pend_projects_[new_proj];
    p.bytes += sz;
    add_map(p.uids, uid, sz);
    add_map(p.gids, gid, sz);
  }
  pending_bytes_ += sz;
}

void QuotaLedger::tick() {
  std::lock_guard lock(mu_);
  std::string err;
  ensure_loaded_locked(err);
  const auto now = now_ms();
  if (pending_bytes_ > 0 && now - last_flush_ms_ >= kFlushIntervalMs) flush_locked(err);
}

void QuotaLedger::flush() {
  std::lock_guard lock(mu_);
  std::string err;
  flush_locked(err);
}

bool QuotaLedger::flush_locked(std::string& err) {
  if (pend_vol_uids_.empty() && pend_vol_gids_.empty() && pend_projects_.empty()) {
    last_flush_ms_ = now_ms();
    return true;
  }
  ensure_loaded_locked(err);
  for (int attempt = 0; attempt < 8; ++attempt) {
    // Refresh base usage cas
    try {
      auto use_snap = session_.get_object(quota_usage_oid(volume_));
      if (use_snap.exists) {
        usage_ = parse_quota_usage(use_snap.body, cas_from_attrs(use_snap.attrs));
      } else {
        usage_.cas = 0;
      }
    } catch (const client_error& e) {
      err = e.code() + ": " + e.what();
      return false;
    }

    QuotaUsage next = usage_;
    for (const auto& [k, v] : pend_vol_uids_) add_map(next.volume_uids, k, v);
    for (const auto& [k, v] : pend_vol_gids_) add_map(next.volume_gids, k, v);
    for (const auto& [pid, p] : pend_projects_) merge_project(next.projects[pid], p);
    next.epoch = usage_.epoch;

    std::uint64_t new_cas = 0;
    bool conflict = false;
    if (!put_cas(session_, quota_usage_oid(volume_), serialize_quota_usage(next), usage_.cas,
                 new_cas, err, conflict)) {
      if (conflict) continue;
      AIOS_LOG_WARN("quota flush failed: ", err);
      // Back off: without this a failing flush is retried on every write.
      last_flush_ms_ = now_ms();
      return false;
    }
    usage_ = next;
    usage_.cas = new_cas;
    pend_vol_uids_.clear();
    pend_vol_gids_.clear();
    pend_projects_.clear();
    pending_bytes_ = 0;
    last_flush_ms_ = now_ms();
    return true;
  }
  err = "quota flush cas retry exhausted";
  last_flush_ms_ = now_ms();
  return false;
}

QuotaLimits QuotaLedger::limits_snapshot() {
  std::lock_guard lock(mu_);
  std::string err;
  ensure_loaded_locked(err);
  return limits_;
}

QuotaUsage QuotaLedger::usage_snapshot() {
  std::lock_guard lock(mu_);
  std::string err;
  ensure_loaded_locked(err);
  QuotaUsage u = usage_;
  for (const auto& [k, v] : pend_vol_uids_) add_map(u.volume_uids, k, v);
  for (const auto& [k, v] : pend_vol_gids_) add_map(u.volume_gids, k, v);
  for (const auto& [pid, p] : pend_projects_) merge_project(u.projects[pid], p);
  return u;
}

nlohmann::json QuotaLedger::to_admin_json() {
  auto lim = limits_snapshot();
  auto use = usage_snapshot();
  nlohmann::json vol_uids = nlohmann::json::array();
  std::unordered_map<std::uint32_t, bool> seen;
  for (const auto& [id, l] : lim.volume_uids) {
    seen[id] = true;
    vol_uids.push_back({{"uid", id},
                        {"limit_bytes", l.bytes ? nlohmann::json(*l.bytes) : nullptr},
                        {"used_bytes", use.volume_uids.count(id) ? use.volume_uids[id] : 0}});
  }
  for (const auto& [id, used] : use.volume_uids) {
    if (seen.count(id)) continue;
    vol_uids.push_back({{"uid", id}, {"limit_bytes", nullptr}, {"used_bytes", used}});
  }
  nlohmann::json vol_gids = nlohmann::json::array();
  seen.clear();
  for (const auto& [id, l] : lim.volume_gids) {
    seen[id] = true;
    vol_gids.push_back({{"gid", id},
                        {"limit_bytes", l.bytes ? nlohmann::json(*l.bytes) : nullptr},
                        {"used_bytes", use.volume_gids.count(id) ? use.volume_gids[id] : 0}});
  }
  for (const auto& [id, used] : use.volume_gids) {
    if (seen.count(id)) continue;
    vol_gids.push_back({{"gid", id}, {"limit_bytes", nullptr}, {"used_bytes", used}});
  }
  nlohmann::json projects = nlohmann::json::array();
  for (const auto& [pid, p] : lim.projects) {
    std::int64_t used = 0;
    if (auto it = use.projects.find(pid); it != use.projects.end()) used = it->second.bytes;
    projects.push_back({{"id", pid},
                        {"name", p.name},
                        {"root_ino", p.root_ino},
                        {"limit_bytes", p.bytes ? nlohmann::json(*p.bytes) : nullptr},
                        {"used_bytes", used}});
  }
  return {{"volume", volume_},
          {"volume_uids", vol_uids},
          {"volume_gids", vol_gids},
          {"projects", projects}};
}

bool QuotaLedger::load_limits(QuotaLimits& out, std::string& err) {
  std::lock_guard lock(mu_);
  if (!ensure_loaded_locked(err)) return false;
  out = limits_;
  return true;
}

bool QuotaLedger::save_limits(const QuotaLimits& lim, std::string& err) {
  std::lock_guard lock(mu_);
  ensure_loaded_locked(err);
  for (int attempt = 0; attempt < 8; ++attempt) {
    try {
      auto snap = session_.get_object(quota_limits_oid(volume_));
      std::uint64_t cas = snap.exists ? cas_from_attrs(snap.attrs) : 0;
      if (snap.exists) limits_ = parse_quota_limits(snap.body, cas);
      std::uint64_t new_cas = 0;
      bool conflict = false;
      if (!put_cas(session_, quota_limits_oid(volume_), serialize_quota_limits(lim), cas, new_cas,
                   err, conflict)) {
        if (conflict) continue;
        return false;
      }
      limits_ = lim;
      limits_.cas = new_cas;
      cache_loaded_ms_ = now_ms();
      loaded_ = true;
      return true;
    } catch (const client_error& e) {
      err = e.code() + ": " + e.what();
      return false;
    }
  }
  err = "limits cas retry exhausted";
  return false;
}

bool QuotaLedger::replace_usage(QuotaUsage u, std::string& err) {
  std::lock_guard lock(mu_);
  ensure_loaded_locked(err);
  for (int attempt = 0; attempt < 8; ++attempt) {
    try {
      auto snap = session_.get_object(quota_usage_oid(volume_));
      std::uint64_t cas = snap.exists ? cas_from_attrs(snap.attrs) : 0;
      u.epoch = (snap.exists ? parse_quota_usage(snap.body, cas).epoch : 0) + 1;
      std::uint64_t new_cas = 0;
      bool conflict = false;
      if (!put_cas(session_, quota_usage_oid(volume_), serialize_quota_usage(u), cas, new_cas,
                   err, conflict)) {
        if (conflict) continue;
        return false;
      }
      usage_ = u;
      usage_.cas = new_cas;
      pend_vol_uids_.clear();
      pend_vol_gids_.clear();
      pend_projects_.clear();
      pending_bytes_ = 0;
      last_flush_ms_ = now_ms();
      return true;
    } catch (const client_error& e) {
      err = e.code() + ": " + e.what();
      return false;
    }
  }
  err = "usage replace cas retry exhausted";
  return false;
}

}  // namespace posix
}  // namespace aios
