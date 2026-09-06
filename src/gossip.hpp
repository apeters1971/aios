#pragma once

#include "cluster/cluster_map.hpp"
#include "cluster/map_monitor.hpp"
#include "config.hpp"
#include "fs/fs_table.hpp"
#include "http/backup_policy.hpp"
#include "http/http_server.hpp"
#include "http/space_history.hpp"
#include "http/posix_layout_store.hpp"
#include "http/qos_admin.hpp"
#include "http/vbd_registry.hpp"
#include "http/quota_admin.hpp"
#include "http/s3_iam.hpp"
#include "membership.hpp"
#include "net/server.hpp"
#include "object/object_service.hpp"
#include "store/local_stores.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace aios {

class GossipEngine {
 public:
  GossipEngine(boost::asio::io_context& ioc, Config cfg, MembershipTable& membership,
               FsTable& fs_table);

  void start();
  // Stop acceptors / HTTP workers (call before io_context::stop).
  void stop();
  HttpServer* http() { return http_server_.get(); }

  const ClusterMap& cluster_map() const { return cluster_map_; }
  // Null when Config::monitors is empty (legacy gossip-derived map).
  MapMonitor* map_monitor() { return monitor_.get(); }
  LocalStores& local_stores() { return local_stores_; }
  ObjectService& object_service() { return *object_service_; }
  std::shared_ptr<S3IamStore> s3_iam() const { return s3_iam_; }
  std::shared_ptr<QuotaAdminStore> quota() const { return quota_; }
  std::shared_ptr<QosAdminStore> qos() const { return qos_; }
  std::shared_ptr<BackupPolicyStore> backup_policies() const { return backup_policies_; }
  std::shared_ptr<PosixLayoutStore> posix_layout() const { return posix_layout_; }
  std::shared_ptr<VbdRegistryStore> vbd_registry() const { return vbd_registry_; }

 private:
  void on_gossip_timer(const boost::system::error_code& ec);
  void on_monitor_timer(const boost::system::error_code& ec);
  void on_scan_timer(const boost::system::error_code& ec);
  void on_status_timer(const boost::system::error_code& ec);
  void on_repair_timer(const boost::system::error_code& ec);
  void on_transition_timer(const boost::system::error_code& ec);
  void on_archive_timer(const boost::system::error_code& ec);
  void on_backup_timer(const boost::system::error_code& ec);
  void run_scan();
  void apply_target_weights(std::vector<AiosTarget>& targets);
  void rebuild_cluster_map();
  // Consensus mode: hand the gossip-built content to the monitor (leader
  // proposes it) and publish the committed map + gate instead of the content.
  void publish_consensus_map(ClusterMap content);
  bool is_monitor_addr(const std::string& addr) const;
  bool hub_mode() const { return !cfg_.monitors.empty(); }
  bool self_is_monitor() const { return monitor_ && monitor_->is_voter(); }
  // Copy of the published map. Every read outside the publishing call must go
  // through this: RPC and HTTP worker threads publish concurrently with the timers.
  ClusterMap map_snapshot() const;
  void sync_local_stores();
  void write_status();
  Frame handle_inbound_gossip(const std::string& peer_node_id,
                              const std::string& peer_listen,
                              const std::string& peer_http_addr, const Frame& req);

  std::string advertise_addr() const;

  boost::asio::io_context& ioc_;
  Config cfg_;
  MembershipTable& membership_;
  FsTable& fs_table_;
  ClusterMap cluster_map_;
  LocalStores local_stores_;
  std::unique_ptr<ObjectService> object_service_;
  std::unique_ptr<MapMonitor> monitor_;
  std::uint64_t published_epoch_{0};
  std::shared_ptr<S3IamStore> s3_iam_;
  std::shared_ptr<QuotaAdminStore> quota_;
  std::shared_ptr<QosAdminStore> qos_;
  std::shared_ptr<BackupPolicyStore> backup_policies_;
  std::shared_ptr<PosixLayoutStore> posix_layout_;
  std::shared_ptr<VbdRegistryStore> vbd_registry_;
  std::unique_ptr<TcpServer> server_;
  std::unique_ptr<HttpServer> http_server_;
  std::shared_ptr<SpaceHistory> space_history_;
  // Last advertised autotune weight per aios_path (hysteresis).
  std::unordered_map<std::string, int> autotune_weights_;
  boost::asio::steady_timer gossip_timer_;
  boost::asio::steady_timer monitor_timer_;
  boost::asio::steady_timer scan_timer_;
  boost::asio::steady_timer status_timer_;
  boost::asio::steady_timer repair_timer_;
  boost::asio::steady_timer transition_timer_;
  boost::asio::steady_timer archive_timer_;
  boost::asio::steady_timer backup_timer_;
  // Outbound gossip / repair / lifecycle jobs block on peer RPC; keep them off
  // ioc_ so async_accept (HTTP + object) keeps running.
  boost::asio::thread_pool gossip_workers_{4};
  std::atomic<bool> stopped_{false};
  std::atomic<bool> gossip_workers_joined_{false};
  std::optional<ClusterMap> last_repair_map_;
  std::int64_t last_scrub_ms_{0};
};

}  // namespace aios
