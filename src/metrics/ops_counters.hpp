#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <string>

namespace aios {

// Process-local OPS counters (monotonic). Aggregate cluster-wide by scraping
// admin nodes' /admin/ops (or via the admin console).
struct OpsCounters {
  std::atomic<std::uint64_t> http_requests{0};

  std::atomic<std::uint64_t> put{0};
  std::atomic<std::uint64_t> put_range{0};
  std::atomic<std::uint64_t> append{0};
  std::atomic<std::uint64_t> get{0};
  std::atomic<std::uint64_t> head{0};
  std::atomic<std::uint64_t> del{0};
  std::atomic<std::uint64_t> list{0};

  std::atomic<std::uint64_t> put_bytes{0};
  std::atomic<std::uint64_t> get_bytes{0};
  std::atomic<std::uint64_t> append_bytes{0};

  std::atomic<std::uint64_t> lock_acquire{0};
  std::atomic<std::uint64_t> watch{0};
  std::atomic<std::uint64_t> pubsub_publish{0};

  std::atomic<std::uint64_t> gossip_rounds{0};
  std::atomic<std::uint64_t> repair_scanned{0};
  std::atomic<std::uint64_t> repair_repaired{0};
  std::atomic<std::uint64_t> repair_failed{0};

  std::atomic<std::uint64_t> errors{0};

  OpsCounters() = default;
  OpsCounters(const OpsCounters&) = delete;
  OpsCounters& operator=(const OpsCounters&) = delete;

  nlohmann::json to_json() const;
  // OpenMetrics / Prometheus text exposition.
  std::string to_prometheus(const std::string& node_id) const;

  void load_json(const nlohmann::json& j);
  void add_from(const OpsCounters& o);
};

}  // namespace aios
