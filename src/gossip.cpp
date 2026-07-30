#include "gossip.hpp"

#include "fs/aios_scan.hpp"
#include "net/client.hpp"
#include "node_id.hpp"
#include "util/log.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace aios {

GossipEngine::GossipEngine(boost::asio::io_context& ioc, Config cfg,
                           MembershipTable& membership, FsTable& fs_table)
    : ioc_(ioc),
      cfg_(std::move(cfg)),
      membership_(membership),
      fs_table_(fs_table),
      gossip_timer_(ioc),
      scan_timer_(ioc),
      status_timer_(ioc) {}

std::string GossipEngine::advertise_addr() const {
  std::string host, port;
  if (!split_host_port(cfg_.listen, host, port)) {
    return cfg_.listen;
  }
  if (host == "0.0.0.0" || host == "::") {
    // Prefer hostname for peer dialing when bound on all interfaces.
    return default_hostname() + ":" + port;
  }
  return cfg_.listen;
}

void GossipEngine::start() {
  const auto adv = advertise_addr();
  membership_.set_local(cfg_.node_id, adv);
  for (const auto& p : cfg_.peers) {
    membership_.add_seed(p);
  }

  run_scan();

  std::string host, port;
  if (!split_host_port(cfg_.listen, host, port)) {
    throw std::runtime_error("invalid listen address: " + cfg_.listen);
  }

  GossipHandlers handlers;
  handlers.local_node_id = cfg_.node_id;
  handlers.local_listen = adv;
  handlers.cluster_key = cfg_.cluster_key;
  handlers.auth_skew_ms = cfg_.auth_skew_ms;
  handlers.on_gossip = [this](const std::string& peer_id, const std::string& peer_listen,
                              const Frame& req) -> std::optional<Frame> {
    return handle_inbound_gossip(peer_id, peer_listen, req);
  };

  server_ = std::make_unique<TcpServer>(ioc_, host, port, std::move(handlers));
  server_->start();

  // Immediate seed contact.
  boost::asio::post(ioc_, [this] {
    for (const auto& p : cfg_.peers) {
      auto r = gossip_with_peer(ioc_, p, cfg_.node_id, advertise_addr(),
                                cfg_.cluster_key, cfg_.auth_skew_ms, membership_,
                                fs_table_);
      if (r.ok) {
        AIOS_LOG_INFO("seed gossip ok with ", p, " as ", r.peer_node_id);
        write_status();
      } else {
        AIOS_LOG_WARN("seed gossip failed ", p, ": ", r.error);
      }
    }
  });

  gossip_timer_.expires_after(std::chrono::milliseconds(cfg_.gossip_interval_ms));
  gossip_timer_.async_wait([this](auto ec) { on_gossip_timer(ec); });

  scan_timer_.expires_after(std::chrono::milliseconds(cfg_.scan_interval_ms));
  scan_timer_.async_wait([this](auto ec) { on_scan_timer(ec); });

  write_status();
  status_timer_.expires_after(std::chrono::seconds(5));
  status_timer_.async_wait([this](auto ec) { on_status_timer(ec); });

  AIOS_LOG_INFO("node ", cfg_.node_id, " advertise=", adv,
                " peers=", cfg_.peers.size(), " auth=hmac-sha256");
}

Frame GossipEngine::handle_inbound_gossip(const std::string& peer_node_id,
                                          const std::string& peer_listen,
                                          const Frame& req) {
  const auto now = now_ms();
  if (!peer_node_id.empty()) {
    membership_.mark_alive(peer_node_id,
                           peer_listen.empty() ? std::string{} : peer_listen, now);
  }
  if (req.body.contains("membership")) {
    membership_.merge(MembershipTable::from_json(req.body["membership"]), now);
  }
  if (req.body.contains("fs_table")) {
    fs_table_.merge(FsTable::from_json(req.body["fs_table"]));
  }

  Frame reply;
  reply.type = MsgType::Gossip;
  reply.body = {
      {"membership", membership_.to_json()},
      {"fs_table", fs_table_.to_json()},
  };
  return reply;
}

void GossipEngine::on_gossip_timer(const boost::system::error_code& ec) {
  if (ec) return;
  const auto now = now_ms();
  membership_.age(now, cfg_.suspect_after_ms, cfg_.dead_after_ms);

  auto peers = membership_.peers_for_gossip(3);
  for (const auto& p : peers) {
    auto r = gossip_with_peer(ioc_, p.addr, cfg_.node_id, advertise_addr(),
                              cfg_.cluster_key, cfg_.auth_skew_ms, membership_,
                              fs_table_);
    if (r.ok) {
      AIOS_LOG_DEBUG("gossip ok ", p.addr, " -> ", r.peer_node_id);
    } else {
      AIOS_LOG_DEBUG("gossip fail ", p.addr, ": ", r.error);
    }
  }

  gossip_timer_.expires_after(std::chrono::milliseconds(cfg_.gossip_interval_ms));
  gossip_timer_.async_wait([this](auto e) { on_gossip_timer(e); });
}

void GossipEngine::run_scan() {
  auto targets = scan_aios_filesystems();
  std::size_t usable = 0;
  for (const auto& t : targets) {
    if (t.usable) {
      ++usable;
      AIOS_LOG_DEBUG("target usable ", t.aios_path, " bavail=", t.bavail);
    } else if (!t.error.empty()) {
      AIOS_LOG_WARN("target unusable ", t.aios_path, ": ", t.error);
    }
  }
  fs_table_.set_local(cfg_.node_id, targets);
  AIOS_LOG_INFO("fs scan: ", targets.size(), " targets, ", usable, " usable");
}

void GossipEngine::on_scan_timer(const boost::system::error_code& ec) {
  if (ec) return;
  run_scan();
  scan_timer_.expires_after(std::chrono::milliseconds(cfg_.scan_interval_ms));
  scan_timer_.async_wait([this](auto e) { on_scan_timer(e); });
}

void GossipEngine::write_status() {
  nlohmann::json j = {
      {"node_id", cfg_.node_id},
      {"listen", cfg_.listen},
      {"advertise", advertise_addr()},
      {"membership", membership_.to_json()},
      {"fs_table", fs_table_.to_json()},
  };
  const auto members = membership_.snapshot();
  std::size_t alive = 0;
  for (const auto& m : members) {
    if (m.state == MemberState::Alive) ++alive;
  }
  AIOS_LOG_INFO("status members=", members.size(), " alive=", alive,
                " fs_entries=", fs_table_.snapshot().size());

  if (cfg_.status_file.empty()) return;
  const std::string tmp = cfg_.status_file + ".tmp";
  {
    std::ofstream out(tmp);
    if (!out) {
      AIOS_LOG_WARN("cannot write status tmp ", tmp);
      return;
    }
    out << j.dump(2) << '\n';
  }
  std::error_code ec;
  std::filesystem::rename(tmp, cfg_.status_file, ec);
  if (ec) {
    AIOS_LOG_WARN("status rename failed: ", ec.message());
  }
}

void GossipEngine::on_status_timer(const boost::system::error_code& ec) {
  if (ec) return;
  write_status();
  status_timer_.expires_after(std::chrono::seconds(5));
  status_timer_.async_wait([this](auto e) { on_status_timer(e); });
}

}  // namespace aios
