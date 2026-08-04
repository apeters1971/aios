#pragma once

#include "config.hpp"
#include "http/s3_iam.hpp"
#include "membership.hpp"
#include "object/object_service.hpp"

#include <boost/asio.hpp>

#include <memory>
#include <string>

namespace aios {

class HttpServer {
 public:
  HttpServer(boost::asio::io_context& ioc, Config cfg, ObjectService& objects,
             MembershipTable& membership, std::shared_ptr<S3IamStore> s3_iam = nullptr);

  void start();

  std::uint64_t requests() const { return objects_.ops().total().http_requests.load(); }

 private:
  void do_accept();
  void handle_session(std::shared_ptr<boost::asio::ip::tcp::socket> sock);
  nlohmann::json admin_status_json() const;
  nlohmann::json admin_config_json() const;

  boost::asio::io_context& ioc_;
  Config cfg_;
  ObjectService& objects_;
  MembershipTable& membership_;
  std::shared_ptr<S3IamStore> s3_iam_;
  boost::asio::ip::tcp::acceptor acceptor_;
};

}  // namespace aios
