#include "http/space_history.hpp"

#include "cluster/lifecycle.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace aios {
namespace {

constexpr std::int64_t kMinSampleMs = 5LL * 60 * 1000;
constexpr std::int64_t kDayMs = 24LL * 60 * 60 * 1000;
constexpr std::int64_t kHourMs = 60LL * 60 * 1000;
constexpr std::int64_t kRecentKeepMs = kDayMs;
constexpr std::int64_t kHourlyKeepMs = 30LL * kDayMs;
constexpr std::int64_t kDailyKeepMs = 366LL * kDayMs;

std::uint64_t mul(std::uint64_t a, std::uint64_t b) {
  if (!a || !b) return 0;
  return a * b;
}

SpaceSample sample_from_entries(const std::vector<FsEntry>& entries, std::int64_t t_ms) {
  SpaceSample s;
  s.t_ms = t_ms;
  for (const auto& e : entries) {
    if (!e.usable) continue;
    const auto total = mul(e.bsize, e.blocks);
    const auto avail = mul(e.bsize, e.bavail);
    s.total_bytes += total;
    s.avail_bytes += avail;
    s.used_bytes += total > avail ? total - avail : 0;
  }
  return s;
}

nlohmann::json sample_json(const SpaceSample& s) {
  return {{"t", s.t_ms},
          {"total_bytes", s.total_bytes},
          {"used_bytes", s.used_bytes},
          {"avail_bytes", s.avail_bytes}};
}

SpaceSample sample_from_json(const nlohmann::json& j) {
  SpaceSample s;
  if (!j.is_object()) return s;
  s.t_ms = j.value("t", std::int64_t{0});
  s.total_bytes = j.value("total_bytes", std::uint64_t{0});
  s.used_bytes = j.value("used_bytes", std::uint64_t{0});
  s.avail_bytes = j.value("avail_bytes", std::uint64_t{0});
  return s;
}

std::vector<SpaceSample> load_series(const nlohmann::json& j, const char* key) {
  std::vector<SpaceSample> out;
  if (!j.contains(key) || !j[key].is_array()) return out;
  for (const auto& x : j[key]) out.push_back(sample_from_json(x));
  return out;
}

nlohmann::json dump_series(const std::vector<SpaceSample>& v) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& s : v) arr.push_back(sample_json(s));
  return arr;
}

void trim_older_than(std::vector<SpaceSample>& v, std::int64_t cutoff) {
  v.erase(std::remove_if(v.begin(), v.end(), [&](const SpaceSample& s) { return s.t_ms < cutoff; }),
          v.end());
}

void upsert_bucket(std::vector<SpaceSample>& v, const SpaceSample& s, std::int64_t bucket_ms) {
  const auto bucket = (s.t_ms / bucket_ms) * bucket_ms;
  SpaceSample stored = s;
  stored.t_ms = bucket;
  if (!v.empty() && (v.back().t_ms / bucket_ms) * bucket_ms == bucket) {
    v.back() = stored;
    return;
  }
  v.push_back(stored);
}

}  // namespace

std::string SpaceHistory::default_path(const std::string& status_file) {
  if (!status_file.empty()) {
    const fs::path p(status_file);
    const auto dir = p.parent_path();
    if (!dir.empty()) return (dir / "aios-space.json").string();
  }
  return "aios-space.json";
}

SpaceHistory::SpaceHistory(std::string path) : path_(std::move(path)) { load(); }

void SpaceHistory::load() {
  std::ifstream in(path_);
  if (!in) return;
  try {
    nlohmann::json j;
    in >> j;
    std::lock_guard<std::mutex> lock(mu_);
    recent_ = load_series(j, "recent");
    hourly_ = load_series(j, "hourly");
    daily_ = load_series(j, "daily");
    if (!recent_.empty()) last_recent_ms_ = recent_.back().t_ms;
  } catch (...) {
    AIOS_LOG_WARN("space history: ignored unreadable ", path_);
  }
}

void SpaceHistory::save_locked() const {
  if (path_.empty()) return;
  nlohmann::json j{{"recent", dump_series(recent_)},
                   {"hourly", dump_series(hourly_)},
                   {"daily", dump_series(daily_)}};
  const fs::path dest(path_);
  std::error_code ec;
  if (!dest.parent_path().empty()) fs::create_directories(dest.parent_path(), ec);
  const auto tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return;
    out << j.dump();
  }
  fs::rename(tmp, path_, ec);
  if (ec) {
    AIOS_LOG_WARN("space history: rename failed: ", ec.message());
  }
}

void SpaceHistory::maybe_record(const std::vector<FsEntry>& entries, const std::string& self_node_id) {
  const auto now = now_ms();
  std::lock_guard<std::mutex> lock(mu_);
  self_node_id_ = self_node_id;
  current_ = entries;
  if (last_recent_ms_ != 0 && now - last_recent_ms_ < kMinSampleMs) return;
  const auto s = sample_from_entries(entries, now);
  last_recent_ms_ = now;
  recent_.push_back(s);
  upsert_bucket(hourly_, s, kHourMs);
  upsert_bucket(daily_, s, kDayMs);
  trim_older_than(recent_, now - kRecentKeepMs);
  trim_older_than(hourly_, now - kHourlyKeepMs);
  trim_older_than(daily_, now - kDailyKeepMs);
  save_locked();
}

nlohmann::json SpaceHistory::current_json_locked() const {
  nlohmann::json disks = nlohmann::json::array();
  std::uint64_t total = 0, used = 0, avail = 0;
  for (const auto& e : current_) {
    const auto t = mul(e.bsize, e.blocks);
    const auto a = mul(e.bsize, e.bavail);
    const auto u = t > a ? t - a : 0;
    if (e.usable) {
      total += t;
      used += u;
      avail += a;
    }
    disks.push_back({
        {"node_id", e.node_id},
        {"mount", e.mount},
        {"aios_path", e.aios_path},
        {"storage_class", e.storage_class},
        {"rack", e.rack},
        {"state", lifecycle_state_name(e.state)},
        {"usable", e.usable},
        {"self", !self_node_id_.empty() && e.node_id == self_node_id_},
        {"total_bytes", t},
        {"used_bytes", u},
        {"avail_bytes", a},
    });
  }
  std::sort(disks.begin(), disks.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    const int as = a.value("self", false) ? 0 : 1;
    const int bs = b.value("self", false) ? 0 : 1;
    if (as != bs) return as < bs;
    if (a["node_id"] != b["node_id"]) return a["node_id"] < b["node_id"];
    return a["mount"] < b["mount"];
  });

  nlohmann::json by_host = nlohmann::json::object();
  nlohmann::json by_class = nlohmann::json::object();
  for (const auto& d : disks) {
    if (!d.value("usable", false)) continue;
    const auto host = d.value("node_id", std::string("—"));
    const auto cls = d.value("storage_class", std::string("—"));
    auto add = [&](nlohmann::json& m, const std::string& k) {
      if (!m.contains(k)) {
        m[k] = {{"total_bytes", 0}, {"used_bytes", 0}, {"avail_bytes", 0}, {"disks", 0}};
      }
      m[k]["total_bytes"] = m[k]["total_bytes"].get<std::uint64_t>() +
                            d.value("total_bytes", std::uint64_t{0});
      m[k]["used_bytes"] =
          m[k]["used_bytes"].get<std::uint64_t>() + d.value("used_bytes", std::uint64_t{0});
      m[k]["avail_bytes"] =
          m[k]["avail_bytes"].get<std::uint64_t>() + d.value("avail_bytes", std::uint64_t{0});
      m[k]["disks"] = m[k]["disks"].get<int>() + 1;
    };
    add(by_host, host);
    add(by_class, cls);
  }

  auto obj_to_rows = [](const nlohmann::json& m, const char* key_name) {
    nlohmann::json rows = nlohmann::json::array();
    for (auto it = m.begin(); it != m.end(); ++it) {
      auto row = it.value();
      row[key_name] = it.key();
      rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
      return a.value("used_bytes", std::uint64_t{0}) > b.value("used_bytes", std::uint64_t{0});
    });
    return rows;
  };

  const double pct = total > 0 ? (100.0 * static_cast<double>(used) / static_cast<double>(total)) : 0;
  return {{"totals",
           {{"total_bytes", total},
            {"used_bytes", used},
            {"avail_bytes", avail},
            {"used_pct", pct}}},
          {"disks", std::move(disks)},
          {"by_host", obj_to_rows(by_host, "node_id")},
          {"by_class", obj_to_rows(by_class, "storage_class")}};
}

nlohmann::json SpaceHistory::to_json() const {
  std::lock_guard<std::mutex> lock(mu_);
  auto j = current_json_locked();
  j["history"] = {{"recent", dump_series(recent_)},
                  {"hourly", dump_series(hourly_)},
                  {"daily", dump_series(daily_)}};
  return j;
}

}  // namespace aios
