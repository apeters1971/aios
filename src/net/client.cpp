#include "net/client.hpp"

#include "net/server.hpp"
#include "util/auth.hpp"
#include "util/log.hpp"

#include "config.hpp"

namespace aios {

GossipExchangeResult gossip_with_peer(boost::asio::io_context& ioc,
                                      const std::string& peer_addr,
                                      const std::string& local_node_id,
                                      const std::string& local_listen,
                                      const std::string& cluster_key, int auth_skew_ms,
                                      MembershipTable& membership, FsTable& fs_table,
                                      const std::string& local_http_addr) {
  GossipExchangeResult result;
  std::string host, port;
  if (!split_host_port(peer_addr, host, port)) {
    result.error = "bad peer addr: " + peer_addr;
    return result;
  }

  boost::system::error_code ec;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(host, port, ec);
  if (ec) {
    result.error = "resolve " + peer_addr + ": " + ec.message();
    return result;
  }

  tcp::socket sock(ioc);
  boost::asio::connect(sock, endpoints, ec);
  if (ec) {
    result.error = "connect " + peer_addr + ": " + ec.message();
    return result;
  }

  std::string err;
  Frame hello;
  hello.type = MsgType::Hello;
  hello.body = {{"node_id", local_node_id},
                {"listen", local_listen},
                {"http_addr", local_http_addr}};
  auth_sign(hello.body, MsgType::Hello, cluster_key);
  if (!write_frame(sock, hello, err, ec)) {
    result.error = "hello write: " + err;
    return result;
  }

  Frame hello_reply;
  if (!read_frame(sock, hello_reply, err, ec) || hello_reply.type != MsgType::Hello) {
    result.error = "hello read: " + err;
    return result;
  }
  if (!auth_verify(hello_reply.body, MsgType::Hello, cluster_key, auth_skew_ms, err)) {
    result.error = "hello auth: " + err;
    return result;
  }
  result.peer_node_id = hello_reply.body.value("node_id", "");
  result.peer_listen = hello_reply.body.value("listen", peer_addr);
  if (result.peer_listen.empty()) result.peer_listen = peer_addr;
  const std::string peer_http = hello_reply.body.value("http_addr", "");

  // cluster_map is rebuilt locally from membership + fs_table; gossip carries
  // only those sources. Peers may optionally echo cluster_map in replies.
  Frame gossip;
  gossip.type = MsgType::Gossip;
  gossip.body = {
      {"membership", membership.to_json()},
      {"fs_table", fs_table.to_json()},
  };
  auth_sign(gossip.body, MsgType::Gossip, cluster_key);
  if (!write_frame(sock, gossip, err, ec)) {
    result.error = "gossip write: " + err;
    return result;
  }

  Frame gossip_reply;
  if (!read_frame(sock, gossip_reply, err, ec) ||
      gossip_reply.type != MsgType::Gossip) {
    result.error = "gossip read: " + err;
    return result;
  }
  if (!auth_verify(gossip_reply.body, MsgType::Gossip, cluster_key, auth_skew_ms,
                   err)) {
    result.error = "gossip auth: " + err;
    return result;
  }

  const auto now = now_ms();
  if (!result.peer_node_id.empty()) {
    membership.mark_alive(result.peer_node_id, result.peer_listen, now, peer_http);
  }
  if (gossip_reply.body.contains("membership")) {
    membership.merge(MembershipTable::from_json(gossip_reply.body["membership"]),
                     now);
  }
  if (gossip_reply.body.contains("fs_table")) {
    fs_table.merge(FsTable::from_json(gossip_reply.body["fs_table"]));
  }

  result.ok = true;
  return result;
}

}  // namespace aios
