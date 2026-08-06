#pragma once

#include "config.hpp"
#include "object/object_service.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aios {

// Cluster-shared VBD volume catalog (CAS object vd/registry).
// Populated by aios-vd create/clone/rename/resize and backup_snapshot_vbd.
struct VbdVolume {
  std::string pool;
  std::string name;
  std::uint64_t size{0};
  std::uint32_t obj_order{22};
  bool sealed{false};
  std::optional<std::string> parent_pool;
  std::optional<std::string> parent_name;
  std::optional<std::string> snapshot_of;
  std::int64_t created_ms{0};
};

class VbdRegistryStore {
 public:
  VbdRegistryStore(Config cfg, ObjectService& objects);

  static std::string oid();

  std::vector<VbdVolume> list();
  std::optional<VbdVolume> get(const std::string& pool, const std::string& name);
  bool upsert(VbdVolume v, std::string& err);
  // Remove registry entry only (no object delete).
  bool remove(const std::string& pool, const std::string& name, std::string& err);
  bool rename(const std::string& old_pool, const std::string& old_name, VbdVolume neu,
              std::string& err);
  bool has_children(const std::string& pool, const std::string& name);

  // Safer delete: refuse sealed / COW children; delete tips under vd/{pool}/{name}/; drop entry.
  bool delete_volume(const std::string& pool, const std::string& name, std::string& err);

  nlohmann::json list_json();
  static nlohmann::json to_json(const VbdVolume& v);
  static bool from_json(const nlohmann::json& j, VbdVolume& v, std::string& err);
  static bool validate(const VbdVolume& v, std::string& err);

 private:
  bool load_locked(std::vector<VbdVolume>& out, std::uint64_t& cas, std::string& err);
  bool save_locked(const std::vector<VbdVolume>& volumes, std::uint64_t expected_cas,
                   std::string& err);
  static std::size_t find_index(const std::vector<VbdVolume>& vols, const std::string& pool,
                                const std::string& name);

  Config cfg_;
  ObjectService& objects_;
  std::mutex mu_;
};

}  // namespace aios
