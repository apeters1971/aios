#include "net/object_client.hpp"

#include "config.hpp"
#include "net/server.hpp"
#include "util/auth.hpp"
#include "util/base64.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <boost/asio.hpp>

#include <algorithm>
#include <fstream>

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
  result.raw = std::move(reply.raw);
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
  if (!result.raw.empty() && !result.data) {
    result.data = result.raw;
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
  if (reply.body.contains("objects") && reply.body["objects"].is_array()) {
    for (const auto& o : reply.body["objects"]) {
      ObjectListEntry e;
      e.oid = o.value("oid", "");
      e.seq = o.value("seq", static_cast<std::uint64_t>(0));
      e.size = o.value("size", static_cast<std::uint64_t>(0));
      e.mtime_ms = o.value("mtime_ms", static_cast<std::int64_t>(0));
      e.crc32c = o.value("crc32c", 0u);
      e.crc32c_known = o.contains("crc32c");
      e.is_delete = o.value("is_delete", false);
      e.redirect_oid = o.value("redirect_oid", "");
      if (o.contains("attrs") && o["attrs"].is_object()) {
        for (auto it = o["attrs"].begin(); it != o["attrs"].end(); ++it) {
          e.attrs[it.key()] = it.value().get<std::string>();
        }
      }
      if (!e.oid.empty()) result.list.objects.push_back(std::move(e));
    }
    result.list.next_cursor = reply.body.value("next_cursor", "");
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

namespace {

ObjectRpcResult rpc_one(tcp::socket& sock, MsgType type, nlohmann::json body,
                        std::vector<std::uint8_t> raw, const std::string& cluster_key,
                        int auth_skew_ms) {
  ObjectRpcResult result;
  std::string err;
  boost::system::error_code ec;
  Frame req;
  req.type = type;
  req.body = std::move(body);
  req.raw = std::move(raw);
  if (!req.raw.empty()) req.flags |= kFlagRawBody;
  auth_sign(req.body, type, cluster_key);
  if (!write_frame(sock, req, err, ec)) {
    result.error = "write: " + err;
    result.code = "io";
    return result;
  }
  Frame reply;
  if (!read_frame(sock, reply, err, ec) || reply.type != MsgType::ObjectReply) {
    result.error = "reply: " + err;
    result.code = "io";
    return result;
  }
  if (!auth_verify(reply.body, MsgType::ObjectReply, cluster_key, auth_skew_ms, err)) {
    result.error = "auth: " + err;
    result.code = "auth";
    return result;
  }
  result.body = reply.body;
  result.ok = reply.body.value("ok", false);
  result.error = reply.body.value("error", "");
  result.code = reply.body.value("code", result.ok ? "" : "error");
  result.epoch = reply.body.value("epoch", static_cast<std::uint64_t>(0));
  return result;
}

}  // namespace

ObjectRpcResult object_install_file_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const PreparedVersion& v,
    const std::unordered_map<std::string, std::string>& attrs,
    const std::string& abs_body_path) {
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
    result.error = "resolve: " + ec.message();
    result.code = "resolve";
    return result;
  }
  tcp::socket sock(ioc);
  boost::asio::connect(sock, endpoints, ec);
  if (ec) {
    result.error = "connect: " + ec.message();
    result.code = "connect";
    return result;
  }

  std::string err;
  Frame hello;
  hello.type = MsgType::Hello;
  hello.body = {{"node_id", local_node_id}, {"listen", local_listen}, {"http_addr", ""}};
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

  nlohmann::json attrs_j = nlohmann::json::object();
  for (const auto& [k, vattr] : attrs) attrs_j[k] = vattr;
  nlohmann::json begin = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", v.oid},
      {"seq", v.seq},
      {"base_seq", v.prev_tip},
      {"size", v.size},
      {"crc32c", v.crc32c},
      {"inline_body", false},
      {"fs_path", v.fs_path},
      {"is_delete", v.is_delete},
      {"attrs", attrs_j},
      {"role", "replica"},
  };
  if (!v.redirect_oid.empty()) begin["redirect"] = v.redirect_oid;
  result = rpc_one(sock, MsgType::ObjectStageBegin, std::move(begin), {}, cluster_key,
                   auth_skew_ms);
  if (!result.ok) return result;

  std::ifstream in(abs_body_path, std::ios::binary);
  if (!in) {
    result.ok = false;
    result.error = "cannot open " + abs_body_path;
    result.code = "io";
    return result;
  }
  std::vector<std::uint8_t> buf(kStageChunkSize);
  std::uint64_t offset = 0;
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const auto n = static_cast<std::size_t>(in.gcount());
    if (n == 0) break;
    nlohmann::json chunk = {
        {"epoch", epoch},
        {"aios_path", aios_path},
        {"oid", v.oid},
        {"seq", v.seq},
        {"offset", offset},
        {"role", "replica"},
    };
    std::vector<std::uint8_t> raw(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
    result = rpc_one(sock, MsgType::ObjectStageData, std::move(chunk), std::move(raw),
                     cluster_key, auth_skew_ms);
    if (!result.ok) return result;
    offset += n;
  }

  nlohmann::json commit = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", v.oid},
      {"seq", v.seq},
      {"base_seq", v.prev_tip},
      {"size", v.size},
      {"crc32c", v.crc32c},
      {"inline_body", false},
      {"fs_path", v.fs_path},
      {"is_delete", v.is_delete},
      {"attrs", attrs_j},
      {"role", "replica"},
  };
  if (!v.redirect_oid.empty()) commit["redirect"] = v.redirect_oid;
  return rpc_one(sock, MsgType::ObjectStageCommit, std::move(commit), {}, cluster_key,
                 auth_skew_ms);
}

ObjectRpcResult object_prepare_put_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs) {
  nlohmann::json attrs_j = nlohmann::json::object();
  for (const auto& [k, v] : attrs) attrs_j[k] = v;
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
      {"data_b64", base64_encode(data, len)},
      {"attrs", attrs_j},
      {"crc32c", crc32c(data, len)},
      {"role", "primary"},
      {"publish", false},
  };
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectPut, std::move(body));
}

ObjectRpcResult object_prepare_delete_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid) {
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
      {"role", "primary"},
      {"publish", false},
  };
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectDel, std::move(body));
}

ObjectRpcResult object_publish_prepared_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    std::uint64_t seq) {
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
      {"seq", seq},
      {"role", "primary"},
  };
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectPublishTip, std::move(body));
}

ObjectRpcResult object_abort_prepared_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    std::uint64_t seq) {
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
      {"seq", seq},
      {"role", "primary"},
  };
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectAbortVersion, std::move(body));
}

ObjectRpcResult object_list_remote(const std::string& peer_addr,
                                   const std::string& local_node_id,
                                   const std::string& local_listen,
                                   const std::string& cluster_key, int auth_skew_ms,
                                   std::uint64_t epoch, const std::string& prefix,
                                   const std::string& attr_eq_key,
                                   const std::string& attr_eq_value, std::size_t limit,
                                   const std::string& cursor, bool include_attrs) {
  nlohmann::json body = {
      {"epoch", epoch},
      {"prefix", prefix},
      {"attr_eq_key", attr_eq_key},
      {"attr_eq_value", attr_eq_value},
      {"limit", limit},
      {"cursor", cursor},
      {"attrs", include_attrs},
  };
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectList, std::move(body));
}

ObjectRpcResult object_get_range_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    std::uint64_t offset, std::size_t len, std::optional<std::uint64_t> seq) {
  nlohmann::json body = {
      {"epoch", epoch},
      {"aios_path", aios_path},
      {"oid", oid},
      {"offset", offset},
      {"length", len},
  };
  if (seq.has_value()) body["seq"] = *seq;
  return object_rpc(peer_addr, local_node_id, local_listen, cluster_key, auth_skew_ms,
                    MsgType::ObjectGet, std::move(body));
}

ObjectRpcResult object_get_file_remote(
    const std::string& peer_addr, const std::string& local_node_id,
    const std::string& local_listen, const std::string& cluster_key, int auth_skew_ms,
    std::uint64_t epoch, const std::string& aios_path, const std::string& oid,
    const std::string& abs_out_path) {
  auto st = object_stat_remote(peer_addr, local_node_id, local_listen, cluster_key,
                               auth_skew_ms, epoch, aios_path, oid);
  if (!st.ok) return st;
  const auto total = st.size;
  const auto seq = st.body.value("seq", static_cast<std::uint64_t>(0));

  std::ofstream out(abs_out_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    ObjectRpcResult r;
    r.error = "cannot create " + abs_out_path;
    r.code = "io";
    return r;
  }

  std::uint64_t offset = 0;
  while (offset < total) {
    const auto n = static_cast<std::size_t>(
        std::min<std::uint64_t>(kStageChunkSize, total - offset));
    auto chunk =
        object_get_range_remote(peer_addr, local_node_id, local_listen, cluster_key,
                                auth_skew_ms, epoch, aios_path, oid, offset, n, seq);
    if (!chunk.ok) return chunk;
    const auto& bytes = !chunk.raw.empty() ? chunk.raw : (chunk.data ? *chunk.data : chunk.raw);
    if (bytes.size() != n && offset + bytes.size() != total && bytes.empty()) {
      ObjectRpcResult r;
      r.ok = false;
      r.error = "empty get range";
      r.code = "io";
      return r;
    }
    if (!bytes.empty()) {
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
      offset += bytes.size();
    } else {
      break;
    }
  }
  out.close();
  ObjectRpcResult r;
  r.ok = (offset == total);
  r.epoch = st.epoch;
  r.size = total;
  r.crc32c = st.crc32c;
  r.crc32c_known = st.crc32c_known;
  r.body = st.body;
  if (!r.ok) {
    r.error = "short read";
    r.code = "io";
  }
  return r;
}

}  // namespace aios
