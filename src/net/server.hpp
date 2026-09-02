#pragma once

#include "net/framing.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

namespace aios {

using tcp = boost::asio::ip::tcp;

// timeout_ms < 0: block indefinitely (legacy behaviour). Otherwise the call fails
// with ec == boost::asio::error::timed_out when the peer makes no progress for
// timeout_ms (a per-poll progress deadline, not a total transfer deadline).
bool read_frame(tcp::socket& sock, Frame& out, std::string& err,
                boost::system::error_code& ec, int timeout_ms = -1);

bool write_frame(tcp::socket& sock, const Frame& frame, std::string& err,
                 boost::system::error_code& ec, int timeout_ms = -1);

struct RpcHandlers {
  std::string local_node_id;
  std::string local_listen;  // advertise address host:port
  std::string local_http_addr;  // host:port HTTP (optional)
  std::string cluster_key;
  int auth_skew_ms{60000};
  // A connection that has not completed Hello within this window is dropped.
  int pre_hello_timeout_ms{5000};
  // Keep-alive sessions idle longer than this are closed so they stop pinning a
  // session worker. Clients recycle pooled sockets well below this (see
  // object_client.cpp kPoolIdleTtlMs).
  int idle_timeout_ms{60000};

  // Merge inbound gossip; return outbound Gossip frame.
  // peer_http_addr is the Hello advertisement (may be empty).
  std::function<std::optional<Frame>(const std::string& peer_node_id,
                                     const std::string& peer_listen,
                                     const std::string& peer_http_addr,
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
  ~TcpServer();

  void start();
  // Closes the acceptor and every live session so blocking session reads return,
  // then drains the session pool. No handler runs once this returns, so it must be
  // called before anything RpcHandlers refers to is destroyed.
  void close();

 private:
  void do_accept();
  void handle_session(std::shared_ptr<tcp::socket> sock);
  void run_session(tcp::socket& sock);
  void kick_sessions();

  boost::asio::io_context& ioc_;
  tcp::acceptor acceptor_;
  RpcHandlers handlers_;
  // Sessions read and write synchronously and stay open for keep-alive, so they
  // must not run on ioc_: one idle peer would stall accepts, gossip and every timer.
  // Sized for concurrent replica install/publish under multi-threaded clients.
  boost::asio::thread_pool workers_{32};
  std::atomic<bool> workers_drained_{false};
  std::atomic<bool> closing_{false};
  std::mutex sessions_mu_;
  std::unordered_set<std::shared_ptr<tcp::socket>> sessions_;
};

}  // namespace aios
