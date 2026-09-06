#pragma once

#include "fs/fs_table.hpp"
#include "membership.hpp"
#include "net/framing.hpp"

#include <boost/asio.hpp>

#include <string>

namespace aios {

struct GossipExchangeResult {
  bool ok{false};
  std::string peer_node_id;
  std::string peer_listen;
  std::string error;
};

// Connect → Hello → Gossip → close. Merges response into tables.
// Uses a private io_context so callers must not run this on the daemon accept
// loop thread (it blocks for the round-trip).
// local_only: send this node's member row + local fs entries only (hub reports).
GossipExchangeResult gossip_with_peer(const std::string& peer_addr,
                                      const std::string& local_node_id,
                                      const std::string& local_listen,
                                      const std::string& cluster_key, int auth_skew_ms,
                                      MembershipTable& membership, FsTable& fs_table,
                                      const std::string& local_http_addr = {},
                                      bool local_only = false);

}  // namespace aios
