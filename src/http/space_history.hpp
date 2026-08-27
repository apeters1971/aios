#pragma once

#include "fs/fs_table.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace aios {

struct SpaceSample {
  std::int64_t t_ms{0};
  std::uint64_t total_bytes{0};
  std::uint64_t used_bytes{0};
  std::uint64_t avail_bytes{0};
};

// Cluster disk usage snapshot plus a downsampled local history (24h / 30d / 1y).
class SpaceHistory {
 public:
  explicit SpaceHistory(std::string path);

  // Always refreshes the live snapshot. Appends history at most every 5 minutes.
  void maybe_record(const std::vector<FsEntry>& entries, const std::string& self_node_id);

  nlohmann::json to_json() const;

  static std::string default_path(const std::string& status_file);

 private:
  void load();
  void save_locked() const;
  nlohmann::json current_json_locked() const;

  std::string path_;
  mutable std::mutex mu_;
  std::string self_node_id_;
  std::vector<FsEntry> current_;
  std::vector<SpaceSample> recent_;  // ~5 min, 24h
  std::vector<SpaceSample> hourly_;  // 30d
  std::vector<SpaceSample> daily_;   // 366d
  std::int64_t last_recent_ms_{0};
};

}  // namespace aios
