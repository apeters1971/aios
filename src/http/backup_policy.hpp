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

struct BackupPolicy {
  std::string id;
  bool enabled{true};
  std::string kind{"posix"};  // posix | vbd
  std::string volume;
  std::string path{"/"};  // posix subtree; "/" = whole volume
  std::string pool;
  std::string name;
  std::string at{"00:00"};  // HH:MM UTC daily
  std::string tz{"UTC"};
  int keep_days{7};
  int keep_monthly{12};
  std::string from;
  std::string staging_class{"archive"};
  std::uint64_t max_bag_bytes{0};
  int max_members{0};
  std::string tape_sink;
  std::string tape_root;
  std::string tape_uri_prefix;
  std::string tape_bin;
  std::string tape_s3_endpoint;
  std::string tape_put_cmd;
  std::string tape_get_cmd;
  std::int64_t last_run_ms{0};
};

// Cluster-shared live backup policies (CAS object oid backup/policies).
class BackupPolicyStore {
 public:
  BackupPolicyStore(Config cfg, ObjectService& objects);

  static std::string oid();

  std::vector<BackupPolicy> list();
  std::optional<BackupPolicy> get(const std::string& id);
  // Create or update. Empty id → generate. Returns stored policy or nullopt + err.
  std::optional<BackupPolicy> upsert(BackupPolicy p, std::string& err);
  bool remove(const std::string& id, std::string& err);
  // CAS-update last_run_ms for id after a successful scheduled run.
  bool touch_last_run(const std::string& id, std::int64_t last_run_ms, std::string& err);

  nlohmann::json list_json();
  static nlohmann::json to_json(const BackupPolicy& p);
  static bool from_json(const nlohmann::json& j, BackupPolicy& p, std::string& err);
  static bool validate(const BackupPolicy& p, std::string& err);
  static bool parse_at(const std::string& at, int& hour, int& minute, std::string& err);
  // True if a daily UTC trigger at HH:MM has occurred after last_run_ms and at/before now_ms.
  static bool is_due(const BackupPolicy& p, std::int64_t now_ms);

 private:
  bool load_locked(std::vector<BackupPolicy>& out, std::uint64_t& cas, std::string& err);
  bool save_locked(const std::vector<BackupPolicy>& policies, std::uint64_t expected_cas,
                   std::string& err);
  static std::string make_id();

  Config cfg_;
  ObjectService& objects_;
  std::mutex mu_;
};

}  // namespace aios
