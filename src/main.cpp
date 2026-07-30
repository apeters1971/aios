#include "config.hpp"
#include "fs/fs_table.hpp"
#include "gossip.hpp"
#include "membership.hpp"
#include "util/log.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <csignal>
#include <iostream>

namespace {
std::atomic<bool> g_stop{false};
boost::asio::io_context* g_ioc = nullptr;

void on_signal(int) {
  g_stop.store(true);
  if (g_ioc) g_ioc->stop();
}
}  // namespace

int main(int argc, char** argv) {
  using namespace aios;

  Config cfg;
  std::string err;
  bool help = false;
  if (!parse_cli(argc, argv, cfg, err, help)) {
    std::cerr << "error: " << err << "\n";
    return 2;
  }
  if (help) {
    std::cout
        << "usage: aiosd --cluster-key UUID [--config PATH] [--listen HOST:PORT]\n"
        << "             [--peer HOST:PORT] [--node-id ID] [--status-file PATH]\n"
        << "\n"
        << "Standalone AIOS daemon: gossip membership + .aios filesystem discovery.\n"
        << "cluster_key is a shared secret (UUID recommended); Hello/Gossip are\n"
        << "authenticated with HMAC-SHA256.\n";
    return 0;
  }

  try {
    boost::asio::io_context ioc;
    g_ioc = &ioc;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    MembershipTable membership;
    FsTable fs_table;
    GossipEngine engine(ioc, cfg, membership, fs_table);
    engine.start();

    AIOS_LOG_INFO("aiosd running");
    ioc.run();
    AIOS_LOG_INFO("aiosd stopped");
  } catch (const std::exception& e) {
    AIOS_LOG_ERROR("fatal: ", e.what());
    return 1;
  }
  return 0;
}
