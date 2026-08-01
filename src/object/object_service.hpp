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
// recursive_mutex used to allow api_head → api_get

namespace aios {

struct ApiResult {
  bool ok{false};
  std::string code;
  std::string error;
  std::uint64_t epoch{0};
  int replicas{0};
  std::optional<std::vector<std::uint8_t>> data;
  std::optional<ObjectInfo> info;
  std::unordered_map<std::string, std::string> attrs;
  ObjectListResult list;
  Placement placement;
};

// Handles ObjectPut/Get/Del/Stat/PutRange. Primary path fans out synchronous replicas.
class ObjectService {
 public:
  ObjectService(Config cfg, ClusterMap& map, LocalStores& stores);

  void set_advertise(std::string advertise);

  // Dispatch authenticated object request → ObjectReply frame (unsigned; caller signs).
  Frame handle(const Frame& req);

  // High-level API used by HTTP front-end (placement + quorum replication).
  ApiResult api_put(const std::string& oid, const std::uint8_t* data, std::size_t len,
                    const std::unordered_map<std::string, std::string>& attrs,
                    bool replace_attrs, const std::vector<AttrPrecondition>& preds);
  ApiResult api_put_range(const std::string& oid, std::uint64_t offset,
                          const std::uint8_t* data, std::size_t len,
                          const std::unordered_map<std::string, std::string>& attrs,
                          bool replace_attrs, const std::vector<AttrPrecondition>& preds);
  ApiResult api_get(const std::string& oid, std::optional<std::uint64_t> offset,
                    std::optional<std::uint64_t> end_inclusive,
                    const std::vector<AttrPrecondition>& preds);
  ApiResult api_head(const std::string& oid, const std::vector<AttrPrecondition>& preds);
  ApiResult api_del(const std::string& oid, const std::vector<AttrPrecondition>& preds);
  ApiResult api_list(const std::string& prefix, const std::string& attr_eq_key,
                     const std::string& attr_eq_value, std::size_t limit,
                     const std::string& cursor, bool include_attrs);

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

  bool epoch_ok(std::uint64_t req_epoch, Frame& err_out) const;
  bool local_put(const std::string& aios_path, const std::string& oid,
                 const std::uint8_t* data, std::size_t len,
                 const std::unordered_map<std::string, std::string>& attrs,
                 std::string& err);
  bool local_put_range(const std::string& aios_path, const std::string& oid,
                       std::uint64_t offset, const std::uint8_t* data, std::size_t len,
                       const std::unordered_map<std::string, std::string>& attrs,
                       bool replace_attrs, std::string& err);
  bool local_del(const std::string& aios_path, const std::string& oid, std::string& err);

  int replicate_put(const Placement& placement, const std::string& oid,
                    const std::uint8_t* data, std::size_t len,
                    const std::unordered_map<std::string, std::string>& attrs);
  int replicate_put_range(const Placement& placement, const std::string& oid,
                          std::uint64_t offset, const std::uint8_t* data, std::size_t len,
                          const std::unordered_map<std::string, std::string>& attrs,
                          bool replace_attrs);
  int replicate_del(const Placement& placement, const std::string& oid);

  int quorum_need(const Placement& placement) const;
  ApiResult fail(const std::string& code, const std::string& error) const;
  ObjectStore* primary_store(const Placement& p, std::string& err);
  PrecondResult check_preds_on(ObjectStore* store, const std::string& oid,
                               const std::vector<AttrPrecondition>& preds, std::string& err);

  Config cfg_;
  ClusterMap& map_;
  LocalStores& stores_;
  std::string advertise_;
  mutable std::recursive_mutex mu_;
};

}  // namespace aios
