#pragma once

#include "client/session.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace aios {
namespace posix {

inline constexpr uint32_t kVolumeProjectId = 0;

struct QuotaIdLimits {
  std::optional<std::uint64_t> bytes;  // nullopt = unlimited
};

struct QuotaProjectLimits {
  std::string name;
  std::uint64_t root_ino{0};
  std::optional<std::uint64_t> bytes;  // project total
  std::unordered_map<std::uint32_t, QuotaIdLimits> uids;
  std::unordered_map<std::uint32_t, QuotaIdLimits> gids;
};

struct QuotaLimits {
  std::unordered_map<std::uint32_t, QuotaIdLimits> volume_uids;
  std::unordered_map<std::uint32_t, QuotaIdLimits> volume_gids;
  std::unordered_map<std::uint32_t, QuotaProjectLimits> projects;
  std::uint64_t cas{0};
};

struct QuotaProjectUsage {
  std::int64_t bytes{0};
  std::unordered_map<std::uint32_t, std::int64_t> uids;
  std::unordered_map<std::uint32_t, std::int64_t> gids;
};

struct QuotaUsage {
  std::unordered_map<std::uint32_t, std::int64_t> volume_uids;
  std::unordered_map<std::uint32_t, std::int64_t> volume_gids;
  std::unordered_map<std::uint32_t, QuotaProjectUsage> projects;
  std::uint64_t cas{0};
  std::uint64_t epoch{0};
};

std::string quota_limits_oid(const std::string& volume);
std::string quota_usage_oid(const std::string& volume);

QuotaLimits parse_quota_limits(const std::string& body, std::uint64_t cas);
QuotaUsage parse_quota_usage(const std::string& body, std::uint64_t cas);
std::string serialize_quota_limits(const QuotaLimits& lim);
std::string serialize_quota_usage(const QuotaUsage& u);

// Soft quota ledger: local atomics + periodic Session flush to cluster objects.
class QuotaLedger {
 public:
  QuotaLedger(Session& session, std::string volume);

  bool may_grow(std::uint32_t project_id, std::uint32_t uid, std::uint32_t gid,
                std::int64_t delta);
  void note_delta(std::uint32_t project_id, std::uint32_t uid, std::uint32_t gid,
                  std::int64_t delta);
  void note_chown(std::uint32_t project_id, std::uint32_t old_uid, std::uint32_t old_gid,
                  std::uint32_t new_uid, std::uint32_t new_gid, std::uint64_t size);
  void note_reproject(std::uint32_t old_proj, std::uint32_t new_proj, std::uint32_t uid,
                      std::uint32_t gid, std::uint64_t size);

  // Reload limits/usage from cluster if TTL expired; best-effort flush of pending deltas.
  void tick();
  void flush();
  void invalidate();

  QuotaLimits limits_snapshot();
  QuotaUsage usage_snapshot();  // durable + pending
  nlohmann::json to_admin_json();

  // Replace durable usage (reconcile). Clears pending.
  bool replace_usage(QuotaUsage u, std::string& err);

  bool load_limits(QuotaLimits& out, std::string& err);
  bool save_limits(const QuotaLimits& lim, std::string& err);

 private:
  bool ensure_loaded_locked(std::string& err);
  bool flush_locked(std::string& err);
  std::int64_t effective_uid(std::uint32_t project_id, std::uint32_t uid) const;
  std::int64_t effective_gid(std::uint32_t project_id, std::uint32_t gid) const;
  std::int64_t effective_project_total(std::uint32_t project_id) const;

  Session& session_;
  std::string volume_;
  std::mutex mu_;
  QuotaLimits limits_;
  QuotaUsage usage_;
  // Pending deltas (not yet flushed).
  std::unordered_map<std::uint32_t, std::int64_t> pend_vol_uids_;
  std::unordered_map<std::uint32_t, std::int64_t> pend_vol_gids_;
  std::unordered_map<std::uint32_t, QuotaProjectUsage> pend_projects_;
  std::int64_t cache_loaded_ms_{0};
  bool loaded_{false};
  std::int64_t last_flush_ms_{0};
  std::int64_t pending_bytes_{0};
};

}  // namespace posix
}  // namespace aios
