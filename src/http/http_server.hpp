#pragma once

#include "config.hpp"
#include "http/backup_policy.hpp"
#include "http/posix_layout_store.hpp"
#include "http/qos_admin.hpp"
#include "http/quota_admin.hpp"
#include "http/s3_iam.hpp"
#include "http/vbd_registry.hpp"
#include "membership.hpp"
#include "object/object_service.hpp"

#include <boost/asio.hpp>

#include <functional>
#include <memory>
#include <string>

namespace aios {

class HttpServer {
 public:
  // cfg is shared with GossipEngine so live admin toggles (node_state, etc.) apply.
  HttpServer(boost::asio::io_context& ioc, Config& cfg, ObjectService& objects,
             MembershipTable& membership, std::shared_ptr<S3IamStore> s3_iam = nullptr,
             std::shared_ptr<QuotaAdminStore> quota = nullptr,
             std::shared_ptr<QosAdminStore> qos = nullptr,
             std::shared_ptr<BackupPolicyStore> backup_policies = nullptr,
             std::shared_ptr<PosixLayoutStore> posix_layout = nullptr,
             std::shared_ptr<VbdRegistryStore> vbd_registry = nullptr);
  ~HttpServer();

  void start();

  // Optional: after lifecycle target rewrite, rescan local .aios markers.
  void set_on_lifecycle_changed(std::function<void()> cb) {
    on_lifecycle_changed_ = std::move(cb);
  }

  std::uint64_t requests() const { return objects_.ops().total().http_requests.load(); }

 private:
  void do_accept();
  void handle_session(std::shared_ptr<boost::asio::ip::tcp::socket> sock);
  nlohmann::json admin_status_json() const;
  nlohmann::json admin_config_json() const;
  nlohmann::json admin_lifecycle_json() const;

  boost::asio::io_context& ioc_;
  Config& cfg_;
  ObjectService& objects_;
  MembershipTable& membership_;
  std::shared_ptr<S3IamStore> s3_iam_;
  std::shared_ptr<QuotaAdminStore> quota_;
  std::shared_ptr<QosAdminStore> qos_;
  std::shared_ptr<BackupPolicyStore> backup_policies_;
  std::shared_ptr<PosixLayoutStore> posix_layout_;
  std::shared_ptr<VbdRegistryStore> vbd_registry_;
  std::function<void()> on_lifecycle_changed_;
  boost::asio::ip::tcp::acceptor acceptor_;
  // Blocking read/write session loop must not run on ioc_ (would stall accepts).
  boost::asio::thread_pool workers_{4};
};

}  // namespace aios
