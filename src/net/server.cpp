#include "net/server.hpp"

#include "util/auth.hpp"
#include "util/log.hpp"

#include <arpa/inet.h>

#include <array>
#include <cstring>
#include <vector>

namespace aios {

bool read_frame(tcp::socket& sock, Frame& out, std::string& err,
                boost::system::error_code& ec) {
  std::array<std::uint8_t, kHeaderSize> header{};
  boost::asio::read(sock, boost::asio::buffer(header), ec);
  if (ec) {
    err = ec.message();
    return false;
  }
  if (std::memcmp(header.data(), kMagic, 4) != 0) {
    err = "bad magic";
    return false;
  }
  std::uint32_t len_be = 0;
  std::memcpy(&len_be, header.data() + 8, 4);
  const std::uint32_t body_len = ntohl(len_be);
  if (body_len > kMaxBodySize) {
    err = "body too large";
    return false;
  }
  std::vector<std::uint8_t> buf(kHeaderSize + body_len);
  std::memcpy(buf.data(), header.data(), kHeaderSize);
  if (body_len > 0) {
    boost::asio::read(sock, boost::asio::buffer(buf.data() + kHeaderSize, body_len),
                      ec);
    if (ec) {
      err = ec.message();
      return false;
    }
  }
  std::size_t consumed = 0;
  return decode_frame(buf.data(), buf.size(), out, consumed, err);
}

bool write_frame(tcp::socket& sock, const Frame& frame, std::string& err,
                 boost::system::error_code& ec) {
  try {
    auto bytes = encode_frame(frame);
    boost::asio::write(sock, boost::asio::buffer(bytes), ec);
    if (ec) {
      err = ec.message();
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

namespace {

bool is_object_req(MsgType t) {
  return t == MsgType::ObjectPut || t == MsgType::ObjectGet || t == MsgType::ObjectDel ||
         t == MsgType::ObjectStat || t == MsgType::ObjectPutRange ||
         t == MsgType::ObjectPublishTip || t == MsgType::ObjectAbortVersion ||
         t == MsgType::ObjectListVersions || t == MsgType::ObjectPurgeVersions ||
         t == MsgType::ObjectStageBegin || t == MsgType::ObjectStageData ||
         t == MsgType::ObjectStageCommit || t == MsgType::ObjectList;
}

}  // namespace

TcpServer::TcpServer(boost::asio::io_context& ioc, const std::string& listen_host,
                     const std::string& listen_port, RpcHandlers handlers)
    : ioc_(ioc),
      acceptor_(ioc),
      handlers_(std::move(handlers)) {
  tcp::resolver resolver(ioc_);
  boost::system::error_code ec;
  const auto endpoints = resolver.resolve(listen_host, listen_port, ec);
  if (ec || endpoints.empty()) {
    throw std::runtime_error("resolve " + listen_host + ":" + listen_port + ": " +
                             (ec ? ec.message() : "no endpoints"));
  }
  const tcp::endpoint ep = *endpoints.begin();
  acceptor_.open(ep.protocol(), ec);
  if (ec) throw std::runtime_error("open listen socket: " + ec.message());
  acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
  if (ec) throw std::runtime_error("set reuse_address: " + ec.message());
  acceptor_.bind(ep, ec);
  if (ec) {
    const std::string msg =
        "bind " + listen_host + ":" + listen_port + ": " + ec.message();
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    throw std::runtime_error(msg);
  }
  acceptor_.listen(tcp::socket::max_listen_connections, ec);
  if (ec) {
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    throw std::runtime_error("listen: " + ec.message());
  }
  AIOS_LOG_INFO("listening on ", ep.address().to_string(), ":", ep.port());
}

void TcpServer::start() { do_accept(); }

void TcpServer::close() {
  boost::system::error_code ec;
  acceptor_.close(ec);
  std::unordered_set<std::shared_ptr<tcp::socket>> socks;
  {
    std::lock_guard lock(sessions_mu_);
    socks.swap(sessions_);
  }
  for (const auto& s : socks) {
    boost::system::error_code ignored;
    s->cancel(ignored);
    s->shutdown(tcp::socket::shutdown_both, ignored);
    s->close(ignored);
  }
}

TcpServer::~TcpServer() { close(); }

void TcpServer::do_accept() {
  if (!acceptor_.is_open()) return;
  auto sock = std::make_shared<tcp::socket>(ioc_);
  acceptor_.async_accept(*sock, [this, sock](boost::system::error_code ec) {
    if (!ec) {
      {
        std::lock_guard lock(sessions_mu_);
        sessions_.insert(sock);
      }
      boost::asio::post(ioc_, [this, sock] {
        handle_session(sock);
        std::lock_guard lock(sessions_mu_);
        sessions_.erase(sock);
      });
      do_accept();
    } else if (ec != boost::asio::error::operation_aborted) {
      AIOS_LOG_WARN("accept error: ", ec.message());
    }
  });
}

void TcpServer::handle_session(std::shared_ptr<tcp::socket> sock) {
  boost::system::error_code ec;
  std::string err;

  Frame hello;
  if (!read_frame(*sock, hello, err, ec) || hello.type != MsgType::Hello) {
    AIOS_LOG_DEBUG("inbound hello failed: ", err);
    return;
  }
  if (!auth_verify(hello.body, MsgType::Hello, handlers_.cluster_key,
                   handlers_.auth_skew_ms, err)) {
    AIOS_LOG_WARN("reject hello auth: ", err);
    return;
  }
  const std::string peer_id = hello.body.value("node_id", "");
  const std::string peer_listen = hello.body.value("listen", "");

  Frame hello_reply;
  hello_reply.type = MsgType::Hello;
  hello_reply.body = {
      {"node_id", handlers_.local_node_id},
      {"listen", handlers_.local_listen},
      {"http_addr", handlers_.local_http_addr},
  };
  auth_sign(hello_reply.body, MsgType::Hello, handlers_.cluster_key);
  if (!write_frame(*sock, hello_reply, err, ec)) return;

  // Allow multiple object RPCs per connection (e.g. ObjectStageBegin/Data/Commit).
  for (;;) {
    Frame req;
    if (!read_frame(*sock, req, err, ec)) {
      if (ec && ec != boost::asio::error::eof) {
        AIOS_LOG_DEBUG("inbound request read failed: ", err);
      }
      return;
    }
    if (req.type == MsgType::Ping) {
      Frame pong;
      pong.type = MsgType::Pong;
      write_frame(*sock, pong, err, ec);
      continue;
    }
    if (req.type == MsgType::Gossip) {
      if (!handlers_.on_gossip) return;
      if (!auth_verify(req.body, MsgType::Gossip, handlers_.cluster_key,
                       handlers_.auth_skew_ms, err)) {
        AIOS_LOG_WARN("reject gossip auth from ", peer_id, ": ", err);
        return;
      }
      auto gossip_reply = handlers_.on_gossip(peer_id, peer_listen, req);
      if (!gossip_reply) return;
      auth_sign(gossip_reply->body, MsgType::Gossip, handlers_.cluster_key);
      write_frame(*sock, *gossip_reply, err, ec);
      return;  // gossip sessions are one-shot
    }
    if (is_object_req(req.type)) {
      if (!handlers_.on_object) return;
      if (!auth_verify(req.body, req.type, handlers_.cluster_key, handlers_.auth_skew_ms,
                       err)) {
        AIOS_LOG_WARN("reject object auth from ", peer_id, ": ", err);
        return;
      }
      Frame reply = handlers_.on_object(req);
      if (reply.type != MsgType::ObjectReply) {
        reply.type = MsgType::ObjectReply;
      }
      auth_sign(reply.body, MsgType::ObjectReply, handlers_.cluster_key);
      if (!write_frame(*sock, reply, err, ec)) return;
      continue;
    }
    AIOS_LOG_DEBUG("unsupported inbound type ", static_cast<int>(req.type));
    return;
  }
}

}  // namespace aios
