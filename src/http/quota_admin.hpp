#pragma once

#include "config.hpp"
#include "object/object_service.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace aios {

// Admin-side soft quota control (limits/usage CAS objects + inode project_id).
class QuotaAdminStore {
 public:
  QuotaAdminStore(Config cfg, ObjectService& objects);

  std::string volume() const;

  nlohmann::json show();
  bool set_volume_uid_limit(std::uint32_t uid, std::optional<std::uint64_t> bytes, std::string& err);
  bool set_volume_gid_limit(std::uint32_t gid, std::optional<std::uint64_t> bytes, std::string& err);
  bool create_project(const std::string& name, std::uint64_t root_ino,
                      std::optional<std::uint64_t> bytes, std::uint32_t& id_out, std::string& err);
  bool set_project_uid_limit(std::uint32_t project_id, std::uint32_t uid,
                             std::optional<std::uint64_t> bytes, std::string& err);
  bool delete_project(std::uint32_t project_id, std::string& err);
  bool reconcile(std::string& err);

 private:
  bool load_limits_json(nlohmann::json& j, std::uint64_t& cas, std::string& err);
  bool save_limits_json(const nlohmann::json& j, std::uint64_t cas, std::string& err);
  bool load_usage_json(nlohmann::json& j, std::uint64_t& cas, std::string& err);
  bool save_usage_json(const nlohmann::json& j, std::uint64_t cas, std::string& err);
  bool set_inode_project(std::uint64_t ino, std::uint32_t project_id, std::string& err);
  bool clear_inode_project_if(std::uint64_t ino, std::uint32_t project_id, std::string& err);

  Config cfg_;
  ObjectService& objects_;
  std::mutex mu_;
};

}  // namespace aios
