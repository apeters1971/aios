#pragma once

#include "config.hpp"
#include "http/s3_iam.hpp"

#include <boost/asio.hpp>

#include <memory>
#include <string>

struct aios_posix_fs;

namespace aios {

// S3-compatible HTTP front-end backed by libaios_posix (shared namespace with FUSE/aiosfs).
class S3Server {
 public:
  // posix_http_endpoint: HOST:PORT for aios_posix_mount (usually 127.0.0.1:<http port>).
  S3Server(boost::asio::io_context& ioc, Config cfg, std::string posix_http_endpoint,
           std::shared_ptr<S3IamStore> iam = nullptr);
  ~S3Server();

  S3Server(const S3Server&) = delete;
  S3Server& operator=(const S3Server&) = delete;

  void start();

 private:
  void do_accept();
  void handle_session(std::shared_ptr<boost::asio::ip::tcp::socket> sock);

  boost::asio::io_context& ioc_;
  Config cfg_;
  std::string posix_endpoint_;
  std::shared_ptr<S3IamStore> iam_;
  aios_posix_fs* fs_{nullptr};
  boost::asio::ip::tcp::acceptor acceptor_;
};

// Dialable 127.0.0.1:PORT from http_listen (maps 0.0.0.0 / :: → 127.0.0.1).
std::string s3_loopback_http_endpoint(const std::string& http_listen);

}  // namespace aios
