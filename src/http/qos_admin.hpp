#pragma once

#include "config.hpp"
#include "object/object_service.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace aios {

// Admin-side soft QoS (IOPS / bandwidth) limits + monitoring rates.
class QosAdminStore {
 public:
  QosAdminStore(Config cfg, ObjectService& objects);

  std::string volume() const;

  nlohmann::json show();
  bool set_volume_uid(std::uint32_t uid, std::optional<std::uint64_t> iops,
                      std::optional<std::uint64_t> bps, bool clear, std::string& err);
  bool set_volume_gid(std::uint32_t gid, std::optional<std::uint64_t> iops,
                      std::optional<std::uint64_t> bps, bool clear, std::string& err);
  bool set_project(std::uint32_t project_id, std::optional<std::uint32_t> uid,
                   std::optional<std::uint64_t> iops, std::optional<std::uint64_t> bps, bool clear,
                   std::string& err);

 private:
  bool load_limits_json(nlohmann::json& j, std::uint64_t& cas, std::string& err);
  bool save_limits_json(const nlohmann::json& j, std::uint64_t cas, std::string& err);
  nlohmann::json monitoring_json();

  Config cfg_;
  ObjectService& objects_;
  std::mutex mu_;

  // Prior OPS snapshot for node-level rate display.
  std::int64_t ops_sample_ms_{0};
  std::uint64_t sample_put_{0};
  std::uint64_t sample_get_{0};
  std::uint64_t sample_put_bytes_{0};
  std::uint64_t sample_get_bytes_{0};
};

}  // namespace aios
