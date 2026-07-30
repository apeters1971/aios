#pragma once

#include "config.hpp"
#include "fs/fs_table.hpp"
#include "membership.hpp"
#include "net/server.hpp"

#include <boost/asio.hpp>

#include <memory>

namespace aios {

class GossipEngine {
 public:
  GossipEngine(boost::asio::io_context& ioc, Config cfg, MembershipTable& membership,
               FsTable& fs_table);

  void start();

 private:
  void on_gossip_timer(const boost::system::error_code& ec);
  void on_scan_timer(const boost::system::error_code& ec);
  void on_status_timer(const boost::system::error_code& ec);
  void run_scan();
  void write_status();
  Frame handle_inbound_gossip(const std::string& peer_node_id,
                              const std::string& peer_listen, const Frame& req);

  std::string advertise_addr() const;

  boost::asio::io_context& ioc_;
  Config cfg_;
  MembershipTable& membership_;
  FsTable& fs_table_;
  std::unique_ptr<TcpServer> server_;
  boost::asio::steady_timer gossip_timer_;
  boost::asio::steady_timer scan_timer_;
  boost::asio::steady_timer status_timer_;
};

}  // namespace aios
