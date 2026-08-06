#pragma once

#include "net/framing.hpp"

#include <boost/asio.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace aios {

using tcp = boost::asio::ip::tcp;

bool read_frame(tcp::socket& sock, Frame& out, std::string& err,
                boost::system::error_code& ec);

bool write_frame(tcp::socket& sock, const Frame& frame, std::string& err,
                 boost::system::error_code& ec);

struct RpcHandlers {
  std::string local_node_id;
  std::string local_listen;  // advertise address host:port
  std::string local_http_addr;  // host:port HTTP (optional)
  std::string cluster_key;
  int auth_skew_ms{60000};

  // Merge inbound gossip; return outbound Gossip frame.
  std::function<std::optional<Frame>(const std::string& peer_node_id,
                                     const std::string& peer_listen,
                                     const Frame& gossip_req)>
      on_gossip;

  // Handle ObjectPut/Get/Del/Stat → ObjectReply (caller signs reply).
  std::function<Frame(const Frame& req)> on_object;
};

// Backward-compatible alias.
using GossipHandlers = RpcHandlers;

class TcpServer {
 public:
  TcpServer(boost::asio::io_context& ioc, const std::string& listen_host,
            const std::string& listen_port, RpcHandlers handlers);

  void start();
  void close();

 private:
  void do_accept();
  void handle_session(std::shared_ptr<tcp::socket> sock);

  boost::asio::io_context& ioc_;
  tcp::acceptor acceptor_;
  RpcHandlers handlers_;
};

}  // namespace aios
