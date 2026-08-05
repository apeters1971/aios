#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

// Process-local OPS counters (monotonic).
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

  // Whole-object compression accounting (logical vs stored payload bytes).
  std::atomic<std::uint64_t> compress_puts{0};
  std::atomic<std::uint64_t> compress_skipped{0};
  std::atomic<std::uint64_t> compress_logical_bytes{0};
  std::atomic<std::uint64_t> compress_stored_bytes{0};

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
  void load_json(const nlohmann::json& j);
  void add_from(const OpsCounters& o);
};

// Node totals plus optional per-app-label counters (see AppLabelScope).
// Cluster/system events (gossip, repair) update totals only.
class OpsRegistry {
 public:
  OpsCounters& total() { return total_; }
  const OpsCounters& total() const { return total_; }

  void note_http_request();
  void note_put(std::uint64_t bytes);
  void note_put_range(std::uint64_t bytes);
  void note_append(std::uint64_t bytes);
  void note_get(std::uint64_t bytes);
  void note_compress(std::uint64_t logical_bytes, std::uint64_t stored_bytes);
  void note_compress_skipped();
  // Undo a get count and record a head instead (HTTP HEAD path).
  void note_reclass_get_to_head(std::uint64_t get_bytes);
  void note_del();
  void note_list();
  void note_lock_acquire();
  void note_watch();
  void note_pubsub_publish();
  void note_error();

  void note_gossip_round();
  void note_repair(std::uint64_t scanned, std::uint64_t repaired, std::uint64_t failed);

  // Totals only (backward compatible shape of former OpsCounters::to_json).
  nlohmann::json to_json() const { return total_.to_json(); }
  nlohmann::json by_label_json() const;
  // Full admin payload: totals + ops_by_label.
  nlohmann::json to_admin_json() const;
  std::string to_prometheus(const std::string& node_id) const;

 private:
  OpsCounters* label_bucket();  // nullptr if unlabeled

  OpsCounters total_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<OpsCounters>> by_label_;
};

}  // namespace aios
