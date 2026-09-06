#pragma once

#include "net/framing.hpp"
#include "object/object_layout.hpp"
#include "store/object_store.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// AttrPrecondition via object_store.hpp

// ObjectListResult / PreparedVersion via object_store.hpp

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
  ObjectListResult list;
  std::vector<std::uint8_t> raw;  // ObjectReply kFlagRawBody trailer
  nlohmann::json body = nlohmann::json::object();
};

// Object RPC over a pooled keep-alive TCP session (Hello once, then request/reply).
// Connections are reused across calls to the same peer; I/O errors discard the socket.
ObjectRpcResult object_rpc(const std::string& peer_addr, const std::string& local_node_id,
                           const std::string& local_listen, const std::string& cluster_key,
                           int auth_skew_ms, MsgType req_type, nlohmann::json req_body,
                           std::vector<std::uint8_t> raw = {});

// Cluster-map consensus RPC (MsgType::MapRpc). Returns the monitor's reply body,
// nullopt on transport / auth failure or when the peer runs no monitor.
std::optional<nlohmann::json> map_rpc_remote(const std::string& peer_addr,
                                             const std::string& local_node_id,
                                             const std::string& local_listen,
                                             const std::string& cluster_key, int auth_skew_ms,
                                             nlohmann::json req);

// Drop idle pooled sockets (call before tearing down peer TcpServers / in tests).
void object_rpc_pool_clear();
// Per-RPC progress deadline for every object RPC (default 30 s; <=0 restores it).
void object_rpc_set_timeout_ms(int ms);

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
                                  bool as_replica, const LayoutRequest& layout = {});

ObjectRpcResult object_install_remote(const std::string& peer_addr,
                                      const std::string& local_node_id,
                                      const std::string& local_listen,
                                      const std::string& cluster_key, int auth_skew_ms,
                                      std::uint64_t epoch, const std::string& aios_path,
                                      const PreparedVersion& v, const std::uint8_t* data,
                                      std::size_t len,
                                      const std::unordered_map<std::string, std::string>& attrs);

// Stream FS body to peer via ObjectStageBegin/Data/Commit (no full-object RAM copy).
ObjectRpcResult object_install_file_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const PreparedVersion& v,
    const std::unordered_map<std::string, std::string>& attrs,
    const std::string& abs_body_path);

// Stage from an already-buffered body (shared fan-out after one primary read).
ObjectRpcResult object_install_bytes_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const PreparedVersion& v,
    const std::unordered_map<std::string, std::string>& attrs, const std::uint8_t* data,
    std::size_t len);

// Sticky ObjectStageBegin/Data/Commit session on one pooled connection (pipelined PUT).
class RemoteStageSession {
 public:
  RemoteStageSession();
  ~RemoteStageSession();
  RemoteStageSession(RemoteStageSession&&) noexcept;
  RemoteStageSession& operator=(RemoteStageSession&&) noexcept;
  RemoteStageSession(const RemoteStageSession&) = delete;
  RemoteStageSession& operator=(const RemoteStageSession&) = delete;

  bool begin(const std::string& peer_addr, const std::string& local_node_id,
             const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
             std::uint64_t epoch, const std::string& aios_path, const PreparedVersion& v);
  bool data(std::uint64_t offset, const std::uint8_t* p, std::size_t n);
  bool commit(const PreparedVersion& v,
              const std::unordered_map<std::string, std::string>& attrs);
  // Best-effort abort + connection drop.
  void abort();

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }
  const std::string& peer_addr() const { return peer_addr_; }
  const std::string& aios_path() const { return aios_path_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string peer_addr_;
  std::string aios_path_;
  std::string cluster_key_;
  int auth_skew_ms_{0};
  std::uint64_t epoch_{0};
  PreparedVersion meta_;
  bool ok_{false};
  std::string error_;
};

ObjectRpcResult object_list_remote(const std::string& peer_addr,
                                   const std::string& local_node_id,
                                   const std::string& local_listen,
                                   const std::string& cluster_key, int auth_skew_ms,
                                   std::uint64_t epoch, const std::string& prefix,
                                   const std::string& attr_eq_key,
                                   const std::string& attr_eq_value, std::size_t limit,
                                   const std::string& cursor, bool include_attrs);

ObjectRpcResult object_publish_tip_remote(const std::string& peer_addr,
                                          const std::string& local_node_id,
                                          const std::string& local_listen,
                                          const std::string& cluster_key, int auth_skew_ms,
                                          std::uint64_t epoch, const std::string& aios_path,
                                          const std::string& oid, std::uint64_t seq);

// Ask a remote primary to prepare+install without publishing (publish=false).
ObjectRpcResult object_prepare_put_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs,
    const std::vector<AttrPrecondition>& preds = {},
    const std::optional<std::string>& lock_token = std::nullopt);

ObjectRpcResult object_prepare_delete_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    const std::vector<AttrPrecondition>& preds = {},
    const std::optional<std::string>& lock_token = std::nullopt);

// Ask a remote primary to publish a prepared seq (fans out to its replicas).
ObjectRpcResult object_publish_prepared_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    std::uint64_t seq);

ObjectRpcResult object_abort_prepared_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    std::uint64_t seq);

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

// Range get with raw trailer (no base64). Prefer for large bodies.
ObjectRpcResult object_get_range_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    std::uint64_t offset, std::size_t len, std::optional<std::uint64_t> seq = std::nullopt);

// Stream full object to a local file via ranged gets (works for large remote bodies).
ObjectRpcResult object_get_file_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    const std::string& abs_out_path);

ObjectRpcResult object_del_remote(const std::string& peer_addr,
                                  const std::string& local_node_id,
                                  const std::string& local_listen,
                                  const std::string& cluster_key, int auth_skew_ms,
                                  std::uint64_t epoch, const std::string& aios_path,
                                  const std::string& oid, bool as_replica);

// include_deleted=true reports a delete-marker tip as ok with {"deleted": true,
// "seq": <marker seq>} instead of not_found (repair needs the raw tip).
ObjectRpcResult object_stat_remote(const std::string& peer_addr,
                                   const std::string& local_node_id,
                                   const std::string& local_listen,
                                   const std::string& cluster_key, int auth_skew_ms,
                                   std::uint64_t epoch, const std::string& aios_path,
                                   const std::string& oid, bool include_deleted = false,
                                   std::optional<std::uint64_t> seq = std::nullopt);

}  // namespace aios
