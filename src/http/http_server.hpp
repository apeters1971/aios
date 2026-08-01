#pragma once

#include "config.hpp"
#include "object/object_service.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <memory>
#include <string>

namespace aios {

class HttpServer {
 public:
  HttpServer(boost::asio::io_context& ioc, Config cfg, ObjectService& objects);

  void start();

  std::uint64_t requests() const { return requests_.load(); }

 private:
  void do_accept();
  void handle_session(std::shared_ptr<boost::asio::ip::tcp::socket> sock);

  boost::asio::io_context& ioc_;
  Config cfg_;
  ObjectService& objects_;
  boost::asio::ip::tcp::acceptor acceptor_;
  std::atomic<std::uint64_t> requests_{0};
};

}  // namespace aios
