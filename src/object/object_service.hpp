#pragma once

#include "cluster/cluster_map.hpp"
#include "cluster/place.hpp"
#include "config.hpp"
#include "metrics/ops_counters.hpp"
#include "net/framing.hpp"
#include "object/locks_watches.hpp"
#include "object/object_layout.hpp"
#include "object/pubsub.hpp"
#include "store/local_stores.hpp"
#include "store/object_store.hpp"

#include <nlohmann/json.hpp>

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
  // Lock / watch payloads (JSON in `data` when set).
  std::optional<nlohmann::json> json_body;
  std::optional<WatchEvent> watch_event;
  std::vector<WatchEvent> watch_events;
  std::vector<PubMessage> pub_messages;
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
                    std::optional<std::uint32_t> expected_crc32c = std::nullopt,
                    const LayoutRequest& layout = {},
                    const std::optional<std::string>& lock_token = std::nullopt);
  // Staging file is moved into the store (FS-backed). Prefer for large bodies.
  ApiResult api_put_file(const std::string& oid, const std::string& staging_abs_path,
                         std::uint64_t size, std::uint32_t crc32c_val,
                         const std::unordered_map<std::string, std::string>& attrs,
                         bool replace_attrs, const std::vector<AttrPrecondition>& preds,
                         std::optional<std::uint32_t> expected_crc32c = std::nullopt,
                         const LayoutRequest& layout = {},
                         const std::optional<std::string>& lock_token = std::nullopt);
  ApiResult api_put_redirect(const std::string& oid, const std::string& target_oid,
                             const std::unordered_map<std::string, std::string>& attrs,
                             bool replace_attrs, const std::vector<AttrPrecondition>& preds,
                             const std::optional<std::string>& lock_token = std::nullopt);
  ApiResult api_put_range(const std::string& oid, std::uint64_t offset,
                          const std::uint8_t* data, std::size_t len,
                          const std::unordered_map<std::string, std::string>& attrs,
                          bool replace_attrs, const std::vector<AttrPrecondition>& preds,
                          const LayoutRequest& layout = {},
                          const std::optional<std::string>& lock_token = std::nullopt);
  // Atomic byte-append at tip.size (primary-serialized). Returns json_body
  // {offset,size,seq,epoch}. Rejected for EC / redirect tips.
  ApiResult api_append(const std::string& oid, const std::uint8_t* data, std::size_t len,
                       const std::unordered_map<std::string, std::string>& attrs,
                       bool replace_attrs, const std::vector<AttrPrecondition>& preds,
                       const LayoutRequest& layout = {},
                       const std::optional<std::string>& lock_token = std::nullopt);
  ApiResult api_get(const std::string& oid, std::optional<std::uint64_t> offset,
                    std::optional<std::uint64_t> end_inclusive,
                    const std::vector<AttrPrecondition>& preds,
                    std::optional<std::uint64_t> seq = std::nullopt);
  ApiResult api_head(const std::string& oid, const std::vector<AttrPrecondition>& preds,
                     std::optional<std::uint64_t> seq = std::nullopt);
  ApiResult api_del(const std::string& oid, const std::vector<AttrPrecondition>& preds,
                    const std::optional<std::string>& lock_token = std::nullopt);

  // Enforced leases (primary-local, TTL + token).
  ApiResult api_lock_acquire(const std::string& oid, int ttl_ms = LockTable::kDefaultTtlMs);
  ApiResult api_lock_renew(const std::string& oid, const std::string& token,
                           int ttl_ms = LockTable::kDefaultTtlMs);
  ApiResult api_lock_release(const std::string& oid, const std::string& token);
  ApiResult api_lock_stat(const std::string& oid);

  // Long-poll watches (blocking; call from a worker thread, not the io_context).
  // after_seq: return immediately if tip.seq > after_seq; else wait for next change.
  ApiResult api_watch_oid(const std::string& oid, std::uint64_t after_seq, int timeout_ms);
  // Prefix watch: events for oids this node commits as primary under prefix.
  ApiResult api_watch_prefix(const std::string& prefix, int timeout_ms);

  // Topic pub/sub (coordinator = primary for pubsub/{topic}). Subscribe blocks.
  ApiResult api_pubsub_create(const std::string& topic, DeliveryMode mode,
                              std::size_t capacity = TopicHub::kDefaultCapacity);
  ApiResult api_pubsub_stat(const std::string& topic);
  ApiResult api_pubsub_publish(const std::string& topic, const std::uint8_t* data,
                               std::size_t len, const std::string& content_type,
                               std::optional<DeliveryMode> mode = std::nullopt,
                               std::size_t capacity = TopicHub::kDefaultCapacity);
  // after_id_set=false → wait for next message after current tip.
  ApiResult api_pubsub_subscribe(const std::string& topic, std::uint64_t after_id,
                                 bool after_id_set, int timeout_ms);

  // cluster=true scatter-gathers ObjectList across nodes; false = local stores only.
  ApiResult api_list(const std::string& prefix, const std::string& attr_eq_key,
                     const std::string& attr_eq_value, std::size_t limit,
                     const std::string& cursor, bool include_attrs, bool cluster = true);
  ApiResult api_list_versions(const std::string& oid);
  ApiResult api_purge_version(const std::string& oid, std::uint64_t seq, bool allow_tip);
  ApiResult api_trim_versions(const std::string& oid, int keep);

  // Prepare + quorum install without publishing tip (for cross-object txns).
  ApiResult api_prepare_put(const std::string& oid, const std::uint8_t* data, std::size_t len,
                            const std::unordered_map<std::string, std::string>& attrs,
                            bool replace_attrs, const std::vector<AttrPrecondition>& preds,
                            std::optional<std::uint32_t> expected_crc32c = std::nullopt,
                            const std::optional<std::string>& lock_token = std::nullopt);
  ApiResult api_prepare_put_file(const std::string& oid, const std::string& staging_abs_path,
                                 std::uint64_t size, std::uint32_t crc32c_val,
                                 const std::unordered_map<std::string, std::string>& attrs,
                                 bool replace_attrs, const std::vector<AttrPrecondition>& preds,
                                 std::optional<std::uint32_t> expected_crc32c = std::nullopt,
                                 const std::optional<std::string>& lock_token = std::nullopt);
  ApiResult api_prepare_delete(const std::string& oid,
                               const std::vector<AttrPrecondition>& preds,
                               const std::optional<std::string>& lock_token = std::nullopt);
  ApiResult api_publish_version(const std::string& oid, std::uint64_t seq);
  ApiResult api_abort_prepared(const std::string& oid, std::uint64_t seq);

  // Cross-object transactions (coordinator = primary for txn/<id>).
  ApiResult api_txn_begin();
  ApiResult api_txn_get(const std::string& txn_id);
  ApiResult api_txn_prepare_put(const std::string& txn_id, const std::string& oid,
                                const std::uint8_t* data, std::size_t len,
                                const std::unordered_map<std::string, std::string>& attrs,
                                const std::vector<AttrPrecondition>& preds,
                                std::optional<std::uint32_t> expected_crc32c = std::nullopt,
                                const std::optional<std::string>& lock_token = std::nullopt);
  ApiResult api_txn_prepare_put_file(const std::string& txn_id, const std::string& oid,
                                     const std::string& staging_abs_path, std::uint64_t size,
                                     std::uint32_t crc32c_val,
                                     const std::unordered_map<std::string, std::string>& attrs,
                                     const std::vector<AttrPrecondition>& preds,
                                     std::optional<std::uint32_t> expected_crc32c = std::nullopt,
                                     const std::optional<std::string>& lock_token = std::nullopt);
  ApiResult api_txn_prepare_delete(const std::string& txn_id, const std::string& oid,
                                   const std::vector<AttrPrecondition>& preds,
                                   const std::optional<std::string>& lock_token = std::nullopt);
  ApiResult api_txn_commit(const std::string& txn_id);
  ApiResult api_txn_abort(const std::string& txn_id);

  // Publish a rebuilt cluster map. Takes the same lock that serializes request
  // handling, so worker threads never observe a half-written map.
  void update_cluster_map(ClusterMap m);

  ClusterMap& map() { return map_; }
  const ClusterMap& map() const { return map_; }
  const Config& config() const { return cfg_; }
  OpsRegistry& ops() { return ops_; }
  const OpsRegistry& ops() const { return ops_; }
  LocalStores& stores() { return stores_; }
  const Config& cfg() const { return cfg_; }
  const std::string& advertise() const { return advertise_; }

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
  // prepare → install quorum only (tip unchanged).
  ApiResult install_prepared(ObjectStore* store, const Placement& placement,
                             PreparedVersion& pv, const std::uint8_t* data, std::size_t len,
                             const std::unordered_map<std::string, std::string>& attrs,
                             const std::string& abs_body_path = {});

  // Erasure-coded put: stripe → per-target shard install → publish tips.
  ApiResult commit_ec_put(ObjectStore* store, const Placement& placement, const std::string& oid,
                          const std::uint8_t* data, std::size_t len,
                          const std::unordered_map<std::string, std::string>& attrs,
                          bool replace_attrs, std::optional<std::uint32_t> expected_crc32c,
                          const ObjectLayout& layout);
  // Gather shards from acting set and decode (any node with cluster access).
  ApiResult reconstruct_ec_object(const Placement& placement, const std::string& oid,
                                  std::optional<std::uint64_t> seq,
                                  const std::unordered_map<std::string, std::string>& tip_attrs);

  ApiResult require_txn_primary(const std::string& txn_id, nlohmann::json& state_out);
  ApiResult save_txn_state(const std::string& txn_id, const nlohmann::json& state);
  ApiResult load_txn_state(const std::string& txn_id, nlohmann::json& state_out);

  ApiResult require_primary(const std::string& oid, Placement& placement_out,
                            const std::string& storage_class = {});
  ApiResult enforce_lock(const std::string& oid, const std::optional<std::string>& token);
  void signal_watch(const std::string& oid, std::uint64_t seq, const std::string& op);

  ApiResult load_pubsub_meta(const std::string& topic, DeliveryMode& mode_out,
                             std::uint64_t& next_id_out);
  ApiResult save_pubsub_meta(const std::string& topic, DeliveryMode mode,
                             std::uint64_t next_id);
  ApiResult ensure_pubsub_topic(const std::string& topic, std::optional<DeliveryMode> mode,
                                std::size_t capacity, DeliveryMode& mode_out);

  Config cfg_;
  ClusterMap& map_;
  LocalStores& stores_;
  std::string advertise_;
  mutable std::recursive_mutex mu_;
  mutable OpsRegistry ops_;
  LockTable locks_;
  WatchHub watches_;
  TopicHub pubsub_;
};

}  // namespace aios
