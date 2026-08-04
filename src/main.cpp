#include "config.hpp"
#include "fs/fs_table.hpp"
#include "gossip.hpp"
#include "http/s3_server.hpp"
#include "membership.hpp"
#include "util/log.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>

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
        << "             [--replica-count N] [--write-quorum N]\n"
        << "             [--http-listen HOST:PORT] [--admin] [--admin-metrics-public]\n"
        << "             [--s3-listen HOST:PORT] [--s3-volume NAME] [--s3-access-key ID]\n"
        << "\n"
        << "Standalone AIOS daemon: gossip membership, .aios discovery,\n"
        << "server-side primary replication, HTTP object API, and optional S3 API.\n"
        << "cluster_key is a shared secret (UUID recommended); Hello/Gossip/object\n"
        << "RPCs and HTTP Authorization use HMAC-SHA256. S3 uses AWS SigV4 with\n"
        << "s3_access_key and secret=cluster_key (FS-backed via libaios_posix).\n"
        << "--admin enables /admin/* and /metrics on http_listen.\n"
        << "--admin-metrics-public allows unauthenticated GET /metrics (scrape).\n";
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

    std::unique_ptr<S3Server> s3;
    if (!cfg.s3_listen.empty()) {
      auto endpoint = s3_loopback_http_endpoint(cfg.http_listen);
      if (endpoint.empty()) {
        throw std::runtime_error("cannot derive loopback HTTP endpoint for S3 posix mount");
      }
      s3 = std::make_unique<S3Server>(ioc, cfg, endpoint);
      s3->start();
    }

    AIOS_LOG_INFO("aiosd running");
    ioc.run();
    AIOS_LOG_INFO("aiosd stopped");
  } catch (const std::exception& e) {
    AIOS_LOG_ERROR("fatal: ", e.what());
    return 1;
  }
  return 0;
}
