#include "client/session.hpp"

#include "ec/codec_factory.hpp"
#include "http/http_auth.hpp"
#include "http/tls_stream.hpp"
#include "util/auth.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <sstream>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace aios {
namespace {

using tcp = boost::asio::ip::tcp;

bool parse_location(const std::string& loc, std::string& host, std::string& port,
                    std::string& path) {
  bool abs_tls = false;
  std::string hostpath;
  if (aios::split_endpoint_scheme(loc, abs_tls, hostpath) && hostpath != loc) {
    auto rest = hostpath;
    auto slash = rest.find('/');
    auto hp = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? std::string("/") : rest.substr(slash);
    auto colon = hp.rfind(':');
    if (colon == std::string::npos) {
      host = hp;
      port = abs_tls ? "443" : "80";
    } else {
      host = hp.substr(0, colon);
      port = hp.substr(colon + 1);
    }
    return true;
  }
  if (!loc.empty() && loc.front() == '/') {
    path = loc;
    return true;
  }
  return false;
}

std::string ascii_lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Asio sync read/write treat SO_*TIMEO EAGAIN as "not ready" and poll forever.
// Force a blocking fd and use recv/send so the sockopt is actually observed
// (same approach as HttpServer::sock_read_exact).
void apply_socket_deadlines(tcp::socket& sock, int timeout_ms) {
#ifndef _WIN32
  const int fd = static_cast<int>(sock.native_handle());
  const int fl = ::fcntl(fd, F_GETFL, 0);
  if (fl >= 0 && (fl & O_NONBLOCK)) ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  if (timeout_ms > 0) {
    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
#else
  (void)sock;
  (void)timeout_ms;
#endif
}

[[noreturn]] void throw_sock(const char* what, const boost::system::error_code& ec) {
#ifndef _WIN32
  if (ec.value() == EAGAIN || ec.value() == EWOULDBLOCK || ec.value() == ETIMEDOUT) {
    throw client_error("http", std::string(what) + ": timeout");
  }
#endif
  throw client_error("http", std::string(what) + ": " + ec.message());
}

// Plain or TLS: TlsStream drives the blocking fd with recv/send (or SSL_read /
// SSL_write on top of it), so SO_RCVTIMEO / SO_SNDTIMEO stay observable.
bool timed_write(TlsStream& sock, const void* in, std::size_t n, boost::system::error_code& ec) {
  int err = 0;
  if (sock.write_all(in, n, err)) {
    ec = {};
    return true;
  }
  ec = boost::system::error_code(err ? err : EPIPE, boost::system::system_category());
  return false;
}

bool timed_read_some(TlsStream& sock, void* out, std::size_t n, std::size_t& got,
                     boost::system::error_code& ec) {
  got = 0;
  int err = 0;
  const long r = sock.read_some(out, n, err);
  if (r > 0) {
    got = static_cast<std::size_t>(r);
    ec = {};
    return true;
  }
  if (r == 0) {
    ec = boost::asio::error::eof;
    return false;
  }
  if (err == EINTR) {
    ec = {};
    return true;
  }
  ec = boost::system::error_code(err, boost::system::system_category());
  return false;
}

constexpr std::size_t kMaxHeaderBytes = 64u * 1024u;

bool timed_read_until(TlsStream& sock, std::string& acc, const char* delim,
                      boost::system::error_code& ec) {
  const std::size_t delim_len = std::char_traits<char>::length(delim);
  char tmp[4096];
  while (acc.size() < delim_len || acc.find(delim) == std::string::npos) {
    if (acc.size() > kMaxHeaderBytes) {
      ec = boost::asio::error::message_size;
      return false;
    }
    std::size_t got = 0;
    if (!timed_read_some(sock, tmp, sizeof(tmp), got, ec)) return false;
    if (got > 0) acc.append(tmp, tmp + got);
  }
  ec = {};
  return true;
}

void throw_http(const HttpResponse& resp, const std::string& what) {
  std::string code = "http";
  std::string msg = what + " status=" + std::to_string(resp.status);
  try {
    if (!resp.body.empty()) {
      auto j = nlohmann::json::parse(resp.body);
      if (j.contains("code")) code = j["code"].get<std::string>();
      if (j.contains("error")) msg = j["error"].get<std::string>();
    }
  } catch (...) {
  }
  if (resp.status == 412 || code == "precondition_failed") code = "conflict";
  if (resp.status == 409 && code == "lock_held") code = "lock_held";
  if (resp.status == 404) code = "not_found";
  throw client_error(code, msg);
}

// Fills CAS headers for aios.posix.cas. Returns new cas (expected+1), or 0 if unused.
std::uint64_t apply_posix_cas_headers(Session& session, const std::string& oid,
                                      std::optional<std::uint64_t> expected_cas,
                                      std::unordered_map<std::string, std::string>& headers) {
  if (!expected_cas.has_value()) return 0;
  const std::uint64_t new_cas = *expected_cas + 1;
  headers["x-aios-attr-aios.posix.cas"] = std::to_string(new_cas);
  if (*expected_cas == 0) {
    auto head = session.head_object(oid);
    if (!head.exists) {
      headers["if-none-match"] = "*";
    } else {
      auto it = head.attrs.find("aios.posix.cas");
      if (it == head.attrs.end()) {
        headers["if-match"] = "*";
        headers["x-aios-if-attr-absent"] = "aios.posix.cas";
      } else {
        throw client_error("conflict", "posix cas mismatch (expected 0)");
      }
    }
  } else {
    headers["if-match"] = "*";
    headers["x-aios-if-attr-eq"] = "aios.posix.cas=" + std::to_string(*expected_cas);
  }
  return new_cas;
}

// What the transport saw before an exchange failed. A pooled connection the
// server closed while idle fails with a peer-closed error and no response bytes;
// anything else (timeout, partial response) means the request may have run.
struct ExchangeIo {
  std::size_t received{0};
  bool peer_closed{false};
};

bool is_peer_closed(const boost::system::error_code& ec) {
  if (ec == boost::asio::error::eof) return true;
#ifndef _WIN32
  return ec.value() == ECONNRESET || ec.value() == EPIPE;
#else
  return ec == boost::asio::error::connection_reset;
#endif
}

HttpResponse exchange_http(TlsStream& sock, std::string& leftover, const std::string& method,
                           const std::string& path, const std::string& host,
                           const std::string& port,
                           const std::unordered_map<std::string, std::string>& headers,
                           const std::string& body, bool* close_out, ExchangeIo* io = nullptr) {
  ExchangeIo local_io;
  ExchangeIo& st = io ? *io : local_io;
  st = {};
  std::ostringstream req;
  req << method << ' ' << path << " HTTP/1.1\r\n";
  req << "Host: " << host << ':' << port << "\r\n";
  req << "Connection: keep-alive\r\n";
  for (const auto& [k, v] : headers) req << k << ": " << v << "\r\n";
  req << "\r\n";
  const auto head = req.str();
  boost::system::error_code ec;
  if (!timed_write(sock, head.data(), head.size(), ec)) {
    st.peer_closed = is_peer_closed(ec);
    throw_sock("write", ec);
  }
  if (!body.empty() && !timed_write(sock, body.data(), body.size(), ec)) {
    st.peer_closed = is_peer_closed(ec);
    throw_sock("write", ec);
  }

  std::string wire = std::move(leftover);
  leftover.clear();
  st.received = wire.size();
  if (wire.find("\r\n\r\n") == std::string::npos) {
    if (!timed_read_until(sock, wire, "\r\n\r\n", ec)) {
      st.received = wire.size();
      st.peer_closed = is_peer_closed(ec);
      throw_sock("read headers", ec);
    }
  }
  st.received = wire.size();
  const auto hdr_end = wire.find("\r\n\r\n");
  std::istringstream is(wire.substr(0, hdr_end + 4));
  std::string status_line;
  std::getline(is, status_line);
  HttpResponse resp;
  {
    std::istringstream ss(status_line);
    std::string ver, reason;
    ss >> ver >> resp.status;
  }
  std::string line;
  std::size_t content_length = 0;
  bool have_content_length = false;
  bool close_conn = false;
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto name = line.substr(0, colon);
    auto value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    resp.headers[name] = value;
    if (name == "content-length") {
      have_content_length = true;
      if (value.empty() || value.front() == '-') {
        throw client_error("http", "invalid Content-Length");
      }
      try {
        const auto n = std::stoull(value);
        if (n > Session::kMaxBodyBytes) {
          throw client_error("payload_too_large", "response body exceeds 16 MiB");
        }
        content_length = static_cast<std::size_t>(n);
      } catch (const client_error&) {
        throw;
      } catch (...) {
        throw client_error("http", "invalid Content-Length");
      }
    } else if (name == "connection") {
      for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (value == "close") close_conn = true;
    }
  }
  resp.body = hdr_end == std::string::npos ? std::string{} : wire.substr(hdr_end + 4);
  const bool informational = resp.status >= 100 && resp.status < 200;
  const bool no_entity =
      method == "HEAD" || resp.status == 204 || resp.status == 304 || informational;
  if (no_entity) {
    // HEAD 2xx advertises the entity size but sends no body. HEAD 4xx/5xx from
    // this server still writes a JSON error body — drain that so keep-alive stays aligned.
    if (method == "HEAD" && have_content_length && resp.status >= 400 &&
        content_length <= kMaxHeaderBytes) {
      while (resp.body.size() < content_length) {
        char tmp[4096];
        std::size_t n = 0;
        if (!timed_read_some(sock, tmp, sizeof(tmp), n, ec)) {
          if (n > 0) resp.body.append(tmp, tmp + n);
          if (ec == boost::asio::error::eof) break;
          throw_sock("read body", ec);
        }
        if (n > 0) resp.body.append(tmp, tmp + n);
      }
    }
    leftover.clear();
    resp.body.clear();
  } else {
    while (resp.body.size() < content_length) {
      char tmp[4096];
      std::size_t n = 0;
      if (!timed_read_some(sock, tmp, sizeof(tmp), n, ec)) {
        if (n > 0) resp.body.append(tmp, tmp + n);
        if (ec == boost::asio::error::eof) break;
        throw_sock("read body", ec);
      }
      if (n > 0) resp.body.append(tmp, tmp + n);
    }
    if (resp.body.size() > content_length) {
      leftover = resp.body.substr(content_length);
      resp.body.resize(content_length);
    }
    if (have_content_length && resp.body.size() != content_length) {
      throw client_error("http", "short response body");
    }
  }
  if (close_out) *close_out = close_conn;
  return resp;
}

}  // namespace

struct Session::ConnPool {
  static constexpr std::size_t kMaxIdle = 8;
  std::mutex mu;
  struct Conn {
    Conn() : sock(ioc) {}
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket sock;
    std::unique_ptr<TlsStream> stream;  // plain or TLS over sock's fd
    std::string host;
    std::string port;
    std::string leftover;
    bool open{false};
  };
  std::vector<std::unique_ptr<Conn>> idle;
  std::shared_ptr<TlsClientContext> tls;  // null => plain http

  std::unique_ptr<Conn> take(const std::string& host, const std::string& port, int timeout_ms,
                             bool* reused) {
    std::unique_ptr<Conn> c;
    {
      std::lock_guard lock(mu);
      for (auto it = idle.begin(); it != idle.end(); ++it) {
        if ((*it)->open && (*it)->host == host && (*it)->port == port) {
          c = std::move(*it);
          idle.erase(it);
          if (reused) *reused = true;
          return c;
        }
      }
    }
    if (reused) *reused = false;
    c = std::make_unique<Conn>();
    // Resolve + connect under one deadline: a black-holed peer must fail in
    // socket_timeout_ms, not hang in a synchronous connect for the TCP SYN timeout.
    boost::asio::ip::tcp::resolver resolver(c->ioc);
    boost::asio::steady_timer deadline(c->ioc);
    boost::system::error_code resolve_ec = boost::asio::error::operation_aborted;
    boost::system::error_code connect_ec = boost::asio::error::operation_aborted;
    bool timed_out = false;
    bool finished = false;
    if (timeout_ms > 0) {
      deadline.expires_after(std::chrono::milliseconds(timeout_ms));
      deadline.async_wait([&](const boost::system::error_code& e) {
        if (e || finished) return;
        timed_out = true;
        resolver.cancel();
        boost::system::error_code ignore;
        c->sock.close(ignore);
      });
    }
    resolver.async_resolve(
        host, port,
        [&](const boost::system::error_code& e, tcp::resolver::results_type results) {
          resolve_ec = e;
          if (e) {
            finished = true;
            deadline.cancel();
            return;
          }
          boost::asio::async_connect(
              c->sock, results, [&](const boost::system::error_code& ce, const tcp::endpoint&) {
                connect_ec = ce;
                finished = true;
                deadline.cancel();
              });
        });
    c->ioc.run();
    c->ioc.restart();
    if (timed_out) throw client_error("http", "connect: timeout");
    if (resolve_ec) throw client_error("http", "resolve: " + resolve_ec.message());
    if (connect_ec) throw client_error("http", "connect: " + connect_ec.message());
    apply_socket_deadlines(c->sock, timeout_ms);
    c->stream = std::make_unique<TlsStream>(static_cast<int>(c->sock.native_handle()), tls, host);
    {
      std::string terr;
      if (!c->stream->connect(terr)) throw client_error("http", "tls: " + terr);
    }
    c->host = host;
    c->port = port;
    c->open = true;
    return c;
  }

  void put(std::unique_ptr<Conn> c, bool reuse) {
    if (!c || !reuse || !c->open) return;
    std::lock_guard lock(mu);
    if (idle.size() >= kMaxIdle) return;
    idle.push_back(std::move(c));
  }
};

Session::Session(SessionConfig cfg) : cfg_(std::move(cfg)), pool_(std::make_unique<ConnPool>()) {
  if (!cfg_.principal.empty()) {
    if (!valid_principal_name(cfg_.principal)) {
      throw client_error("bad_request", "invalid principal name");
    }
    if (cfg_.principal_key.size() != 64) {
      throw client_error("bad_request", "principal_key must be 64 hex characters");
    }
  } else if (cfg_.cluster_key.empty()) {
    throw client_error("bad_request", "cluster_key or principal+principal_key required");
  }
  parse_endpoint();
  if (cfg_.tls) {
    std::string terr;
    pool_->tls = TlsClientContext::create(cfg_.tls_ca, cfg_.tls_insecure, terr);
    if (!pool_->tls) throw client_error("bad_request", "tls: " + terr);
  }
  allow_redirect_peer(host_ + ":" + port_);
  for (const auto& p : cfg_.redirect_peers) allow_redirect_peer(p);
}

Session::~Session() = default;

void Session::parse_endpoint() {
  // "https://host:port" turns TLS on; "http://" is accepted and stripped.
  bool scheme_tls = false;
  std::string hostport;
  if (!split_endpoint_scheme(cfg_.endpoint, scheme_tls, hostport)) {
    throw client_error("bad_request", "endpoint must be [http[s]://]HOST:PORT");
  }
  if (scheme_tls) cfg_.tls = true;
  cfg_.endpoint = hostport;
  auto colon = hostport.rfind(':');
  if (colon == std::string::npos) {
    throw client_error("bad_request", "endpoint must be HOST:PORT");
  }
  host_ = hostport.substr(0, colon);
  port_ = hostport.substr(colon + 1);
}

std::string Session::normalize_host_port(std::string host, std::string port) {
  host = ascii_lower(std::move(host));
  // Strip IPv6 brackets if present: [::1]:7480 → host=[::1] already split by rfind(':')
  // for non-bracket forms; keep host as-is after lowercasing.
  if (port.empty()) port = "80";
  return host + ":" + port;
}

void Session::allow_redirect_peer(const std::string& http_addr) {
  if (http_addr.empty()) return;
  std::string key;
  auto colon = http_addr.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= http_addr.size()) {
    // host only → default HTTP port
    key = normalize_host_port(http_addr, "80");
  } else {
    key = normalize_host_port(http_addr.substr(0, colon), http_addr.substr(colon + 1));
  }
  std::lock_guard lock(allow_mu_);
  redirect_allow_.insert(std::move(key));
}

bool Session::redirect_allowed(const std::string& host, const std::string& port) const {
  const auto key = normalize_host_port(host, port);
  std::lock_guard lock(allow_mu_);
  return redirect_allow_.count(key) > 0;
}

HttpResponse Session::bootstrap_get(const std::string& path) {
  std::unordered_map<std::string, std::string> headers;
  headers["content-length"] = "0";
  ensure_ticket();
  add_auth(headers, "GET", path, {});

  auto hop = [&](bool allow_reuse) -> HttpResponse {
    bool reused = false;
    auto conn = pool_->take(host_, port_, cfg_.socket_timeout_ms, &reused);
    if (!allow_reuse && reused) {
      pool_->put(std::move(conn), false);
      conn = pool_->take(host_, port_, cfg_.socket_timeout_ms, &reused);
    }
    ExchangeIo io;
    try {
      bool close_conn = false;
      auto r = exchange_http(*conn->stream, conn->leftover, "GET", path, host_, port_, headers, {},
                             &close_conn, &io);
      pool_->put(std::move(conn), !close_conn);
      return r;
    } catch (const client_error&) {
      pool_->put(std::move(conn), false);
      if (!reused || !io.peer_closed || io.received != 0) throw;
      bool ignored = false;
      auto fresh = pool_->take(host_, port_, cfg_.socket_timeout_ms, &ignored);
      try {
        bool close_conn = false;
        auto r = exchange_http(*fresh->stream, fresh->leftover, "GET", path, host_, port_, headers, {},
                               &close_conn);
        pool_->put(std::move(fresh), !close_conn);
        return r;
      } catch (...) {
        pool_->put(std::move(fresh), false);
        throw;
      }
    }
  };
  return hop(true);
}

void Session::refresh_redirect_allowlist() {
  if (redirect_refreshed_.load()) return;
  // Exactly one thread performs the one-shot refresh; concurrent callers return
  // and re-check the allowlist (still the bootstrap set until the refresh lands).
  if (refreshing_allowlist_.exchange(true)) return;
  struct Clear {
    std::atomic<bool>& f;
    ~Clear() { f.store(false); }
  } clear{refreshing_allowlist_};
  if (redirect_refreshed_.exchange(true)) return;

  try {
    auto resp = bootstrap_get("/admin/cluster");
    if (resp.status != 200 || resp.body.empty()) return;
    const auto j = nlohmann::json::parse(resp.body);
    if (!j.contains("admin_peers") || !j["admin_peers"].is_array()) return;
    for (const auto& peer : j["admin_peers"]) {
      if (!peer.is_object()) continue;
      allow_redirect_peer(peer.value("http_addr", ""));
    }
  } catch (...) {
    // Keep the bootstrap allowlist; absolute redirects to unknown hosts stay rejected.
  }
}

std::string Session::url_encode_oid(const std::string& oid) {
  return http_url_encode_oid(oid);
}

std::string Session::stl_oid(const std::string& type, const std::string& name) {
  if (name.empty()) throw client_error("bad_request", "empty stl name");
  return "stl/" + type + "/" + name;
}

void Session::validate_header_value(const std::string& value, const char* what) {
  for (char c : value) {
    if (c == '\r' || c == '\n' || c == '\0') {
      throw client_error("bad_request", std::string("invalid characters in ") + what);
    }
  }
}

std::string Session::next_nonce() {
  static const std::uint64_t salt = [] {
    std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
  }();
  static std::atomic<std::uint64_t> seq{0};
  std::ostringstream oss;
  oss << std::hex << salt << '-' << static_cast<unsigned long>(::getpid()) << '-'
      << seq.fetch_add(1, std::memory_order_relaxed);
  return oss.str();
}

void Session::add_auth(std::unordered_map<std::string, std::string>& headers,
                       const std::string& method, const std::string& target,
                       const std::string& body) const {
  const std::string date = std::to_string(now_ms());
  headers["x-aios-date"] = date;
  // The body is fully in memory here, so every request signs its real digest;
  // the server verifies a concrete hash even for bodies it streams to disk.
  const std::string payload_hash = sha256_hex(body);
  headers["x-aios-content-sha256"] = payload_hash;
  // The server's replay cache keys signed mutating requests on
  // (method, target, date, signature); two clients issuing an identical request
  // (e.g. POST .../lock with an empty body) in the same millisecond would
  // collide. A per-request nonce makes every signature unique.
  headers[kHttpNonceHeader] = next_nonce();
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  const auto canon =
      http_canonical(method, target, date, signed_headers, headers, payload_hash);
  std::string credential = "stl";
  std::string key = cfg_.cluster_key;
  if (uses_ticket()) {
    std::lock_guard lock(ticket_mu_);
    credential = ticket_;
    key = session_key_;
  }
  const auto sig = http_sign(key, canon);
  headers["authorization"] = "AIOS-HMAC-SHA256 Credential=" + credential +
                             ", SignedHeaders=" + signed_headers + ", Signature=" + sig;
}

std::int64_t Session::ticket_expires_ms() const {
  std::lock_guard lock(ticket_mu_);
  return ticket_expires_ms_;
}

Session::TicketMaterial Session::ticket_material() const {
  std::lock_guard lock(ticket_mu_);
  return {ticket_, session_key_, ticket_expires_ms_};
}

HttpResponse Session::post_ticket_request(const std::string& body) {
  std::unordered_map<std::string, std::string> headers;
  headers["content-length"] = std::to_string(body.size());
  headers["content-type"] = "application/json";
  // Always a fresh connection to the bootstrap endpoint: no credential is sent,
  // and a stale pooled socket must not turn a grant into a spurious failure.
  bool reused = false;
  auto conn = pool_->take(host_, port_, cfg_.socket_timeout_ms, &reused);
  if (reused) {
    pool_->put(std::move(conn), false);
    conn = pool_->take(host_, port_, cfg_.socket_timeout_ms, &reused);
  }
  try {
    bool close_conn = false;
    auto r = exchange_http(*conn->stream, conn->leftover, "POST", "/auth/ticket", host_, port_,
                           headers, body, &close_conn);
    pool_->put(std::move(conn), !close_conn);
    return r;
  } catch (...) {
    pool_->put(std::move(conn), false);
    throw;
  }
}

void Session::ensure_ticket(bool force) {
  if (!uses_ticket()) return;
  std::lock_guard lock(ticket_mu_);
  const auto now = now_ms();
  if (!force && !ticket_.empty()) {
    // Renew at half-life so a ticket never expires mid-burst.
    const auto half = ticket_issued_ms_ + (ticket_expires_ms_ - ticket_issued_ms_) / 2;
    if (now < half) return;
  }
  const auto req = make_ticket_request(cfg_.principal, cfg_.principal_key, now);
  HttpResponse resp = post_ticket_request(ticket_request_to_json(req).dump());
  if (resp.status != 200) {
    std::string code = "unauthorized";
    std::string msg = "ticket grant failed (HTTP " + std::to_string(resp.status) + ")";
    try {
      auto j = nlohmann::json::parse(resp.body);
      if (j.contains("error") && j["error"].is_string()) msg += ": " + j["error"].get<std::string>();
      if (j.contains("code") && j["code"].is_string()) code = j["code"].get<std::string>();
    } catch (...) {
    }
    if (resp.status == 429) code = "login_throttled";
    throw client_error(code, msg);
  }
  std::optional<TicketReply> reply;
  try {
    reply = ticket_reply_from_json(nlohmann::json::parse(resp.body));
  } catch (...) {
  }
  if (!reply) throw client_error("http", "malformed ticket reply");
  std::string err;
  auto session_key = verify_ticket_reply(*reply, cfg_.principal_key, req.nonce, err);
  if (!session_key) throw client_error("unauthorized", err);
  ticket_ = reply->ticket;
  session_key_ = *session_key;
  ticket_issued_ms_ = now;
  ticket_expires_ms_ = reply->expires_ms;
}

HttpResponse Session::request(const std::string& method, const std::string& target,
                              std::unordered_map<std::string, std::string> headers,
                              const std::string& body, int max_redirects) {
  return request_peer({}, method, target, std::move(headers), body, max_redirects);
}

HttpResponse Session::request_peer(const std::string& http_addr, const std::string& method,
                                   const std::string& target,
                                   std::unordered_map<std::string, std::string> headers,
                                   const std::string& body, int max_redirects) {
  if (body.size() > kMaxBodyBytes) {
    throw client_error("payload_too_large", "request body exceeds 16 MiB");
  }
  for (const auto& [k, v] : headers) {
    validate_header_value(k, "header name");
    validate_header_value(v, "header value");
  }
  if (!cfg_.app_label.empty()) validate_header_value(cfg_.app_label, "app label");

  std::string host = host_;
  std::string port = port_;
  if (!http_addr.empty()) {
    allow_redirect_peer(http_addr);
    auto colon = http_addr.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= http_addr.size()) {
      throw client_error("bad_request", "peer address must be HOST:PORT");
    }
    host = http_addr.substr(0, colon);
    port = http_addr.substr(colon + 1);
  }
  std::string path = target;
  HttpResponse resp;
  bool ticket_renewed = false;
  int transition_waited_ms = 0;
  constexpr int kTransitionWaitStepMs = 200;
  const int kTransitionWaitMaxMs = std::max(0, cfg_.map_transition_wait_ms);

  ensure_ticket();
  for (int hop = 0; hop <= max_redirects; ++hop) {
    headers.erase("authorization");
    headers.erase("x-aios-date");
    headers["content-length"] = std::to_string(body.size());
    if (!cfg_.app_label.empty()) headers["x-aios-app-label"] = cfg_.app_label;
    add_auth(headers, method, path, body);

    // A pooled keep-alive socket may have been closed by the server while idle.
    // Only that exact signature (peer closed, zero response bytes) on a reused
    // connection is replayed, and only for requests whose second execution is
    // harmless: GET/HEAD/DELETE, and PUT (full-object writes are idempotent; a
    // CAS-guarded PUT that already landed fails closed with 412). POST (lock,
    // append, txn) is never replayed — a timeout or a half-answered request is
    // surfaced so the caller does not run a non-idempotent operation twice.
    const bool idempotent =
        method == "GET" || method == "HEAD" || method == "DELETE" || method == "PUT";
    auto try_hop = [&]() -> HttpResponse {
      bool reused = false;
      auto conn = pool_->take(host, port, cfg_.socket_timeout_ms, &reused);
      ExchangeIo io;
      try {
        bool close_conn = false;
        auto r = exchange_http(*conn->stream, conn->leftover, method, path, host, port, headers, body,
                               &close_conn, &io);
        pool_->put(std::move(conn), !close_conn);
        return r;
      } catch (const client_error&) {
        pool_->put(std::move(conn), false);
        if (!reused || !idempotent || !io.peer_closed || io.received != 0) throw;
        bool ignored = false;
        auto fresh = pool_->take(host, port, cfg_.socket_timeout_ms, &ignored);
        // Fresh date/nonce: if the first attempt did land server-side, the
        // replay cache would otherwise reject this one as a duplicate.
        add_auth(headers, method, path, body);
        try {
          bool close_conn = false;
          auto r = exchange_http(*fresh->stream, fresh->leftover, method, path, host, port, headers,
                                 body, &close_conn);
          pool_->put(std::move(fresh), !close_conn);
          return r;
        } catch (...) {
          pool_->put(std::move(fresh), false);
          throw;
        }
      }
    };
    resp = try_hop();

    // A node's clock or a long idle gap can expire the ticket before the
    // half-life renewal ran; fetch a new one and repeat this hop once.
    if (resp.status == 401 && uses_ticket() && !ticket_renewed) {
      std::string code;
      try {
        code = nlohmann::json::parse(resp.body).value("code", "");
      } catch (...) {
      }
      if (code == "ticket_expired" || code == "bad_ticket") {
        ticket_renewed = true;
        ensure_ticket(/*force=*/true);
        --hop;
        continue;
      }
    }

    // A primary in the middle of a cluster-map transition (or without a map lease)
    // answers 503 with a retryable code; the condition clears within a lease
    // period, so wait it out instead of failing the caller.
    if (resp.status == 503 && transition_waited_ms < kTransitionWaitMaxMs) {
      std::string code;
      try {
        code = nlohmann::json::parse(resp.body).value("code", "");
      } catch (...) {
      }
      if (code == "map_transition" || code == "no_map_lease") {
        std::this_thread::sleep_for(std::chrono::milliseconds(kTransitionWaitStepMs));
        transition_waited_ms += kTransitionWaitStepMs;
        --hop;
        continue;
      }
    }

    if (resp.status == 307 || resp.status == 301 || resp.status == 302) {
      const auto loc = header_get(resp.headers, "location");
      std::string new_host = host;
      std::string new_port = port;
      std::string new_path;
      if (!parse_location(loc, new_host, new_port, new_path)) {
        throw client_error("http", "bad redirect Location");
      }
      // Absolute Locations may point anywhere; only follow cluster HTTP peers.
      // Relative Locations keep the current hop host (already connected/trusted path).
      if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) {
        if (!redirect_allowed(new_host, new_port)) {
          refresh_redirect_allowlist();
          if (!redirect_allowed(new_host, new_port)) {
            throw client_error("http", "redirect target not in cluster: " + new_host + ":" +
                                           new_port);
          }
        }
      }
      host = std::move(new_host);
      port = std::move(new_port);
      path = std::move(new_path);
      continue;
    }
    return resp;
  }
  throw client_error("http", "too many redirects");
}

ObjectSnapshot Session::parse_object_meta(const HttpResponse& resp, bool with_body) {
  ObjectSnapshot snap;
  if (resp.status == 404) {
    snap.exists = false;
    return snap;
  }
  if (resp.status != 200 && resp.status != 204 && resp.status != 206) {
    throw_http(resp, "object get/head");
  }
  snap.exists = true;
  const auto ver = header_get(resp.headers, "x-aios-version");
  if (!ver.empty()) {
    try {
      snap.seq = static_cast<std::uint64_t>(std::stoull(ver));
    } catch (...) {
    }
  }
  const auto sz = header_get(resp.headers, "x-aios-size");
  if (!sz.empty()) {
    try {
      snap.size = static_cast<std::uint64_t>(std::stoull(sz));
    } catch (...) {
    }
  }
  for (const auto& [k, v] : resp.headers) {
    if (k.rfind("x-aios-attr-", 0) == 0) {
      snap.attrs[k.substr(12)] = v;
    }
  }
  auto it = snap.attrs.find("aios.stl.cas");
  if (it != snap.attrs.end()) {
    try {
      snap.cas = static_cast<std::uint64_t>(std::stoull(it->second));
    } catch (...) {
    }
  }
  if (with_body) snap.body = resp.body;
  if (snap.size == 0 && with_body) snap.size = snap.body.size();
  return snap;
}

ObjectSnapshot Session::get_object(const std::string& oid) {
  const auto path = "/o/" + url_encode_oid(oid);
  auto resp = request("GET", path);
  if (resp.status == 404) return ObjectSnapshot{};
  return parse_object_meta(resp, true);
}

ObjectSnapshot Session::head_object(const std::string& oid) {
  const auto path = "/o/" + url_encode_oid(oid);
  auto resp = request("HEAD", path);
  if (resp.status == 404) return ObjectSnapshot{};
  return parse_object_meta(resp, false);
}

ObjectSnapshot Session::get_range(const std::string& oid, std::uint64_t start,
                                  std::uint64_t end_inclusive) {
  if (end_inclusive < start) {
    throw client_error("bad_request", "invalid get_range bounds");
  }
  const auto path = "/o/" + url_encode_oid(oid);
  std::unordered_map<std::string, std::string> headers;
  headers["range"] = "bytes=" + std::to_string(start) + "-" + std::to_string(end_inclusive);
  auto resp = request("GET", path, headers);
  if (resp.status == 404) return ObjectSnapshot{};
  if (resp.status == 416) throw client_error("range_unsatisfiable", "get_range unsatisfiable");
  return parse_object_meta(resp, true);
}

std::uint64_t Session::put_bytes(const std::string& oid, const std::string& body,
                                 const std::unordered_map<std::string, std::string>& attrs,
                                 std::optional<std::uint64_t> expected_cas,
                                 const std::optional<std::string>& lock_token,
                                 const PutLayout& layout) {
  if (body.size() > kMaxBodyBytes) {
    throw client_error("payload_too_large", "put_bytes exceeds 16 MiB");
  }
  std::unordered_map<std::string, std::string> headers;
  headers["content-type"] = "application/octet-stream";
  for (const auto& [k, v] : attrs) {
    validate_header_value(k, "attribute name");
    validate_header_value(v, "attribute value");
    headers["x-aios-attr-" + k] = v;
  }
  apply_put_layout_headers(headers, layout);
  const std::uint64_t new_cas = apply_posix_cas_headers(*this, oid, expected_cas, headers);
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }
  if (use_client_io()) {
    return put_bytes_client(oid, body, headers, new_cas);
  }
  const auto path = "/o/" + url_encode_oid(oid);
  auto resp = request("PUT", path, headers, body);
  if (resp.status != 204 && resp.status != 200 && resp.status != 201) {
    throw_http(resp, "put_bytes");
  }
  return new_cas;
}

bool Session::use_client_io() {
  if (cfg_.io_path == "server") return false;
  if (cfg_.io_path == "client") return true;
  {
    std::lock_guard lock(io_mu_);
    if (!cluster_io_path_.empty()) return cluster_io_path_ == "client";
  }
  std::string discovered = "server";
  try {
    auto resp = request("GET", "/map");
    if (resp.status == 200 && !resp.body.empty()) {
      discovered = nlohmann::json::parse(resp.body).value("io_path", "server");
    }
  } catch (...) {
    discovered = "server";
  }
  if (discovered != "client") discovered = "server";
  std::lock_guard lock(io_mu_);
  if (cluster_io_path_.empty()) cluster_io_path_ = discovered;
  return cluster_io_path_ == "client";
}

std::uint64_t Session::put_bytes_client(const std::string& oid, const std::string& body,
                                        std::unordered_map<std::string, std::string> headers,
                                        std::uint64_t new_cas) {
  const auto enc = url_encode_oid(oid);
  const std::uint32_t full_crc =
      crc32c(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
  headers["x-aios-size"] = std::to_string(body.size());
  headers["x-aios-crc32c"] = std::to_string(full_crc);

  auto prep_resp = request("POST", "/o/" + enc + "/prepare", headers, {});
  if (prep_resp.status == 501) {
    throw client_error("not_supported", "cluster io_path is server");
  }
  if (prep_resp.status != 200) throw_http(prep_resp, "client prepare");

  nlohmann::json pj;
  try {
    pj = nlohmann::json::parse(prep_resp.body);
  } catch (...) {
    throw client_error("http", "malformed prepare reply");
  }
  const std::string grant = pj.value("grant", "");
  const std::string layout_kind = pj.value("layout", "replica");
  auto acting = pj.value("acting_set", nlohmann::json::array());
  if (grant.empty() || !acting.is_array() || acting.empty()) {
    throw client_error("http", "prepare reply missing grant/acting_set");
  }

  auto abort_grant = [&] {
    try {
      std::unordered_map<std::string, std::string> ah;
      ah["x-aios-write-grant"] = grant;
      request("POST", "/o/" + enc + "/abort-prepared", ah, {});
    } catch (...) {
    }
  };

  struct Piece {
    int shard{0};
    std::string http_addr;
    std::string payload;
  };
  std::vector<Piece> pieces;
  if (layout_kind == "ec") {
    const int k = pj.value("ec_k", 0);
    const int m = pj.value("ec_m", 0);
    const std::string codec = pj.value("ec_codec", "");
    std::string err;
    auto ec = make_erasure_codec(k, m, codec, err);
    if (!ec) {
      abort_grant();
      throw client_error("bad_request", err);
    }
    std::vector<std::vector<std::uint8_t>> shards;
    if (!ec->encode(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(body.data()),
                                                  body.size()),
                    shards, err)) {
      abort_grant();
      throw client_error("store_error", err);
    }
    if (static_cast<int>(shards.size()) != static_cast<int>(acting.size())) {
      abort_grant();
      throw client_error("http", "ec shard count mismatch");
    }
    for (int i = 0; i < static_cast<int>(shards.size()); ++i) {
      Piece p;
      p.shard = i;
      p.http_addr = acting[static_cast<std::size_t>(i)].value("http_addr", "");
      p.payload.assign(reinterpret_cast<const char*>(shards[static_cast<std::size_t>(i)].data()),
                       shards[static_cast<std::size_t>(i)].size());
      pieces.push_back(std::move(p));
    }
  } else {
    for (int i = 0; i < static_cast<int>(acting.size()); ++i) {
      Piece p;
      p.shard = i;
      p.http_addr = acting[static_cast<std::size_t>(i)].value("http_addr", "");
      p.payload = body;
      pieces.push_back(std::move(p));
    }
  }

  std::atomic<int> fails{0};
  std::string first_err;
  std::mutex err_mu;
  std::vector<std::thread> workers;
  workers.reserve(pieces.size());
  for (const auto& piece : pieces) {
    workers.emplace_back([&, piece] {
      try {
        std::unordered_map<std::string, std::string> ih;
        ih["content-type"] = "application/octet-stream";
        ih["x-aios-write-grant"] = grant;
        ih["x-aios-shard"] = std::to_string(piece.shard);
        const auto crc = crc32c(reinterpret_cast<const std::uint8_t*>(piece.payload.data()),
                                piece.payload.size());
        ih["x-aios-crc32c"] = std::to_string(crc);
        auto resp = request_peer(piece.http_addr, "PUT", "/o/" + enc + "/install", ih,
                                 piece.payload);
        if (resp.status != 204 && resp.status != 200) {
          fails.fetch_add(1);
          std::lock_guard elock(err_mu);
          if (first_err.empty()) first_err = "install shard " + std::to_string(piece.shard);
        }
      } catch (const std::exception& e) {
        fails.fetch_add(1);
        std::lock_guard elock(err_mu);
        if (first_err.empty()) first_err = e.what();
      }
    });
  }
  for (auto& t : workers) t.join();
  if (fails.load() > 0) {
    abort_grant();
    throw client_error("quorum_failed", first_err.empty() ? "client install failed" : first_err);
  }

  std::unordered_map<std::string, std::string> ph;
  ph["x-aios-write-grant"] = grant;
  auto pub = request("POST", "/o/" + enc + "/publish", ph, {});
  if (pub.status != 200 && pub.status != 204) {
    abort_grant();
    throw_http(pub, "client publish");
  }
  return new_cas;
}

void Session::put_range(const std::string& oid, std::uint64_t offset, const std::string& data,
                        const std::optional<std::string>& lock_token) {
  if (data.empty()) return;
  if (data.size() > kMaxBodyBytes) {
    throw client_error("payload_too_large", "put_range exceeds 16 MiB");
  }
  const std::uint64_t end = offset + static_cast<std::uint64_t>(data.size()) - 1;
  std::unordered_map<std::string, std::string> headers;
  headers["content-type"] = "application/octet-stream";
  headers["content-range"] =
      "bytes " + std::to_string(offset) + "-" + std::to_string(end) + "/*";
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }
  const auto path = "/o/" + url_encode_oid(oid);
  auto resp = request("PUT", path, headers, data);
  if (resp.status != 204 && resp.status != 200 && resp.status != 201) {
    throw_http(resp, "put_range");
  }
}

void Session::delete_object(const std::string& oid,
                            const std::optional<std::string>& lock_token) {
  std::unordered_map<std::string, std::string> headers;
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }
  const auto path = "/o/" + url_encode_oid(oid);
  auto resp = request("DELETE", path, headers);
  if (resp.status == 404) return;
  if (resp.status != 204 && resp.status != 200) throw_http(resp, "delete_object");
}

ListResult Session::list_prefix(const std::string& prefix, std::size_t limit,
                                const std::string& cursor) {
  std::string target = "/o?limit=" + std::to_string(limit > 0 ? limit : 256);
  if (!prefix.empty()) target += "&prefix=" + url_encode_oid(prefix);
  if (!cursor.empty()) target += "&cursor=" + url_encode_oid(cursor);
  auto resp = request("GET", target);
  if (resp.status != 200) throw_http(resp, "list_prefix");
  ListResult out;
  try {
    auto j = nlohmann::json::parse(resp.body);
    out.next_cursor = j.value("next_cursor", "");
    if (j.contains("objects") && j["objects"].is_array()) {
      for (const auto& o : j["objects"]) {
        ListObject e;
        e.oid = o.value("oid", "");
        e.size = o.value("size", static_cast<std::uint64_t>(0));
        e.mtime_ms = o.value("mtime_ms", static_cast<std::int64_t>(0));
        if (!e.oid.empty()) out.objects.push_back(std::move(e));
      }
    }
  } catch (const client_error&) {
    throw;
  } catch (...) {
    throw client_error("http", "bad list response");
  }
  return out;
}

std::uint64_t Session::put_object(const std::string& oid, const std::string& body,
                                  const std::string& stl_type, std::uint64_t expected_cas,
                                  const std::optional<std::string>& lock_token, int stl_v) {
  if (body.size() > kMaxBodyBytes) {
    throw client_error("payload_too_large", "stl object exceeds 16 MiB");
  }
  const std::uint64_t new_cas = expected_cas + 1;
  std::unordered_map<std::string, std::string> headers;
  headers["content-type"] = "application/json";
  headers["x-aios-attr-aios.stl.type"] = stl_type;
  headers["x-aios-attr-aios.stl.v"] = std::to_string(stl_v);
  headers["x-aios-attr-aios.stl.cas"] = std::to_string(new_cas);
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }

  if (expected_cas == 0) {
    // Create if absent, or first write when cas attr missing.
    auto head = head_object(oid);
    if (!head.exists) {
      headers["if-none-match"] = "*";
    } else if (head.cas == 0) {
      headers["if-match"] = "*";
      headers["x-aios-if-attr-absent"] = "aios.stl.cas";
    } else {
      throw client_error("conflict", "stl cas mismatch (expected 0)");
    }
  } else {
    headers["if-match"] = "*";
    headers["x-aios-if-attr-eq"] = "aios.stl.cas=" + std::to_string(expected_cas);
  }

  const auto path = "/o/" + url_encode_oid(oid);
  auto resp = request("PUT", path, headers, body);
  if (resp.status != 204 && resp.status != 200 && resp.status != 201) {
    throw_http(resp, "put_object");
  }
  return new_cas;
}

AppendResult Session::append(const std::string& oid, const std::string& data,
                             const std::optional<std::string>& lock_token) {
  if (data.size() > kMaxBodyBytes) {
    throw client_error("payload_too_large", "append exceeds 16 MiB");
  }
  std::unordered_map<std::string, std::string> headers;
  headers["content-type"] = "application/octet-stream";
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }
  const auto path = "/o/" + url_encode_oid(oid) + "/append";
  auto resp = request("POST", path, headers, data);
  if (resp.status != 200) throw_http(resp, "append");
  try {
    auto j = nlohmann::json::parse(resp.body);
    AppendResult ar;
    ar.offset = j.at("offset").get<std::uint64_t>();
    ar.size = j.at("size").get<std::uint64_t>();
    ar.seq = j.value("seq", static_cast<std::uint64_t>(0));
    ar.epoch = j.value("epoch", static_cast<std::uint64_t>(0));
    return ar;
  } catch (const client_error&) {
    throw;
  } catch (...) {
    throw client_error("http", "bad append response");
  }
}

LockResult Session::lock_acquire(const std::string& oid, int ttl_ms) {
  std::unordered_map<std::string, std::string> headers;
  headers["x-aios-lock-ttl-ms"] = std::to_string(ttl_ms);
  const auto path = "/o/" + url_encode_oid(oid) + "/lock";
  auto resp = request("POST", path, headers);
  if (resp.status != 201) throw_http(resp, "lock_acquire");
  try {
    auto j = nlohmann::json::parse(resp.body);
    LockResult out;
    out.token = j.at("token").get<std::string>();
    out.expires_ms = j.value("expires_ms", static_cast<std::int64_t>(0));
    return out;
  } catch (...) {
    throw client_error("http", "bad lock response");
  }
}

bool Session::lock_try_acquire(const std::string& oid, std::string& token_out, int ttl_ms,
                               std::int64_t* expires_ms_out) {
  try {
    const auto lr = lock_acquire(oid, ttl_ms);
    token_out = lr.token;
    if (expires_ms_out) *expires_ms_out = lr.expires_ms;
    return true;
  } catch (const client_error& e) {
    if (e.code() == "lock_held") return false;
    throw;
  }
}

std::int64_t Session::lock_break(const std::string& oid, int grace_ms) {
  std::unordered_map<std::string, std::string> headers;
  headers["x-aios-lock-grace-ms"] = std::to_string(grace_ms);
  const auto path = "/o/" + url_encode_oid(oid) + "/lock/break";
  auto resp = request("POST", path, headers);
  if (resp.status == 404) return 0;
  if (resp.status != 200) throw_http(resp, "lock_break");
  try {
    auto j = nlohmann::json::parse(resp.body);
    return j.value("expires_ms", static_cast<std::int64_t>(0));
  } catch (...) {
    throw client_error("http", "bad lock_break response");
  }
}

bool Session::lock_acquire_wait(const std::string& oid, std::string& token_out, int ttl_ms,
                                int max_wait_ms, std::int64_t* expires_ms_out) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
  bool broke = false;
  int sleep_ms = 2;
  for (;;) {
    if (lock_try_acquire(oid, token_out, ttl_ms, expires_ms_out)) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    // A holder that batches work under the lease releases early once it sees
    // the break; a dead one loses the lease at the grace deadline.
    if (!broke) {
      lock_break(oid);
      broke = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    sleep_ms = std::min(sleep_ms * 2, 200);
  }
}

void Session::lock_renew(const std::string& oid, const std::string& token, int ttl_ms,
                         std::int64_t* expires_ms_out, bool* break_requested_out) {
  std::unordered_map<std::string, std::string> headers;
  headers["x-aios-lock-ttl-ms"] = std::to_string(ttl_ms);
  validate_header_value(token, "lock token");
  headers["x-aios-lock-token"] = token;
  const auto path = "/o/" + url_encode_oid(oid) + "/lock/renew";
  auto resp = request("POST", path, headers);
  if (resp.status != 200) throw_http(resp, "lock_renew");
  if (expires_ms_out) *expires_ms_out = 0;
  if (break_requested_out) *break_requested_out = false;
  if (expires_ms_out || break_requested_out) {
    try {
      auto j = nlohmann::json::parse(resp.body);
      if (expires_ms_out) *expires_ms_out = j.value("expires_ms", static_cast<std::int64_t>(0));
      if (break_requested_out) *break_requested_out = j.value("break_requested", false);
    } catch (...) {
    }
  }
}

void Session::lock_release(const std::string& oid, const std::string& token) {
  std::unordered_map<std::string, std::string> headers;
  headers["x-aios-lock-token"] = token;
  const auto path = "/o/" + url_encode_oid(oid) + "/lock";
  auto resp = request("DELETE", path, headers);
  if (resp.status != 204 && resp.status != 200) throw_http(resp, "lock_release");
}

std::string Session::txn_begin() {
  auto resp = request("POST", "/txn");
  if (resp.status != 201 && resp.status != 200) throw_http(resp, "txn_begin");
  try {
    auto j = nlohmann::json::parse(resp.body);
    auto id = j.value("txn_id", "");
    if (id.empty()) throw client_error("http", "txn_begin missing txn_id");
    return id;
  } catch (const client_error&) {
    throw;
  } catch (...) {
    throw client_error("http", "bad txn_begin response");
  }
}

void Session::txn_prepare_put(const std::string& txn_id, const std::string& oid,
                              const std::string& body,
                              std::optional<std::uint64_t> expected_cas,
                              const std::optional<std::string>& lock_token,
                              const std::unordered_map<std::string, std::string>& attrs) {
  if (txn_id.empty() || oid.empty()) throw client_error("bad_request", "txn_prepare_put args");
  if (body.size() > kMaxBodyBytes) {
    throw client_error("payload_too_large", "txn_prepare_put exceeds 16 MiB");
  }
  std::unordered_map<std::string, std::string> headers;
  headers["content-type"] = "application/octet-stream";
  for (const auto& [k, v] : attrs) {
    validate_header_value(k, "attribute name");
    validate_header_value(v, "attribute value");
    headers["x-aios-attr-" + k] = v;
  }
  apply_posix_cas_headers(*this, oid, expected_cas, headers);
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }
  const auto path = "/txn/" + url_encode_oid(txn_id) + "/o/" + url_encode_oid(oid);
  auto resp = request("PUT", path, headers, body);
  if (resp.status != 200 && resp.status != 201 && resp.status != 204) {
    throw_http(resp, "txn_prepare_put");
  }
}

void Session::txn_prepare_delete(const std::string& txn_id, const std::string& oid,
                                 const std::optional<std::string>& lock_token) {
  if (txn_id.empty() || oid.empty()) {
    throw client_error("bad_request", "txn_prepare_delete args");
  }
  std::unordered_map<std::string, std::string> headers;
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }
  const auto path = "/txn/" + url_encode_oid(txn_id) + "/o/" + url_encode_oid(oid);
  auto resp = request("DELETE", path, headers);
  if (resp.status != 200 && resp.status != 201 && resp.status != 204) {
    throw_http(resp, "txn_prepare_delete");
  }
}

void Session::txn_commit(const std::string& txn_id) {
  if (txn_id.empty()) throw client_error("bad_request", "empty txn_id");
  auto resp = request("POST", "/txn/" + url_encode_oid(txn_id) + "/commit");
  if (resp.status != 200) throw_http(resp, "txn_commit");
}

void Session::txn_abort(const std::string& txn_id) {
  if (txn_id.empty()) throw client_error("bad_request", "empty txn_id");
  auto resp = request("POST", "/txn/" + url_encode_oid(txn_id) + "/abort");
  if (resp.status != 200 && resp.status != 204) throw_http(resp, "txn_abort");
}

}  // namespace aios
