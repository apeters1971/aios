#pragma once

#include "cluster/cluster_map.hpp"
#include "config.hpp"
#include "fs/fs_table.hpp"
#include "http/backup_policy.hpp"
#include "http/http_server.hpp"
#include "http/posix_layout_store.hpp"
#include "http/qos_admin.hpp"
#include "http/quota_admin.hpp"
#include "http/s3_iam.hpp"
#include "membership.hpp"
#include "net/server.hpp"
#include "object/object_service.hpp"
#include "store/local_stores.hpp"

#include <boost/asio.hpp>

#include <memory>
#include <string>

namespace aios {

class GossipEngine {
 public:
  GossipEngine(boost::asio::io_context& ioc, Config cfg, MembershipTable& membership,
               FsTable& fs_table);

  void start();

  const ClusterMap& cluster_map() const { return cluster_map_; }
  LocalStores& local_stores() { return local_stores_; }
  ObjectService& object_service() { return *object_service_; }
  std::shared_ptr<S3IamStore> s3_iam() const { return s3_iam_; }
  std::shared_ptr<QuotaAdminStore> quota() const { return quota_; }
  std::shared_ptr<QosAdminStore> qos() const { return qos_; }
  std::shared_ptr<BackupPolicyStore> backup_policies() const { return backup_policies_; }
  std::shared_ptr<PosixLayoutStore> posix_layout() const { return posix_layout_; }

 private:
  void on_gossip_timer(const boost::system::error_code& ec);
  void on_scan_timer(const boost::system::error_code& ec);
  void on_status_timer(const boost::system::error_code& ec);
  void on_repair_timer(const boost::system::error_code& ec);
  void on_transition_timer(const boost::system::error_code& ec);
  void on_archive_timer(const boost::system::error_code& ec);
  void on_backup_timer(const boost::system::error_code& ec);
  void run_scan();
  void rebuild_cluster_map();
  void sync_local_stores();
  void write_status();
  Frame handle_inbound_gossip(const std::string& peer_node_id,
                              const std::string& peer_listen, const Frame& req);

  std::string advertise_addr() const;

  boost::asio::io_context& ioc_;
  Config cfg_;
  MembershipTable& membership_;
  FsTable& fs_table_;
  ClusterMap cluster_map_;
  LocalStores local_stores_;
  std::unique_ptr<ObjectService> object_service_;
  std::shared_ptr<S3IamStore> s3_iam_;
  std::shared_ptr<QuotaAdminStore> quota_;
  std::shared_ptr<QosAdminStore> qos_;
  std::shared_ptr<BackupPolicyStore> backup_policies_;
  std::shared_ptr<PosixLayoutStore> posix_layout_;
  std::unique_ptr<TcpServer> server_;
  std::unique_ptr<HttpServer> http_server_;
  boost::asio::steady_timer gossip_timer_;
  boost::asio::steady_timer scan_timer_;
  boost::asio::steady_timer status_timer_;
  boost::asio::steady_timer repair_timer_;
  boost::asio::steady_timer transition_timer_;
  boost::asio::steady_timer archive_timer_;
  boost::asio::steady_timer backup_timer_;
};

}  // namespace aios
