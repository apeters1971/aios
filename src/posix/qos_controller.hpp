#pragma once

#include "client/session.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace aios {
namespace posix {

struct QosIdLimits {
  std::optional<std::uint64_t> iops;  // ops / second
  std::optional<std::uint64_t> bps;   // bytes / second
};

struct QosProjectLimits {
  std::optional<std::uint64_t> iops;
  std::optional<std::uint64_t> bps;
  std::unordered_map<std::uint32_t, QosIdLimits> uids;
  std::unordered_map<std::uint32_t, QosIdLimits> gids;
};

struct QosLimits {
  std::unordered_map<std::uint32_t, QosIdLimits> volume_uids;
  std::unordered_map<std::uint32_t, QosIdLimits> volume_gids;
  std::unordered_map<std::uint32_t, QosProjectLimits> projects;
  std::uint64_t cas{0};
};

std::string qos_limits_oid(const std::string& volume);
QosLimits parse_qos_limits(const std::string& body, std::uint64_t cas);
std::string serialize_qos_limits(const QosLimits& lim);

// Soft node-local IOPS / bandwidth QoS for posix (FUSE + S3).
// Token buckets per volume uid/gid and optional project domain (same project_id as quotas).
class QosController {
 public:
  QosController(Session& session, std::string volume);

  // Admit one logical I/O of `bytes`. Returns false → caller should return -EAGAIN.
  bool admit(std::uint32_t project_id, std::uint32_t uid, std::uint32_t gid, std::uint64_t bytes);

  void invalidate();
  QosLimits limits_snapshot();

 private:
  struct Bucket {
    double tokens{0};
    double rate{0};   // tokens per second
    double burst{0};  // max tokens
    std::int64_t last_ms{0};
    bool enabled{false};
  };

  struct KeyBuckets {
    Bucket iops;
    Bucket bps;
  };

  bool ensure_loaded_locked(std::string& err);
  void configure_bucket(Bucket& b, std::optional<std::uint64_t> rate_per_sec, std::int64_t now);
  static double clamped_cost(const Bucket& b, double cost);
  bool try_consume(Bucket& b, double cost, std::int64_t now);
  bool check_id(KeyBuckets& kb, const QosIdLimits& lim, std::uint64_t bytes, std::int64_t now,
                bool consume);
  bool check_project_total(KeyBuckets& kb, const QosProjectLimits& lim, std::uint64_t bytes,
                           std::int64_t now, bool consume);

  Session& session_;
  std::string volume_;
  std::mutex mu_;
  QosLimits limits_;
  std::int64_t cache_loaded_ms_{0};
  bool loaded_{false};

  std::unordered_map<std::uint32_t, KeyBuckets> vol_uids_;
  std::unordered_map<std::uint32_t, KeyBuckets> vol_gids_;
  std::unordered_map<std::uint32_t, KeyBuckets> proj_totals_;
  std::unordered_map<std::uint64_t, KeyBuckets> proj_uids_;  // (pid<<32)|uid
  std::unordered_map<std::uint64_t, KeyBuckets> proj_gids_;
};

}  // namespace posix
}  // namespace aios
