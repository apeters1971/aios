#include "posix/qos_controller.hpp"

#include "metrics/qos_rates.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

namespace aios {
namespace posix {
namespace {

constexpr std::int64_t kCacheTtlMs = 5000;
// Bucket depth in seconds of rate. Depth must exceed the rate, otherwise a single
// I/O costing a full second of budget can never be admitted.
constexpr double kBurstSeconds = 2.0;

// Monotonic: a wall-clock step backwards must not stall refills.
std::int64_t now_ms() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
      .count();
}

QosIdLimits parse_id_limits(const nlohmann::json& j) {
  QosIdLimits lim;
  if (!j.is_object()) return lim;
  if (j.contains("iops") && !j["iops"].is_null()) lim.iops = j["iops"].get<std::uint64_t>();
  if (j.contains("bps") && !j["bps"].is_null()) lim.bps = j["bps"].get<std::uint64_t>();
  return lim;
}

nlohmann::json id_limits_to_json(const QosIdLimits& lim) {
  nlohmann::json j = nlohmann::json::object();
  if (lim.iops) j["iops"] = *lim.iops;
  if (lim.bps) j["bps"] = *lim.bps;
  return j;
}

nlohmann::json id_map_to_json(const std::unordered_map<std::uint32_t, QosIdLimits>& m) {
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [id, lim] : m) {
    auto o = id_limits_to_json(lim);
    if (!o.empty()) j[std::to_string(id)] = std::move(o);
  }
  return j;
}

std::unordered_map<std::uint32_t, QosIdLimits> parse_id_map(const nlohmann::json& j) {
  std::unordered_map<std::uint32_t, QosIdLimits> out;
  if (!j.is_object()) return out;
  for (auto it = j.begin(); it != j.end(); ++it) {
    try {
      out[static_cast<std::uint32_t>(std::stoul(it.key()))] = parse_id_limits(it.value());
    } catch (...) {
    }
  }
  return out;
}

}  // namespace

std::string qos_limits_oid(const std::string& volume) {
  return "qos/" + (volume.empty() ? std::string("default") : volume) + "/limits";
}

QosLimits parse_qos_limits(const std::string& body, std::uint64_t cas) {
  QosLimits lim;
  lim.cas = cas;
  if (body.empty()) return lim;
  auto j = nlohmann::json::parse(body);
  if (j.contains("volume") && j["volume"].is_object()) {
    lim.volume_uids = parse_id_map(j["volume"].value("uids", nlohmann::json::object()));
    lim.volume_gids = parse_id_map(j["volume"].value("gids", nlohmann::json::object()));
  }
  if (j.contains("projects") && j["projects"].is_object()) {
    for (auto it = j["projects"].begin(); it != j["projects"].end(); ++it) {
      try {
        auto pid = static_cast<std::uint32_t>(std::stoul(it.key()));
        if (pid == 0) continue;
        QosProjectLimits p;
        if (it.value().contains("iops") && !it.value()["iops"].is_null())
          p.iops = it.value()["iops"].get<std::uint64_t>();
        if (it.value().contains("bps") && !it.value()["bps"].is_null())
          p.bps = it.value()["bps"].get<std::uint64_t>();
        p.uids = parse_id_map(it.value().value("uids", nlohmann::json::object()));
        p.gids = parse_id_map(it.value().value("gids", nlohmann::json::object()));
        lim.projects[pid] = std::move(p);
      } catch (...) {
      }
    }
  }
  return lim;
}

std::string serialize_qos_limits(const QosLimits& lim) {
  nlohmann::json projects = nlohmann::json::object();
  for (const auto& [pid, p] : lim.projects) {
    nlohmann::json pj{{"uids", id_map_to_json(p.uids)}, {"gids", id_map_to_json(p.gids)}};
    if (p.iops) pj["iops"] = *p.iops;
    if (p.bps) pj["bps"] = *p.bps;
    projects[std::to_string(pid)] = pj;
  }
  nlohmann::json j{{"volume",
                    {{"uids", id_map_to_json(lim.volume_uids)},
                     {"gids", id_map_to_json(lim.volume_gids)}}},
                   {"projects", projects}};
  return j.dump();
}

QosController::QosController(Session& session, std::string volume)
    : session_(session), volume_(std::move(volume)) {}

void QosController::invalidate() {
  std::lock_guard lock(mu_);
  loaded_ = false;
}

QosLimits QosController::limits_snapshot() {
  std::lock_guard lock(mu_);
  std::string err;
  ensure_loaded_locked(err);
  return limits_;
}

void QosController::configure_bucket(Bucket& b, std::optional<std::uint64_t> rate_per_sec,
                                     std::int64_t now) {
  if (!rate_per_sec || *rate_per_sec == 0) {
    b.enabled = false;
    return;
  }
  const double rate = static_cast<double>(*rate_per_sec);
  if (!b.enabled || b.rate != rate) {
    b.rate = rate;
    b.burst = rate * kBurstSeconds;
    b.tokens = b.burst;
    b.last_ms = now;
    b.enabled = true;
    return;
  }
  const double elapsed = std::max(0.0, (now - b.last_ms) / 1000.0);
  b.tokens = std::min(b.burst, b.tokens + elapsed * b.rate);
  b.last_ms = now;
}

// An I/O larger than the bucket depth costs a full bucket rather than being
// rejected forever: it waits for a full bucket, then drains it.
double QosController::clamped_cost(const Bucket& b, double cost) {
  return std::min(cost, b.burst);
}

bool QosController::try_consume(Bucket& b, double cost, std::int64_t /*now*/) {
  if (!b.enabled) return true;
  const double need = clamped_cost(b, cost);
  if (b.tokens + 1e-9 < need) return false;
  b.tokens -= need;
  return true;
}

bool QosController::check_id(KeyBuckets& kb, const QosIdLimits& lim, std::uint64_t bytes,
                             std::int64_t now, bool consume) {
  configure_bucket(kb.iops, lim.iops, now);
  configure_bucket(kb.bps, lim.bps, now);
  if (!consume) {
    if (kb.iops.enabled && kb.iops.tokens + 1e-9 < clamped_cost(kb.iops, 1.0)) return false;
    if (kb.bps.enabled &&
        kb.bps.tokens + 1e-9 < clamped_cost(kb.bps, static_cast<double>(bytes))) {
      return false;
    }
    return true;
  }
  return try_consume(kb.iops, 1.0, now) && try_consume(kb.bps, static_cast<double>(bytes), now);
}

bool QosController::check_project_total(KeyBuckets& kb, const QosProjectLimits& lim,
                                        std::uint64_t bytes, std::int64_t now, bool consume) {
  QosIdLimits as_id;
  as_id.iops = lim.iops;
  as_id.bps = lim.bps;
  return check_id(kb, as_id, bytes, now, consume);
}

bool QosController::ensure_loaded_locked(std::string& err) {
  const auto now = now_ms();
  if (loaded_ && now - cache_loaded_ms_ < kCacheTtlMs) return true;
  try {
    auto snap = session_.get_object(qos_limits_oid(volume_));
    if (snap.exists) {
      limits_ = parse_qos_limits(snap.body, snap.cas);
    } else {
      limits_ = QosLimits{};
    }
    loaded_ = true;
    cache_loaded_ms_ = now;
    return true;
  } catch (const client_error& e) {
    err = e.code() + ": " + e.what();
    if (!loaded_) {
      limits_ = QosLimits{};
      loaded_ = true;
      cache_loaded_ms_ = now;
      return true;
    }
    return false;
  } catch (const std::exception& e) {
    err = e.what();
    return loaded_;
  }
}

bool QosController::admit(std::uint32_t project_id, std::uint32_t uid, std::uint32_t gid,
                          std::uint64_t bytes) {
  std::lock_guard lock(mu_);
  std::string err;
  ensure_loaded_locked(err);
  const auto now = now_ms();

  auto has_any = [](const QosIdLimits& l) { return l.iops || l.bps; };

  struct Step {
    KeyBuckets* kb;
    QosIdLimits lim;
  };
  std::vector<Step> steps;

  if (auto it = limits_.volume_uids.find(uid); it != limits_.volume_uids.end() && has_any(it->second)) {
    steps.push_back({&vol_uids_[uid], it->second});
  }
  if (auto it = limits_.volume_gids.find(gid); it != limits_.volume_gids.end() && has_any(it->second)) {
    steps.push_back({&vol_gids_[gid], it->second});
  }
  if (project_id != 0) {
    if (auto pit = limits_.projects.find(project_id); pit != limits_.projects.end()) {
      if (pit->second.iops || pit->second.bps) {
        QosIdLimits t;
        t.iops = pit->second.iops;
        t.bps = pit->second.bps;
        steps.push_back({&proj_totals_[project_id], t});
      }
      if (auto uit = pit->second.uids.find(uid);
          uit != pit->second.uids.end() && has_any(uit->second)) {
        const auto key = (static_cast<std::uint64_t>(project_id) << 32) | uid;
        steps.push_back({&proj_uids_[key], uit->second});
      }
      if (auto git = pit->second.gids.find(gid);
          git != pit->second.gids.end() && has_any(git->second)) {
        const auto key = (static_cast<std::uint64_t>(project_id) << 32) | gid;
        steps.push_back({&proj_gids_[key], git->second});
      }
    }
  }

  if (!steps.empty()) {
    for (auto& s : steps) {
      if (!check_id(*s.kb, s.lim, bytes, now, false)) return false;
    }
    for (auto& s : steps) {
      if (!check_id(*s.kb, s.lim, bytes, now, true)) return false;
    }
  }

  qos_note_observed(volume_, project_id, uid, gid, bytes);
  return true;
}

}  // namespace posix
}  // namespace aios
