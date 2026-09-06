#include "net/server.hpp"

#include "util/auth.hpp"
#include "util/log.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

namespace aios {
namespace {

// Timed transfers go through the native fd: asio's synchronous ops ignore
// SO_RCVTIMEO (they poll(-1) on EWOULDBLOCK), so poll() the fd ourselves.
// Works whether asio left the fd blocking or non-blocking.
bool wait_fd(int fd, short events, int timeout_ms, boost::system::error_code& ec) {
  for (;;) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = events;
    const int r = ::poll(&pfd, 1, timeout_ms);
    if (r > 0) return true;
    if (r == 0) {
      ec = boost::asio::error::timed_out;
      return false;
    }
    if (errno == EINTR) continue;
    ec = boost::system::error_code(errno, boost::system::system_category());
    return false;
  }
}

bool recv_exact_timed(tcp::socket& sock, std::uint8_t* p, std::size_t n, int timeout_ms,
                      boost::system::error_code& ec) {
  const int fd = static_cast<int>(sock.native_handle());
  while (n > 0) {
    if (!wait_fd(fd, POLLIN, timeout_ms, ec)) return false;
    const ssize_t r = ::recv(fd, p, n, 0);
    if (r > 0) {
      p += r;
      n -= static_cast<std::size_t>(r);
      continue;
    }
    if (r == 0) {
      ec = boost::asio::error::eof;
      return false;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
    ec = boost::system::error_code(errno, boost::system::system_category());
    return false;
  }
  return true;
}

bool send_all_timed(tcp::socket& sock, const std::uint8_t* p, std::size_t n, int timeout_ms,
                    boost::system::error_code& ec) {
  const int fd = static_cast<int>(sock.native_handle());
  while (n > 0) {
    if (!wait_fd(fd, POLLOUT, timeout_ms, ec)) return false;
#ifdef MSG_NOSIGNAL
    const ssize_t r = ::send(fd, p, n, MSG_NOSIGNAL);
#else
    const ssize_t r = ::send(fd, p, n, 0);
#endif
    if (r >= 0) {
      p += r;
      n -= static_cast<std::size_t>(r);
      continue;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
    ec = boost::system::error_code(errno, boost::system::system_category());
    return false;
  }
  return true;
}

bool read_exact(tcp::socket& sock, std::uint8_t* p, std::size_t n, int timeout_ms,
                boost::system::error_code& ec) {
  if (timeout_ms < 0) {
    boost::asio::read(sock, boost::asio::buffer(p, n), ec);
    return !ec;
  }
  return recv_exact_timed(sock, p, n, timeout_ms, ec);
}

}  // namespace

bool read_frame(tcp::socket& sock, Frame& out, std::string& err,
                boost::system::error_code& ec, int timeout_ms) {
  out = Frame{};
  std::array<std::uint8_t, kHeaderSize> header{};
  if (!read_exact(sock, header.data(), header.size(), timeout_ms, ec)) {
    err = ec.message();
    return false;
  }
  if (std::memcmp(header.data(), kMagic, 4) != 0) {
    err = "bad magic";
    return false;
  }
  if (header[4] != kProtoVersion) {
    err = "unsupported version";
    return false;
  }
  const auto type = static_cast<MsgType>(header[5]);
  std::uint16_t flags_be = 0;
  std::memcpy(&flags_be, header.data() + 6, 2);
  const std::uint16_t flags = ntohs(flags_be);
  std::uint32_t len_be = 0;
  std::memcpy(&len_be, header.data() + 8, 4);
  const std::uint32_t body_len = ntohl(len_be);
  if (body_len > kMaxBodySize) {
    err = "body too large";
    return false;
  }

  std::vector<std::uint8_t> body(body_len);
  if (body_len > 0) {
    if (!read_exact(sock, body.data(), body.size(), timeout_ms, ec)) {
      err = ec.message();
      return false;
    }
  }

  nlohmann::json json_body = nlohmann::json::object();
  if (flags & kFlagRawBody) {
    if (body_len < 4) {
      err = "raw body missing json_len";
      return false;
    }
    std::uint32_t jlen_be = 0;
    std::memcpy(&jlen_be, body.data(), 4);
    const std::uint32_t jlen = ntohl(jlen_be);
    if (4u + jlen > body_len) {
      err = "raw body json_len overflow";
      return false;
    }
    if (jlen > 0) {
      try {
        json_body = nlohmann::json::parse(reinterpret_cast<const char*>(body.data() + 4),
                                          reinterpret_cast<const char*>(body.data() + 4 + jlen));
      } catch (const std::exception& e) {
        err = std::string("json parse: ") + e.what();
        return false;
      }
    }
    // Keep the recv buffer; payload starts after the JSON envelope (no memcpy).
    out.raw = std::move(body);
    out.raw_off = 4u + jlen;
  } else if (body_len > 0) {
    try {
      json_body = nlohmann::json::parse(reinterpret_cast<const char*>(body.data()),
                                        reinterpret_cast<const char*>(body.data() + body_len));
    } catch (const std::exception& e) {
      err = std::string("json parse: ") + e.what();
      return false;
    }
  }

  out.type = type;
  out.flags = flags;
  out.body = std::move(json_body);
  err.clear();
  return true;
}

bool write_frame(tcp::socket& sock, const Frame& frame, std::string& err,
                 boost::system::error_code& ec, int timeout_ms) {
  try {
    // Large raw stage/get-range frames: gather-write header+json and raw to avoid
    // an extra full-body memcpy through encode_frame's contiguous buffer.
    if (!frame.raw_empty() || (frame.flags & kFlagRawBody)) {
      const std::string json = frame.body.dump();
      if (json.size() > 0xffffffffu) {
        err = "json too large";
        return false;
      }
      const auto raw_n = frame.raw_size();
      const auto body_len = static_cast<std::uint32_t>(4 + json.size() + raw_n);
      if (body_len > kMaxBodySize) {
        err = "frame body too large";
        return false;
      }
      const std::uint16_t flags = static_cast<std::uint16_t>(frame.flags | kFlagRawBody);
      std::vector<std::uint8_t> head(kHeaderSize + 4 + json.size());
      std::memcpy(head.data(), kMagic, 4);
      head[4] = kProtoVersion;
      head[5] = static_cast<std::uint8_t>(frame.type);
      const std::uint16_t flags_be = htons(flags);
      std::memcpy(head.data() + 6, &flags_be, 2);
      const std::uint32_t len_be = htonl(body_len);
      std::memcpy(head.data() + 8, &len_be, 4);
      const std::uint32_t jlen_be = htonl(static_cast<std::uint32_t>(json.size()));
      std::memcpy(head.data() + kHeaderSize, &jlen_be, 4);
      if (!json.empty()) {
        std::memcpy(head.data() + kHeaderSize + 4, json.data(), json.size());
      }
      if (timeout_ms >= 0) {
        if (!send_all_timed(sock, head.data(), head.size(), timeout_ms, ec) ||
            (raw_n > 0 && !send_all_timed(sock, frame.raw_data(), raw_n, timeout_ms, ec))) {
          err = ec.message();
          return false;
        }
        return true;
      }
      if (raw_n == 0) {
        boost::asio::write(sock, boost::asio::buffer(head), ec);
      } else {
        std::array<boost::asio::const_buffer, 2> bufs{
            boost::asio::buffer(head),
            boost::asio::buffer(frame.raw_data(), raw_n),
        };
        boost::asio::write(sock, bufs, ec);
      }
      if (ec) {
        err = ec.message();
        return false;
      }
      return true;
    }
    auto bytes = encode_frame(frame);
    if (timeout_ms >= 0) {
      if (!send_all_timed(sock, bytes.data(), bytes.size(), timeout_ms, ec)) {
        err = ec.message();
        return false;
      }
      return true;
    }
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
         t == MsgType::ObjectStageCommit || t == MsgType::ObjectList ||
         t == MsgType::ObjectInstallRange;
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

namespace {

void force_close_socket(tcp::socket& s) {
  boost::system::error_code ignored;
  // shutdown() on the native fd wakes a blocking poll/recv in another thread;
  // asio close() alone is not reliable for that on macOS.
  if (s.is_open()) {
    const int fd = static_cast<int>(s.native_handle());
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
  }
  s.cancel(ignored);
  s.shutdown(tcp::socket::shutdown_both, ignored);
  s.close(ignored);
}

void wake_socket_fd(tcp::socket& s) {
  const int fd = static_cast<int>(s.native_handle());
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
}

// Asio sockets are not thread-safe: close the acceptor on the io_context thread
// when that thread is still running. dispatch() runs inline if we are already on it.
void close_acceptor_on_ioc(boost::asio::io_context& ioc, tcp::acceptor& acceptor) {
  if (ioc.stopped()) {
    boost::system::error_code ec;
    acceptor.close(ec);
    return;
  }
  std::promise<void> done;
  auto fut = done.get_future();
  boost::asio::dispatch(ioc, [&acceptor, &done] {
    boost::system::error_code ec;
    acceptor.close(ec);
    done.set_value();
  });
  fut.wait();
}

}  // namespace

void TcpServer::start() { do_accept(); }

void TcpServer::kick_sessions() {
  // Hold the lock for the native shutdown so we never call is_open()/native_handle()
  // concurrently with force_close_socket() on a worker.
  std::lock_guard lock(sessions_mu_);
  for (const auto& s : sessions_) {
    if (s) wake_socket_fd(*s);
  }
}

void TcpServer::close() {
  closing_.store(true, std::memory_order_release);
  close_acceptor_on_ioc(ioc_, acceptor_);

  // ioc_ may still be running, so a late accept can insert a keep-alive session
  // after a one-shot snapshot. Keep waking sockets until workers drain.
  std::atomic<bool> joining{true};
  std::thread waker([this, &joining] {
    while (joining.load(std::memory_order_acquire)) {
      kick_sessions();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  kick_sessions();

  if (!workers_drained_.exchange(true)) {
    // Any accept callback that posts to workers_ holds sessions_mu_ across the
    // post, so this lock is a barrier: no post can land after stop().
    {
      std::lock_guard lock(sessions_mu_);
    }
    workers_.stop();
    workers_.join();
  }
  joining.store(false, std::memory_order_release);
  waker.join();
}

TcpServer::~TcpServer() { close(); }

void TcpServer::do_accept() {
  if (closing_.load(std::memory_order_acquire) || !acceptor_.is_open()) return;
  auto sock = std::make_shared<tcp::socket>(ioc_);
  acceptor_.async_accept(*sock, [this, sock](boost::system::error_code ec) {
    if (!ec) {
      bool drop = false;
      {
        std::lock_guard lock(sessions_mu_);
        if (closing_.load(std::memory_order_acquire) || workers_drained_.load()) {
          drop = true;
        } else {
          sessions_.insert(sock);
          boost::asio::post(workers_, [this, sock] { handle_session(sock); });
        }
      }
      if (drop) {
        force_close_socket(*sock);
        return;
      }
      do_accept();
    } else if (ec != boost::asio::error::operation_aborted) {
      AIOS_LOG_WARN("accept error: ", ec.message());
    }
  });
}

namespace {

// Peer-supplied JSON fields: a wrong type must never throw into the session.
std::string json_str_or(const nlohmann::json& body, const char* key) {
  if (!body.is_object()) return {};
  auto it = body.find(key);
  if (it == body.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

}  // namespace

void TcpServer::handle_session(std::shared_ptr<tcp::socket> sock) {
  // Sessions run on a thread_pool: an escaped exception is std::terminate for the
  // whole daemon, and everything below parses unauthenticated peer input.
  try {
    run_session(*sock);
  } catch (const std::exception& e) {
    AIOS_LOG_WARN("inbound session aborted: ", e.what());
  } catch (...) {
    AIOS_LOG_WARN("inbound session aborted: unknown exception");
  }
  std::lock_guard lock(sessions_mu_);
  force_close_socket(*sock);
  sessions_.erase(sock);
}

void TcpServer::run_session(tcp::socket& sock) {
  boost::system::error_code ec;
  std::string err;
  sock.set_option(tcp::no_delay(true), ec);
  ec.clear();
  const int pre_hello = handlers_.pre_hello_timeout_ms;
  const int idle = handlers_.idle_timeout_ms;

  Frame hello;
  if (!read_frame(sock, hello, err, ec, pre_hello) || hello.type != MsgType::Hello) {
    AIOS_LOG_DEBUG("inbound hello failed: ", err);
    return;
  }
  if (!auth_verify(hello.body, MsgType::Hello, handlers_.cluster_key,
                   handlers_.auth_skew_ms, err)) {
    AIOS_LOG_WARN("reject hello auth: ", err);
    return;
  }
  const std::string peer_id = json_str_or(hello.body, "node_id");
  const std::string peer_listen = json_str_or(hello.body, "listen");
  const std::string peer_http = json_str_or(hello.body, "http_addr");

  Frame hello_reply;
  hello_reply.type = MsgType::Hello;
  hello_reply.body = {
      {"node_id", handlers_.local_node_id},
      {"listen", handlers_.local_listen},
      {"http_addr", handlers_.local_http_addr},
  };
  auth_sign(hello_reply.body, MsgType::Hello, handlers_.cluster_key);
  if (!write_frame(sock, hello_reply, err, ec, pre_hello)) return;

  // Allow multiple object RPCs per connection (e.g. ObjectStageBegin/Data/Commit).
  for (;;) {
    if (closing_.load(std::memory_order_acquire)) return;
    Frame req;
    if (!read_frame(sock, req, err, ec, idle)) {
      if (ec && ec != boost::asio::error::eof) {
        AIOS_LOG_DEBUG("inbound request read failed: ", err);
      }
      return;
    }
    if (req.type == MsgType::Ping) {
      Frame pong;
      pong.type = MsgType::Pong;
      write_frame(sock, pong, err, ec, idle);
      continue;
    }
    if (req.type == MsgType::Gossip) {
      if (!handlers_.on_gossip) return;
      if (!auth_verify(req.body, MsgType::Gossip, handlers_.cluster_key,
                       handlers_.auth_skew_ms, err)) {
        AIOS_LOG_WARN("reject gossip auth from ", peer_id, ": ", err);
        return;
      }
      auto gossip_reply = handlers_.on_gossip(peer_id, peer_listen, peer_http, req);
      if (!gossip_reply) return;
      auth_sign(gossip_reply->body, MsgType::Gossip, handlers_.cluster_key);
      write_frame(sock, *gossip_reply, err, ec, idle);
      return;  // gossip sessions are one-shot
    }
    if (req.type == MsgType::MapRpc) {
      if (!handlers_.on_map) return;
      if (!auth_verify(req.body, req.type, handlers_.cluster_key, handlers_.auth_skew_ms,
                       err)) {
        AIOS_LOG_WARN("reject map rpc auth from ", peer_id, ": ", err);
        return;
      }
      Frame reply;
      reply.type = MsgType::ObjectReply;
      reply.body = {{"ok", true}, {"map", handlers_.on_map(req.body)}};
      auth_sign(reply.body, MsgType::ObjectReply, handlers_.cluster_key);
      if (!write_frame(sock, reply, err, ec, idle)) return;
      continue;
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
      if (!write_frame(sock, reply, err, ec, idle)) return;
      continue;
    }
    AIOS_LOG_DEBUG("unsupported inbound type ", static_cast<int>(req.type));
    return;
  }
}

}  // namespace aios
