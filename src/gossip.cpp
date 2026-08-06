#include "gossip.hpp"

#include "fs/aios_scan.hpp"
#include "net/client.hpp"
#include "node_id.hpp"
#include "object/archive_pack.hpp"
#include "object/archive_tape.hpp"
#include "object/backup.hpp"
#include "object/repair.hpp"
#include "object/transition.hpp"
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
      repair_timer_(ioc),
      transition_timer_(ioc),
      archive_timer_(ioc),
      backup_timer_(ioc) {
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
  PlacementConfig pc;
  pc.vnodes_per_target = cfg_.vnodes_per_target;
  pc.min_vnodes = cfg_.min_vnodes;
  pc.max_vnodes = cfg_.max_vnodes;
  cluster_map_ = ClusterMap::build(membership_, fs_table_, cfg_.replica_count, pc);
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
    if (!cfg_.s3_listen.empty()) {
      s3_iam_ = std::make_shared<S3IamStore>(cfg_, *object_service_);
    }
    if (cfg_.admin) {
      quota_ = std::make_shared<QuotaAdminStore>(cfg_, *object_service_);
      qos_ = std::make_shared<QosAdminStore>(cfg_, *object_service_);
      backup_policies_ = std::make_shared<BackupPolicyStore>(cfg_, *object_service_);
      posix_layout_ = std::make_shared<PosixLayoutStore>(cfg_, *object_service_);
      posix_layout_->seed_from_config_if_empty();
    }
    http_server_ = std::make_unique<HttpServer>(ioc_, cfg_, *object_service_, membership_,
                                                s3_iam_, quota_, qos_, backup_policies_,
                                                posix_layout_);
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
  if (cfg_.transition_interval_ms > 0 && !cfg_.transition_rules.empty()) {
    transition_timer_.expires_after(std::chrono::milliseconds(cfg_.transition_interval_ms));
    transition_timer_.async_wait([this](auto ec) { on_transition_timer(ec); });
  }
  if (cfg_.archive_interval_ms > 0 && !cfg_.archive_rules.empty()) {
    archive_timer_.expires_after(std::chrono::milliseconds(cfg_.archive_interval_ms));
    archive_timer_.async_wait([this](auto ec) { on_archive_timer(ec); });
  }
  {
    const bool want_yaml = !cfg_.backup_rules.empty();
    const bool want_live = backup_policies_ != nullptr;
    int poll_ms = cfg_.backup_interval_ms;
    if (want_live && (poll_ms <= 0 || (!want_yaml && poll_ms > 60000))) poll_ms = 60000;
    if (poll_ms > 0 && (want_yaml || want_live)) {
      backup_timer_.expires_after(std::chrono::milliseconds(poll_ms));
      backup_timer_.async_wait([this](auto ec) { on_backup_timer(ec); });
    }
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

void GossipEngine::on_transition_timer(const boost::system::error_code& ec) {
  if (ec) return;
  rebuild_cluster_map();
  const auto stats = run_transitions(
      cfg_, advertise_addr(), cluster_map_, local_stores_,
      static_cast<std::size_t>(std::max(1, cfg_.transition_batch_oids)));
  if (stats.matched > 0 || stats.migrated > 0 || stats.drained > 0) {
    AIOS_LOG_INFO("transition scanned=", stats.oids_scanned, " matched=", stats.matched,
                  " migrated=", stats.migrated, " drained=", stats.drained,
                  " failed=", stats.failed);
  }
  transition_timer_.expires_after(std::chrono::milliseconds(cfg_.transition_interval_ms));
  transition_timer_.async_wait([this](auto e) { on_transition_timer(e); });
}

void GossipEngine::on_archive_timer(const boost::system::error_code& ec) {
  if (ec) return;
  rebuild_cluster_map();
  const auto batch = static_cast<std::size_t>(std::max(1, cfg_.archive_batch_oids));
  const auto stats = run_archive(cfg_, advertise_addr(), cluster_map_, local_stores_, batch);
  if (stats.matched > 0 || stats.bags_sealed > 0 || stats.packed > 0) {
    AIOS_LOG_INFO("archive scanned=", stats.oids_scanned, " matched=", stats.matched,
                  " packed=", stats.packed, " bags=", stats.bags_sealed,
                  " failed=", stats.failed);
  }
  const auto drain = run_archive_drain(cfg_, advertise_addr(), cluster_map_, local_stores_, batch);
  if (drain.drained > 0 || drain.failed > 0) {
    AIOS_LOG_INFO("archive drain scanned=", drain.bags_scanned, " drained=", drain.drained,
                  " skipped=", drain.skipped, " failed=", drain.failed);
  }
  archive_timer_.expires_after(std::chrono::milliseconds(cfg_.archive_interval_ms));
  archive_timer_.async_wait([this](auto e) { on_archive_timer(e); });
}

void GossipEngine::on_backup_timer(const boost::system::error_code& ec) {
  if (ec) return;
  rebuild_cluster_map();
  const auto batch = static_cast<std::size_t>(std::max(1, cfg_.backup_batch_oids));
  const auto stats =
      run_backup(cfg_, advertise_addr(), cluster_map_, local_stores_, *object_service_, batch,
                 backup_policies_.get(), false);
  if (stats.snaps_created > 0 || stats.bags_sealed > 0 || stats.drained > 0) {
    AIOS_LOG_INFO("backup rules=", stats.rules_run, " snaps=", stats.snaps_created,
                  " oids=", stats.oids_copied, " bags=", stats.bags_sealed,
                  " drained=", stats.drained, " pruned=", stats.pruned,
                  " failed=", stats.failed);
  }
  int poll_ms = cfg_.backup_interval_ms;
  if (backup_policies_ && (poll_ms <= 0 || (cfg_.backup_rules.empty() && poll_ms > 60000)))
    poll_ms = 60000;
  if (poll_ms <= 0) poll_ms = 60000;
  backup_timer_.expires_after(std::chrono::milliseconds(poll_ms));
  backup_timer_.async_wait([this](auto e) { on_backup_timer(e); });
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
