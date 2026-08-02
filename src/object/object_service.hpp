#pragma once

#include "cluster/cluster_map.hpp"
#include "cluster/place.hpp"
#include "config.hpp"
#include "net/framing.hpp"
#include "store/local_stores.hpp"
#include "store/object_store.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

struct ApiResult {
  bool ok{false};
  std::string code;
  std::string error;
  std::uint64_t epoch{0};
  int replicas{0};
  std::optional<std::vector<std::uint8_t>> data;
  // When set, HTTP should stream this FS body instead of loading `data`.
  std::string body_path;
  std::optional<ObjectInfo> info;
  std::unordered_map<std::string, std::string> attrs;
  ObjectListResult list;
  std::vector<VersionInfo> versions;
  Placement placement;
  // Non-empty when tip (or selected version) is a redirect.
  std::string redirect_oid;
};

// Handles object RPC + primary replication (prepare → quorum install → publish tip).
class ObjectService {
 public:
  ObjectService(Config cfg, ClusterMap& map, LocalStores& stores);

  void set_advertise(std::string advertise);

  Frame handle(const Frame& req);

  ApiResult api_put(const std::string& oid, const std::uint8_t* data, std::size_t len,
                    const std::unordered_map<std::string, std::string>& attrs,
                    bool replace_attrs, const std::vector<AttrPrecondition>& preds,
                    std::optional<std::uint32_t> expected_crc32c = std::nullopt);
  // Staging file is moved into the store (FS-backed). Prefer for large bodies.
  ApiResult api_put_file(const std::string& oid, const std::string& staging_abs_path,
                         std::uint64_t size, std::uint32_t crc32c_val,
                         const std::unordered_map<std::string, std::string>& attrs,
                         bool replace_attrs, const std::vector<AttrPrecondition>& preds,
                         std::optional<std::uint32_t> expected_crc32c = std::nullopt);
  ApiResult api_put_redirect(const std::string& oid, const std::string& target_oid,
                             const std::unordered_map<std::string, std::string>& attrs,
                             bool replace_attrs, const std::vector<AttrPrecondition>& preds);
  ApiResult api_put_range(const std::string& oid, std::uint64_t offset,
                          const std::uint8_t* data, std::size_t len,
                          const std::unordered_map<std::string, std::string>& attrs,
                          bool replace_attrs, const std::vector<AttrPrecondition>& preds);
  ApiResult api_get(const std::string& oid, std::optional<std::uint64_t> offset,
                    std::optional<std::uint64_t> end_inclusive,
                    const std::vector<AttrPrecondition>& preds,
                    std::optional<std::uint64_t> seq = std::nullopt);
  ApiResult api_head(const std::string& oid, const std::vector<AttrPrecondition>& preds,
                     std::optional<std::uint64_t> seq = std::nullopt);
  ApiResult api_del(const std::string& oid, const std::vector<AttrPrecondition>& preds);
  // cluster=true scatter-gathers ObjectList across nodes; false = local stores only.
  ApiResult api_list(const std::string& prefix, const std::string& attr_eq_key,
                     const std::string& attr_eq_value, std::size_t limit,
                     const std::string& cursor, bool include_attrs, bool cluster = true);
  ApiResult api_list_versions(const std::string& oid);
  ApiResult api_purge_version(const std::string& oid, std::uint64_t seq, bool allow_tip);
  ApiResult api_trim_versions(const std::string& oid, int keep);

  ClusterMap& map() { return map_; }
  const ClusterMap& map() const { return map_; }
  LocalStores& stores() { return stores_; }

 private:
  Frame reply_ok(std::uint64_t epoch) const;
  Frame reply_err(std::uint64_t epoch, const std::string& code,
                  const std::string& error) const;
  Frame handle_put(const nlohmann::json& body);
  Frame handle_put_range(const Frame& req);
  Frame handle_get(const nlohmann::json& body);
  Frame handle_del(const nlohmann::json& body);
  Frame handle_stat(const nlohmann::json& body);
  Frame handle_publish_tip(const nlohmann::json& body);
  Frame handle_abort_version(const nlohmann::json& body);
  Frame handle_list_versions(const nlohmann::json& body);
  Frame handle_purge_versions(const nlohmann::json& body);
  Frame handle_stage_begin(const nlohmann::json& body);
  Frame handle_stage_data(const Frame& req);
  Frame handle_stage_commit(const nlohmann::json& body);
  Frame handle_list(const nlohmann::json& body);

  bool epoch_ok(std::uint64_t req_epoch, Frame& err_out) const;
  int quorum_need(const Placement& placement) const;
  ApiResult fail(const std::string& code, const std::string& error) const;
  ObjectStore* primary_store(const Placement& p, std::string& err);
  PrecondResult check_preds_on(ObjectStore* store, const std::string& oid,
                               const std::vector<AttrPrecondition>& preds, std::string& err);

  bool local_install(const std::string& aios_path, const PreparedVersion& v,
                     const std::uint8_t* data, std::size_t len,
                     const std::unordered_map<std::string, std::string>& attrs,
                     std::string& err);
  bool local_install_file(const std::string& aios_path, const PreparedVersion& v,
                          const std::string& abs_body_path,
                          const std::unordered_map<std::string, std::string>& attrs,
                          std::string& err);
  bool local_publish(const std::string& aios_path, const std::string& oid, std::uint64_t seq,
                     std::string& err);
  bool local_abort(const std::string& aios_path, const std::string& oid, std::uint64_t seq,
                   std::string& err);

  int replicate_install(const Placement& placement, const PreparedVersion& v,
                        const std::uint8_t* data, std::size_t len,
                        const std::unordered_map<std::string, std::string>& attrs,
                        const std::string& abs_body_path = {});
  int replicate_publish(const Placement& placement, const std::string& oid,
                        std::uint64_t seq);
  void replicate_abort(const Placement& placement, const std::string& oid, std::uint64_t seq);

  // prepare → install quorum → publish (or abort).
  ApiResult commit_prepared(ObjectStore* store, const Placement& placement,
                            PreparedVersion& pv, const std::uint8_t* data, std::size_t len,
                            const std::unordered_map<std::string, std::string>& attrs,
                            const std::string& abs_body_path = {});

  Config cfg_;
  ClusterMap& map_;
  LocalStores& stores_;
  std::string advertise_;
  mutable std::recursive_mutex mu_;
};

}  // namespace aios
