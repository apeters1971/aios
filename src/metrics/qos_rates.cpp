#include "metrics/qos_rates.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace aios {
namespace {

constexpr std::int64_t kRateWindowMs = 1000;

std::int64_t now_ms() {
  using clock = std::chrono::system_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
      .count();
}

struct RateSample {
  std::int64_t window_start_ms{0};
  std::uint64_t ops{0};
  std::uint64_t bytes{0};
  double rate_iops{0};
  double rate_bps{0};
};

std::mutex g_mu;
std::unordered_map<std::string, RateSample> g_rates;

void note_key(const std::string& key, std::uint64_t bytes, std::int64_t now) {
  auto& s = g_rates[key];
  if (s.window_start_ms == 0) s.window_start_ms = now;
  if (now - s.window_start_ms >= kRateWindowMs) {
    const double secs = std::max(0.001, (now - s.window_start_ms) / 1000.0);
    s.rate_iops = static_cast<double>(s.ops) / secs;
    s.rate_bps = static_cast<double>(s.bytes) / secs;
    s.ops = 0;
    s.bytes = 0;
    s.window_start_ms = now;
  }
  s.ops += 1;
  s.bytes += bytes;
}

}  // namespace

void qos_note_observed(const std::string& volume, std::uint32_t project_id, std::uint32_t uid,
                       std::uint32_t gid, std::uint64_t bytes) {
  const auto now = now_ms();
  std::lock_guard lock(g_mu);
  note_key(volume + "|uid:" + std::to_string(uid), bytes, now);
  note_key(volume + "|gid:" + std::to_string(gid), bytes, now);
  if (project_id != 0) note_key(volume + "|proj:" + std::to_string(project_id), bytes, now);
}

nlohmann::json qos_observed_rates_json(const std::string& volume) {
  std::lock_guard lock(g_mu);
  const auto now = now_ms();
  nlohmann::json uids = nlohmann::json::array();
  nlohmann::json gids = nlohmann::json::array();
  nlohmann::json projects = nlohmann::json::array();
  const std::string pref = volume + "|";
  for (auto& [key, s] : g_rates) {
    if (key.rfind(pref, 0) != 0) continue;
    if (s.window_start_ms && now - s.window_start_ms >= kRateWindowMs) {
      const double secs = std::max(0.001, (now - s.window_start_ms) / 1000.0);
      s.rate_iops = static_cast<double>(s.ops) / secs;
      s.rate_bps = static_cast<double>(s.bytes) / secs;
      s.ops = 0;
      s.bytes = 0;
      s.window_start_ms = now;
    }
    const auto rest = key.substr(pref.size());
    if (rest.rfind("uid:", 0) == 0) {
      uids.push_back({{"uid", std::stoul(rest.substr(4))},
                      {"rate_iops", s.rate_iops},
                      {"rate_bps", s.rate_bps}});
    } else if (rest.rfind("gid:", 0) == 0) {
      gids.push_back({{"gid", std::stoul(rest.substr(4))},
                      {"rate_iops", s.rate_iops},
                      {"rate_bps", s.rate_bps}});
    } else if (rest.rfind("proj:", 0) == 0) {
      projects.push_back({{"id", std::stoul(rest.substr(5))},
                          {"rate_iops", s.rate_iops},
                          {"rate_bps", s.rate_bps}});
    }
  }
  return {{"volume_uids", uids}, {"volume_gids", gids}, {"projects", projects}};
}

}  // namespace aios
