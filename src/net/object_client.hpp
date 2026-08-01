#pragma once

#include "net/framing.hpp"
#include "store/object_store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

struct ObjectRpcResult {
  bool ok{false};
  std::string error;
  std::string code;  // epoch_mismatch | not_primary | not_found | ...
  std::uint64_t epoch{0};
  std::optional<std::vector<std::uint8_t>> data;
  std::uint64_t size{0};
  std::int64_t mtime_ms{0};
  std::uint32_t crc32c{0};
  bool crc32c_known{false};
  nlohmann::json body = nlohmann::json::object();
};

// Short-lived session on a private io_context: Hello → request → ObjectReply.
ObjectRpcResult object_rpc(const std::string& peer_addr, const std::string& local_node_id,
                           const std::string& local_listen, const std::string& cluster_key,
                           int auth_skew_ms, MsgType req_type, nlohmann::json req_body,
                           std::vector<std::uint8_t> raw = {});

ObjectRpcResult object_put_range_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    std::uint64_t offset, const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    bool as_replica);

ObjectRpcResult object_put_remote(const std::string& peer_addr,
                                  const std::string& local_node_id,
                                  const std::string& local_listen,
                                  const std::string& cluster_key, int auth_skew_ms,
                                  std::uint64_t epoch, const std::string& aios_path,
                                  const std::string& oid, const std::uint8_t* data,
                                  std::size_t len,
                                  const std::unordered_map<std::string, std::string>& attrs,
                                  bool as_replica);

ObjectRpcResult object_install_remote(const std::string& peer_addr,
                                      const std::string& local_node_id,
                                      const std::string& local_listen,
                                      const std::string& cluster_key, int auth_skew_ms,
                                      std::uint64_t epoch, const std::string& aios_path,
                                      const PreparedVersion& v, const std::uint8_t* data,
                                      std::size_t len,
                                      const std::unordered_map<std::string, std::string>& attrs);

ObjectRpcResult object_publish_tip_remote(const std::string& peer_addr,
                                          const std::string& local_node_id,
                                          const std::string& local_listen,
                                          const std::string& cluster_key, int auth_skew_ms,
                                          std::uint64_t epoch, const std::string& aios_path,
                                          const std::string& oid, std::uint64_t seq);

ObjectRpcResult object_abort_version_remote(const std::string& peer_addr,
                                            const std::string& local_node_id,
                                            const std::string& local_listen,
                                            const std::string& cluster_key, int auth_skew_ms,
                                            std::uint64_t epoch, const std::string& aios_path,
                                            const std::string& oid, std::uint64_t seq);

ObjectRpcResult object_purge_versions_remote(const std::string& peer_addr,
                                             const std::string& local_node_id,
                                             const std::string& local_listen,
                                             const std::string& cluster_key, int auth_skew_ms,
                                             std::uint64_t epoch, const std::string& aios_path,
                                             const std::string& oid, int keep);

ObjectRpcResult object_get_remote(const std::string& peer_addr,
                                  const std::string& local_node_id,
                                  const std::string& local_listen,
                                  const std::string& cluster_key, int auth_skew_ms,
                                  std::uint64_t epoch, const std::string& aios_path,
                                  const std::string& oid);

ObjectRpcResult object_del_remote(const std::string& peer_addr,
                                  const std::string& local_node_id,
                                  const std::string& local_listen,
                                  const std::string& cluster_key, int auth_skew_ms,
                                  std::uint64_t epoch, const std::string& aios_path,
                                  const std::string& oid, bool as_replica);

ObjectRpcResult object_stat_remote(const std::string& peer_addr,
                                   const std::string& local_node_id,
                                   const std::string& local_listen,
                                   const std::string& cluster_key, int auth_skew_ms,
                                   std::uint64_t epoch, const std::string& aios_path,
                                   const std::string& oid);

}  // namespace aios
