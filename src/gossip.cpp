#include "gossip.hpp"

#include "fs/aios_scan.hpp"
#include "net/client.hpp"
#include "node_id.hpp"
#include "object/repair.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace aios {

GossipEngine::GossipEngine(boost::asio::io_context& ioc, Config cfg,
                           MembershipTable& membership, FsTable& fs_table)
    : ioc_(ioc),
      cfg_(std::move(cfg)),
      membership_(membership),
      fs_table_(fs_table),
      gossip_timer_(ioc),
      scan_timer_(ioc),
      status_timer_(ioc),
      repair_timer_(ioc) {
  object_service_ = std::make_unique<ObjectService>(cfg_, cluster_map_, local_stores_);
}

std::string GossipEngine::advertise_addr() const {
  std::string host, port;
  if (!split_host_port(cfg_.listen, host, port)) {
    return cfg_.listen;
  }
  if (host == "0.0.0.0" || host == "::") {
    return default_hostname() + ":" + port;
  }
  return cfg_.listen;
}

void GossipEngine::rebuild_cluster_map() {
  const auto prev = cluster_map_.epoch;
  cluster_map_ = ClusterMap::build(membership_, fs_table_, cfg_.replica_count);
  if (cluster_map_.epoch != prev) {
    AIOS_LOG_INFO("cluster map epoch=", cluster_map_.epoch,
                  " targets=", cluster_map_.targets.size(),
                  " replica_count=", cluster_map_.replica_count);
  }
}

void GossipEngine::sync_local_stores() {
  std::vector<std::string> paths;
  for (const auto& e : fs_table_.snapshot()) {
    if (e.node_id == cfg_.node_id && e.usable && !e.aios_path.empty()) {
      paths.push_back(e.aios_path);
    }
  }
  ObjectStoreOptions opts;
  // Fewer shards for daemon-managed targets keeps repair listing light.
  opts.shard_count = 16;
  opts.max_versions = cfg_.max_versions;
  opts.clone_required = cfg_.clone_required;
  local_stores_.sync_paths(paths, opts);
}

void GossipEngine::start() {
  const auto adv = advertise_addr();
  const auto http_adv = derive_http_addr(adv, cfg_.http_listen);
  membership_.set_local(cfg_.node_id, adv, http_adv);
  for (const auto& p : cfg_.peers) {
    membership_.add_seed(p);
  }

  object_service_->set_advertise(adv);

  run_scan();
  rebuild_cluster_map();
  sync_local_stores();

  std::string host, port;
  if (!split_host_port(cfg_.listen, host, port)) {
    throw std::runtime_error("invalid listen address: " + cfg_.listen);
  }

  RpcHandlers handlers;
  handlers.local_node_id = cfg_.node_id;
  handlers.local_listen = adv;
  handlers.local_http_addr = http_adv;
  handlers.cluster_key = cfg_.cluster_key;
  handlers.auth_skew_ms = cfg_.auth_skew_ms;
  handlers.on_gossip = [this](const std::string& peer_id, const std::string& peer_listen,
                              const Frame& req) -> std::optional<Frame> {
    return handle_inbound_gossip(peer_id, peer_listen, req);
  };
  handlers.on_object = [this](const Frame& req) { return object_service_->handle(req); };

  server_ = std::make_unique<TcpServer>(ioc_, host, port, std::move(handlers));
  server_->start();

  if (!cfg_.http_listen.empty()) {
    http_server_ = std::make_unique<HttpServer>(ioc_, cfg_, *object_service_, membership_);
    http_server_->start();
    if (cfg_.admin) {
      AIOS_LOG_INFO("admin API enabled on ", cfg_.http_listen,
                    cfg_.admin_metrics_public ? " (metrics public)" : "");
    }
  }

  boost::asio::post(ioc_, [this] {
    for (const auto& p : cfg_.peers) {
      auto r = gossip_with_peer(ioc_, p, cfg_.node_id, advertise_addr(),
                                cfg_.cluster_key, cfg_.auth_skew_ms, membership_,
                                fs_table_, derive_http_addr(advertise_addr(), cfg_.http_listen));
      if (r.ok) {
        AIOS_LOG_INFO("seed gossip ok with ", p, " as ", r.peer_node_id);
        rebuild_cluster_map();
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

  if (cfg_.repair_interval_ms > 0) {
    repair_timer_.expires_after(std::chrono::milliseconds(cfg_.repair_interval_ms));
    repair_timer_.async_wait([this](auto ec) { on_repair_timer(ec); });
  }

  AIOS_LOG_INFO("node ", cfg_.node_id, " advertise=", adv,
                " peers=", cfg_.peers.size(), " auth=hmac-sha256",
                " replica_count=", cfg_.replica_count);
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
  rebuild_cluster_map();

  Frame reply;
  reply.type = MsgType::Gossip;
  reply.body = {
      {"membership", membership_.to_json()},
      {"fs_table", fs_table_.to_json()},
      {"cluster_map", cluster_map_.to_json()},
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
                              fs_table_,
                              derive_http_addr(advertise_addr(), cfg_.http_listen));
    if (r.ok) {
      AIOS_LOG_DEBUG("gossip ok ", p.addr, " -> ", r.peer_node_id);
    } else {
      AIOS_LOG_DEBUG("gossip fail ", p.addr, ": ", r.error);
    }
  }
  rebuild_cluster_map();
  if (object_service_) object_service_->ops().note_gossip_round();

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
  sync_local_stores();
  rebuild_cluster_map();
  scan_timer_.expires_after(std::chrono::milliseconds(cfg_.scan_interval_ms));
  scan_timer_.async_wait([this](auto e) { on_scan_timer(e); });
}

void GossipEngine::on_repair_timer(const boost::system::error_code& ec) {
  if (ec) return;
  rebuild_cluster_map();
  const auto stats =
      run_repair(cfg_, advertise_addr(), cluster_map_, local_stores_,
                 static_cast<std::size_t>(std::max(1, cfg_.repair_batch_oids)));
  if (object_service_) {
    object_service_->ops().note_repair(stats.oids_scanned, stats.repaired, stats.failed);
  }
  if (stats.oids_scanned > 0 || stats.under_replicated > 0) {
    AIOS_LOG_INFO("repair scanned=", stats.oids_scanned,
                  " under_replicated=", stats.under_replicated,
                  " repaired=", stats.repaired, " failed=", stats.failed);
  }
  repair_timer_.expires_after(std::chrono::milliseconds(cfg_.repair_interval_ms));
  repair_timer_.async_wait([this](auto e) { on_repair_timer(e); });
}

void GossipEngine::write_status() {
  nlohmann::json j = {
      {"node_id", cfg_.node_id},
      {"listen", cfg_.listen},
      {"advertise", advertise_addr()},
      {"http_listen", cfg_.http_listen},
      {"admin", cfg_.admin},
      {"http_requests",
       object_service_ ? object_service_->ops().total().http_requests.load() : 0},
      {"replica_count", cfg_.replica_count},
      {"write_quorum", cfg_.write_quorum > 0 ? cfg_.write_quorum : cfg_.replica_count},
      {"membership", membership_.to_json()},
      {"fs_table", fs_table_.to_json()},
      {"cluster_map", cluster_map_.to_json()},
  };
  if (object_service_) {
    auto admin = object_service_->ops().to_admin_json();
    j["ops"] = admin["ops"];
    j["ops_by_label"] = admin["ops_by_label"];
  }
  const auto members = membership_.snapshot();
  std::size_t alive = 0;
  for (const auto& m : members) {
    if (m.state == MemberState::Alive) ++alive;
  }
  AIOS_LOG_INFO("status members=", members.size(), " alive=", alive,
                " fs_entries=", fs_table_.snapshot().size(),
                " map_targets=", cluster_map_.targets.size(),
                " epoch=", cluster_map_.epoch);

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
  rebuild_cluster_map();
  write_status();
  status_timer_.expires_after(std::chrono::seconds(5));
  status_timer_.async_wait([this](auto e) { on_status_timer(e); });
}

}  // namespace aios
