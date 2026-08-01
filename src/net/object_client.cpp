#include "net/object_client.hpp"

#include "config.hpp"
#include "net/server.hpp"
#include "util/auth.hpp"
#include "util/base64.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <boost/asio.hpp>

namespace aios {

ObjectRpcResult object_rpc(const std::string& peer_addr, const std::string& local_node_id,
                           const std::string& local_listen, const std::string& cluster_key,
                           int auth_skew_ms, MsgType req_type, nlohmann::json req_body,
                           std::vector<std::uint8_t> raw) {
  ObjectRpcResult result;
  std::string host, port;
  if (!split_host_port(peer_addr, host, port)) {
    result.error = "bad peer addr: " + peer_addr;
    result.code = "bad_addr";
    return result;
  }

  boost::asio::io_context ioc;
  boost::system::error_code ec;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(host, port, ec);
  if (ec) {
    result.error = "resolve " + peer_addr + ": " + ec.message();
    result.code = "resolve";
    return result;
  }

  tcp::socket sock(ioc);
  boost::asio::connect(sock, endpoints, ec);
  if (ec) {
    result.error = "connect " + peer_addr + ": " + ec.message();
    result.code = "connect";
    return result;
  }

  std::string err;
  Frame hello;
  hello.type = MsgType::Hello;
  hello.body = {{"node_id", local_node_id}, {"listen", local_listen}};
  auth_sign(hello.body, MsgType::Hello, cluster_key);
  if (!write_frame(sock, hello, err, ec)) {
    result.error = "hello write: " + err;
    result.code = "io";
    return result;
  }

  Frame hello_reply;
  if (!read_frame(sock, hello_reply, err, ec) || hello_reply.type != MsgType::Hello) {
    result.error = "hello read: " + err;
    result.code = "io";
    return result;
  }
  if (!auth_verify(hello_reply.body, MsgType::Hello, cluster_key, auth_skew_ms, err)) {
    result.error = "hello auth: " + err;
    result.code = "auth";
    return result;
  }

  Frame req;
  req.type = req_type;
  req.body = std::move(req_body);
  req.raw = std::move(raw);
  if (!req.raw.empty()) req.flags |= kFlagRawBody;
  auth_sign(req.body, req_type, cluster_key);
  if (!write_frame(sock, req, err, ec)) {
    result.error = "request write: " + err;
    result.code = "io";
    return result;
  }

  Frame reply;
  if (!read_frame(sock, reply, err, ec) || reply.type != MsgType::ObjectReply) {
    result.error = "reply read: " + err;
    result.code = "io";
    return result;
  }
  if (!auth_verify(reply.body, MsgType::ObjectReply, cluster_key, auth_skew_ms, err)) {
    result.error = "reply auth: " + err;
    result.code = "auth";
    return result;
  }

  result.body = reply.body;
  result.ok = reply.body.value("ok", false);
  result.error = reply.body.value("error", "");
  result.code = reply.body.value("code", result.ok ? "" : "error");
  result.epoch = reply.body.value("epoch", static_cast<std::uint64_t>(0));
  result.size = reply.body.value("size", static_cast<std::uint64_t>(0));
  result.mtime_ms = reply.body.value("mtime_ms", static_cast<std::int64_t>(0));
  if (reply.body.contains("crc32c") && !reply.body["crc32c"].is_null()) {
    result.crc32c = reply.body.value("crc32c", 0u);
    result.crc32c_known = true;
  }
  if (reply.body.contains("data_b64") && reply.body["data_b64"].is_string()) {
    std::vector<std::uint8_t> data;
    std::string derr;
    if (base64_decode(reply.body["data_b64"].get<std::string>(), data, derr)) {
      result.data = std::move(data);
    } else if (result.ok) {
      result.ok = false;
      result.error = "bad data_b64: " + derr;
      result.code = "decode";
    }
  }
  return result;
}

ObjectRpcResult object_put_remote(const std::string& peer_addr,
                                  const std::string& local_node_id,
                                  const std::string& local_listen,
                                  const std::string& cluster_key, int auth_skew_ms,
                                  std::uint64_t epoch, const std::string& aios_path,
                                  const std::string& oid, const std::uint8_t* data,
                                  std::size_t len,
                                  const std::unordered_map<std::string, std::string>& attrs,
                                  bool as_replica) {
  nlohmann::json attrs_j = nlohmann::json::object();
  for (const auto& [k, v] : attrs) attrs_j[k] = v;
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
      {"data_b64", base64_encode(data, len)},
      {"attrs", attrs_j},
      {"crc32c", crc32c(data, len)},
      {"role", as_replica ? "replica" : "primary"},
  };
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectPut, std::move(body));
}

ObjectRpcResult object_get_remote(const std::string& peer_addr,
                                  const std::string& local_node_id,
                                  const std::string& local_listen,
                                  const std::string& cluster_key, int auth_skew_ms,
                                  std::uint64_t epoch, const std::string& aios_path,
                                  const std::string& oid) {
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
  };
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectGet, std::move(body));
}

ObjectRpcResult object_del_remote(const std::string& peer_addr,
                                  const std::string& local_node_id,
                                  const std::string& local_listen,
                                  const std::string& cluster_key, int auth_skew_ms,
                                  std::uint64_t epoch, const std::string& aios_path,
                                  const std::string& oid, bool as_replica) {
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
      {"role", as_replica ? "replica" : "primary"},
  };
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectDel, std::move(body));
}

ObjectRpcResult object_stat_remote(const std::string& peer_addr,
                                   const std::string& local_node_id,
                                   const std::string& local_listen,
                                   const std::string& cluster_key, int auth_skew_ms,
                                   std::uint64_t epoch, const std::string& aios_path,
                                   const std::string& oid) {
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
  };
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectStat, std::move(body));
}

ObjectRpcResult object_put_range_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    std::uint64_t offset, const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    bool as_replica) {
  nlohmann::json attrs_j = nlohmann::json::object();
  for (const auto& [k, v] : attrs) attrs_j[k] = v;
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
      {"offset", offset},
      {"attrs", attrs_j},
      {"replace_attrs", replace_attrs},
      {"range_crc32c", crc32c(data, len)},
      {"role", as_replica ? "replica" : "primary"},
  };
  std::vector<std::uint8_t> raw(data, data + len);
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectPutRange, std::move(body), std::move(raw));
}

ObjectRpcResult object_install_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const PreparedVersion& v,
    const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs) {
  nlohmann::json attrs_j = nlohmann::json::object();
  for (const auto& [k, vattr] : attrs) attrs_j[k] = vattr;
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", v.oid},
      {"seq", v.seq},
      {"base_seq", v.prev_tip},
      {"size", v.size},
      {"crc32c", v.crc32c},
      {"inline_body", v.inline_body},
      {"fs_path", v.fs_path},
      {"is_delete", v.is_delete},
      {"attrs", attrs_j},
      {"role", "replica"},
  };
  if (!v.redirect_oid.empty()) body["redirect"] = v.redirect_oid;
  if (!v.is_delete && v.redirect_oid.empty()) {
    body["data_b64"] = base64_encode(data, len);
  }
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectPut, std::move(body));
}

ObjectRpcResult object_publish_tip_remote(const std::string& peer_addr,
                                          const std::string& local_node_id,
                                          const std::string& local_listen,
                                          const std::string& cluster_key, int auth_skew_ms,
                                          std::uint64_t epoch, const std::string& aios_path,
                                          const std::string& oid, std::uint64_t seq) {
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
      {"seq", seq},
      {"role", "replica"},
  };
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectPublishTip, std::move(body));
}

ObjectRpcResult object_abort_version_remote(const std::string& peer_addr,
                                            const std::string& local_node_id,
                                            const std::string& local_listen,
                                            const std::string& cluster_key, int auth_skew_ms,
                                            std::uint64_t epoch, const std::string& aios_path,
                                            const std::string& oid, std::uint64_t seq) {
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
      {"seq", seq},
      {"role", "replica"},
  };
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectAbortVersion, std::move(body));
}

ObjectRpcResult object_purge_versions_remote(const std::string& peer_addr,
                                             const std::string& local_node_id,
                                             const std::string& local_listen,
                                             const std::string& cluster_key, int auth_skew_ms,
                                             std::uint64_t epoch, const std::string& aios_path,
                                             const std::string& oid, int keep) {
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
      {"keep", keep},
      {"role", "replica"},
  };
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectPurgeVersions, std::move(body));
}

}  // namespace aios
