#include "bench/http_bench.hpp"
#include "client/stl.hpp"
#include "http/http_auth.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

using BenchArgs = aios::HttpBenchConfig;

bool cancelled(const aios::HttpBenchCancel& cancel) { return cancel && cancel(); }

std::unordered_map<std::string, std::string> layout_headers(const BenchArgs& a) {
  std::unordered_map<std::string, std::string> h;
  if (!a.layout.empty()) h["x-aios-layout"] = a.layout;
  if (a.ec_k > 0) h["x-aios-ec-k"] = std::to_string(a.ec_k);
  if (a.ec_m > 0) h["x-aios-ec-m"] = std::to_string(a.ec_m);
  if (!a.ec_codec.empty()) h["x-aios-ec-codec"] = a.ec_codec;
  return h;
}

std::size_t parse_size(const std::string& s) {
  if (s.empty()) throw std::runtime_error("empty size");
  char* end = nullptr;
  const double n = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) throw std::runtime_error("bad size: " + s);
  std::string u = end;
  for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  double mul = 1.0;
  if (u.empty() || u == "b") mul = 1.0;
  else if (u == "k" || u == "kb" || u == "kib") mul = 1024.0;
  else if (u == "m" || u == "mb" || u == "mib") mul = 1024.0 * 1024.0;
  else if (u == "g" || u == "gb" || u == "gib") mul = 1024.0 * 1024.0 * 1024.0;
  else throw std::runtime_error("bad size unit: " + s);
  if (n < 0) throw std::runtime_error("negative size");
  return static_cast<std::size_t>(n * mul + 0.5);
}

std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (c != ' ' && c != '\t') {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string format_size(std::size_t n) {
  const char* suf[] = {"B", "KiB", "MiB", "GiB"};
  double v = static_cast<double>(n);
  int i = 0;
  while (v >= 1024.0 && i < 3) {
    v /= 1024.0;
    ++i;
  }
  std::ostringstream os;
  if (i == 0 || std::fabs(v - std::round(v)) < 1e-9) {
    os << static_cast<long long>(std::llround(v)) << suf[i];
  } else {
    os << std::fixed << std::setprecision(1) << v << suf[i];
  }
  return os.str();
}

void parse_endpoint(const std::string& ep, std::string& host, std::string& port) {
  auto colon = ep.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= ep.size()) {
    throw std::runtime_error("endpoint must be HOST:PORT");
  }
  host = ep.substr(0, colon);
  port = ep.substr(colon + 1);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
}

std::string url_encode_oid(const std::string& oid) { return aios::http_url_encode_oid(oid); }

struct HttpResp {
  int status{-1};
  std::string body;
  std::size_t body_n{0};
  std::string error;
  std::string location;
};

enum class BodyMode { Store, Discard };

bool parse_http_location(const std::string& loc, std::string& host, std::string& port,
                         std::string& path) {
  if (loc.rfind("http://", 0) == 0) {
    auto rest = loc.substr(7);
    auto slash = rest.find('/');
    auto hp = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? std::string("/") : rest.substr(slash);
    auto colon = hp.rfind(':');
    if (colon == std::string::npos) {
      host = hp;
      port = "80";
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

class HttpSession {
 public:
  HttpSession(std::string host, std::string port, std::string cluster_key)
      : host_(std::move(host)),
        port_(std::move(port)),
        bootstrap_host_(host_),
        bootstrap_port_(port_),
        cluster_key_(std::move(cluster_key)),
        resolver_(ioc_),
        sock_(ioc_) {
    allow_peer(host_ + ":" + port_);
  }

  bool ensure_connected(std::string& err) {
    if (sock_.is_open()) return true;
    boost::system::error_code ec;
    auto endpoints = resolver_.resolve(host_, port_, ec);
    if (ec) {
      err = "resolve: " + ec.message();
      return false;
    }
    asio::connect(sock_, endpoints, ec);
    if (ec) {
      err = "connect: " + ec.message();
      close();
      return false;
    }
    // Prefer blocking + SO_*TIMEO so a wedged peer cannot stall forever. Asio may
    // leave the socket non-blocking after some reactor paths; force blocking first.
    sock_.non_blocking(false, ec);
    sock_.set_option(tcp::no_delay(true), ec);
    const int fd = static_cast<int>(sock_.native_handle());
    timeval tv{};
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return true;
  }

  void close() {
    boost::system::error_code ec;
    sock_.shutdown(tcp::socket::shutdown_both, ec);
    sock_.close(ec);
  }

  HttpResp request(const std::string& method, const std::string& target,
                   const std::uint8_t* body, std::size_t body_len,
                   const std::unordered_map<std::string, std::string>& extra_headers,
                   BodyMode body_mode = BodyMode::Store) {
    std::string path = target;
    HttpResp resp;
    for (int hop = 0; hop <= 5; ++hop) {
      for (int attempt = 0; attempt < 2; ++attempt) {
        std::string err;
        if (!ensure_connected(err)) {
          resp.error = err;
          resp.status = -1;
          close();
          continue;
        }
        resp = do_request(method, path, body, body_len, extra_headers, body_mode);
        if (resp.status >= 0) break;
        close();
      }
      if (resp.status < 0) return resp;
      if (resp.status != 307 && resp.status != 301 && resp.status != 302) return resp;

      std::string new_host = host_;
      std::string new_port = port_;
      std::string new_path;
      if (!parse_http_location(resp.location, new_host, new_port, new_path)) {
        resp.error = "bad redirect Location";
        return resp;
      }
      if (resp.location.rfind("http://", 0) == 0) {
        if (!redirect_allowed(new_host, new_port)) {
          refresh_redirect_allowlist();
          if (!redirect_allowed(new_host, new_port)) {
            resp.error = "redirect target not in cluster";
            return resp;
          }
        }
      }
      // Primary redirects move keep-alive to the new peer for subsequent oids.
      if (new_host != host_ || new_port != port_) {
        close();
        host_ = std::move(new_host);
        port_ = std::move(new_port);
      }
      path = std::move(new_path);
    }
    resp.error = "too many redirects";
    resp.status = -1;
    return resp;
  }

 private:
  static std::string norm_peer(std::string host, std::string port) {
    for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (port.empty()) port = "80";
    return host + ":" + port;
  }

  void allow_peer(const std::string& http_addr) {
    if (http_addr.empty()) return;
    auto colon = http_addr.rfind(':');
    if (colon == std::string::npos) {
      redirect_allow_.insert(norm_peer(http_addr, "80"));
      return;
    }
    redirect_allow_.insert(
        norm_peer(http_addr.substr(0, colon), http_addr.substr(colon + 1)));
  }

  bool redirect_allowed(const std::string& host, const std::string& port) const {
    return redirect_allow_.count(norm_peer(host, port)) > 0;
  }

  void refresh_redirect_allowlist() {
    if (redirect_refreshed_) return;
    redirect_refreshed_ = true;
    // /admin/cluster is only on the admin node; always probe the bootstrap endpoint.
    auto saved_host = host_;
    auto saved_port = port_;
    close();
    host_ = bootstrap_host_;
    port_ = bootstrap_port_;
    std::string err;
    HttpResp probe;
    if (ensure_connected(err)) {
      probe = do_request("GET", "/admin/cluster", nullptr, 0, {});
    }
    host_ = std::move(saved_host);
    port_ = std::move(saved_port);
    close();
    if (probe.status != 200) return;
    try {
      auto j = nlohmann::json::parse(probe.body);
      for (const auto& peer : j.value("admin_peers", nlohmann::json::array())) {
        allow_peer(peer.value("http_addr", ""));
      }
    } catch (...) {
    }
  }

  void add_auth(std::unordered_map<std::string, std::string>& headers, const std::string& method,
                const std::string& target) {
    const std::string date = std::to_string(aios::now_ms());
    headers["x-aios-date"] = date;
    headers["x-aios-content-sha256"] = "UNSIGNED-PAYLOAD";
    const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
    const auto canon =
        aios::http_canonical(method, target, date, signed_headers, headers, "UNSIGNED-PAYLOAD");
    const auto sig = aios::http_sign(cluster_key_, canon);
    headers["authorization"] = "AIOS-HMAC-SHA256 Credential=bench, SignedHeaders=" +
                               signed_headers + ", Signature=" + sig;
  }

  // Avoid uploading large bodies to the wrong replica: wait for 100 Continue (or a
  // final error/redirect) before sending the payload.
  static constexpr std::size_t kExpectContinueBytes = 256u * 1024u;

  HttpResp read_http_message(asio::streambuf& buf, BodyMode body_mode = BodyMode::Store) {
    HttpResp resp;
    boost::system::error_code ec;
    asio::read_until(sock_, buf, "\r\n\r\n", ec);
    if (ec && ec != asio::error::eof) {
      resp.status = -1;
      resp.error = "read headers: " + ec.message();
      close();
      return resp;
    }

    std::istream is(&buf);
    std::string status_line;
    std::getline(is, status_line);
    if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();
    {
      std::istringstream ss(status_line);
      std::string http_ver, reason;
      ss >> http_ver >> resp.status;
      std::getline(ss, reason);
    }

    std::string line;
    std::size_t content_length = 0;
    bool close_conn = false;
    while (std::getline(is, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) break;
      auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      auto name = line.substr(0, colon);
      auto value = line.substr(colon + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
      }
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (name == "content-length") {
        content_length = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
      } else if (name == "connection") {
        for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (value == "close") close_conn = true;
      } else if (name == "location") {
        resp.location = value;
      }
    }

    // 1xx responses have no body.
    if (resp.status >= 100 && resp.status < 200) {
      if (close_conn || ec == asio::error::eof) close();
      return resp;
    }

    resp.body_n = content_length;
    std::size_t have = buf.size();
    if (have > content_length) have = content_length;
    if (body_mode == BodyMode::Discard) {
      char scratch[65536];
      std::size_t from_buf = have;
      while (from_buf > 0) {
        const auto n = std::min(from_buf, sizeof(scratch));
        is.read(scratch, static_cast<std::streamsize>(n));
        from_buf -= n;
      }
      std::size_t need = content_length - have;
      while (need > 0) {
        const auto chunk = std::min(need, sizeof(scratch));
        const auto n = asio::read(sock_, asio::buffer(scratch, chunk), asio::transfer_at_least(1),
                                  ec);
        if (ec) {
          resp.status = -1;
          resp.error = "read body: " + ec.message();
          close();
          return resp;
        }
        need -= n;
      }
    } else {
      resp.body.resize(content_length);
      if (have > 0) {
        is.read(resp.body.data(), static_cast<std::streamsize>(have));
      }
      std::size_t need = content_length - have;
      while (need > 0) {
        const auto n =
            asio::read(sock_, asio::buffer(resp.body.data() + (content_length - need), need),
                       asio::transfer_at_least(1), ec);
        if (ec) {
          resp.status = -1;
          resp.error = "read body: " + ec.message();
          close();
          return resp;
        }
        need -= n;
      }
    }

    if (close_conn || ec == asio::error::eof) close();
    return resp;
  }

  HttpResp do_request(const std::string& method, const std::string& target,
                      const std::uint8_t* body, std::size_t body_len,
                      const std::unordered_map<std::string, std::string>& extra_headers,
                      BodyMode body_mode = BodyMode::Store) {
    HttpResp resp;
    std::unordered_map<std::string, std::string> headers = extra_headers;
    headers.erase("authorization");
    headers.erase("x-aios-date");
    headers["content-length"] = std::to_string(body_len);
    const bool use_continue = body_len > kExpectContinueBytes;
    if (use_continue) headers["Expect"] = "100-continue";
    add_auth(headers, method, target);

    std::ostringstream req;
    req << method << ' ' << target << " HTTP/1.1\r\n";
    req << "Host: " << host_ << ':' << port_ << "\r\n";
    req << "Connection: keep-alive\r\n";
    for (const auto& [k, v] : headers) {
      req << k << ": " << v << "\r\n";
    }
    req << "\r\n";
    const auto head = req.str();

    boost::system::error_code ec;
    asio::write(sock_, asio::buffer(head), ec);
    if (ec) {
      resp.status = -1;
      resp.error = "write: " + ec.message();
      close();
      return resp;
    }

    if (use_continue) {
      asio::streambuf buf;
      resp = read_http_message(buf, BodyMode::Store);
      if (resp.status < 0) return resp;
      if (resp.status != 100) {
        // 307/4xx/5xx before the body — do not upload.
        return resp;
      }
    }

    if (body_len > 0) {
      asio::write(sock_, asio::buffer(body, body_len), ec);
      if (ec) {
        resp.status = -1;
        resp.error = "write body: " + ec.message();
        close();
        return resp;
      }
    }

    asio::streambuf buf;
    return read_http_message(buf, body_mode);
  }

  std::string host_;
  std::string port_;
  std::string bootstrap_host_;
  std::string bootstrap_port_;
  std::string cluster_key_;
  asio::io_context ioc_;
  tcp::resolver resolver_;
  tcp::socket sock_;
  std::unordered_set<std::string> redirect_allow_;
  bool redirect_refreshed_{false};
};

enum class OpKind { Create, Update, Put, Read, Delete };

const char* op_name(OpKind k) {
  switch (k) {
    case OpKind::Create:
      return "create";
    case OpKind::Update:
      return "update";
    case OpKind::Put:
      return "put";
    case OpKind::Read:
      return "read";
    case OpKind::Delete:
      return "delete";
  }
  return "?";
}

struct Sample {
  double ms{0};
  bool ok{false};
};

struct PhaseStats {
  OpKind op{OpKind::Create};
  std::size_t size{0};
  std::string stl_type;  // empty in object mode
  std::string stl_sync;  // sync|async or empty
  std::vector<double> lat_ms;
  std::size_t ok{0};
  std::size_t err{0};
  std::uint64_t bytes{0};
  double wall_s{0};
};

std::string oid_for(const BenchArgs& a, std::size_t size, std::size_t idx) {
  return a.prefix + "/" + std::to_string(size) + "/" + std::to_string(idx);
}

PhaseStats run_phase(const BenchArgs& a, const std::string& host, const std::string& port,
                     OpKind op, std::size_t size, std::size_t total_ops, bool measure,
                     const aios::HttpBenchCancel& cancel) {
  PhaseStats st;
  st.op = op;
  st.size = size;

  const std::size_t nthreads = std::min<std::size_t>(a.threads, std::max<std::size_t>(1, total_ops));
  std::atomic<std::size_t> next{0};
  std::mutex mu;
  std::vector<double> lats;
  lats.reserve(total_ops);
  std::atomic<std::size_t> ok{0};
  std::atomic<std::size_t> err{0};
  std::atomic<std::uint64_t> bytes{0};

  std::vector<std::vector<std::uint8_t>> thread_bufs(nthreads);
  for (std::size_t t = 0; t < nthreads; ++t) {
    thread_bufs[t].resize(size);
    for (std::size_t i = 0; i < size; ++i) {
      thread_bufs[t][i] = static_cast<std::uint8_t>((i + t) & 0xff);
    }
  }

  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  workers.reserve(nthreads);

  for (std::size_t t = 0; t < nthreads; ++t) {
    workers.emplace_back([&, t]() {
      HttpSession sess(host, port, a.cluster_key);
      auto& buf = thread_bufs[t];

      for (;;) {
        const std::size_t idx = next.fetch_add(1, std::memory_order_relaxed);
        if (idx >= total_ops || cancelled(cancel)) break;

        const std::string oid = oid_for(a, size, idx);
        const std::string target = "/o/" + url_encode_oid(oid);
        if (!buf.empty()) {
          buf[0] = static_cast<std::uint8_t>((idx + static_cast<std::size_t>(op)) & 0xff);
        }

        HttpResp resp;
        const auto s0 = std::chrono::steady_clock::now();
        if (op == OpKind::Create) {
          auto h = layout_headers(a);
          h["if-none-match"] = "*";
          resp = sess.request("PUT", target, buf.data(), buf.size(), h);
        } else if (op == OpKind::Update) {
          auto h = layout_headers(a);
          h["if-match"] = "*";
          resp = sess.request("PUT", target, buf.data(), buf.size(), h);
        } else if (op == OpKind::Put) {
          resp = sess.request("PUT", target, buf.data(), buf.size(), layout_headers(a));
        } else if (op == OpKind::Read) {
          resp = sess.request("GET", target, nullptr, 0, {}, BodyMode::Discard);
        } else {
          resp = sess.request("DELETE", target, nullptr, 0, {});
        }
        const auto s1 = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(s1 - s0).count();

        bool success = false;
        if (op == OpKind::Create || op == OpKind::Update || op == OpKind::Put) {
          success = (resp.status == 204);
        } else if (op == OpKind::Read) {
          success = (resp.status == 200 && resp.body_n == size);
        } else {
          success = (resp.status == 204 || resp.status == 404);
        }

        if (success) {
          ok.fetch_add(1, std::memory_order_relaxed);
          if (op == OpKind::Read) {
            bytes.fetch_add(size, std::memory_order_relaxed);
          } else if (op == OpKind::Create || op == OpKind::Update) {
            bytes.fetch_add(size, std::memory_order_relaxed);
          }
        } else {
          err.fetch_add(1, std::memory_order_relaxed);
        }

        if (measure) {
          std::lock_guard<std::mutex> lock(mu);
          lats.push_back(ms);
        }
      }
      sess.close();
    });
  }

  for (auto& w : workers) w.join();
  const auto t1 = std::chrono::steady_clock::now();

  st.ok = ok.load();
  st.err = err.load();
  st.bytes = bytes.load();
  st.wall_s = std::chrono::duration<double>(t1 - t0).count();
  st.lat_ms = std::move(lats);
  return st;
}

struct Summary {
  double p50{0};
  double p95{0};
  double p99{0};
  double avg{0};
  double iops{0};
  double mib_s{0};
};

Summary summarize(const PhaseStats& st) {
  Summary s;
  auto lats = st.lat_ms;
  if (!lats.empty()) {
    std::sort(lats.begin(), lats.end());
    auto pct = [&](double p) {
      const double idx = p * static_cast<double>(lats.size() - 1);
      const std::size_t lo = static_cast<std::size_t>(idx);
      const std::size_t hi = std::min(lo + 1, lats.size() - 1);
      const double frac = idx - static_cast<double>(lo);
      return lats[lo] * (1.0 - frac) + lats[hi] * frac;
    };
    s.p50 = pct(0.50);
    s.p95 = pct(0.95);
    s.p99 = pct(0.99);
    for (double x : lats) s.avg += x;
    s.avg /= static_cast<double>(lats.size());
  }
  s.iops = st.wall_s > 0 ? static_cast<double>(st.ok) / st.wall_s : 0;
  s.mib_s =
      st.wall_s > 0 ? (static_cast<double>(st.bytes) / (1024.0 * 1024.0)) / st.wall_s : 0;
  return s;
}

nlohmann::json phase_json(const PhaseStats& st) {
  const auto s = summarize(st);
  nlohmann::json j{
      {"size", st.size},
      {"size_label", format_size(st.size)},
      {"op", op_name(st.op)},
      {"ok", st.ok},
      {"err", st.err},
      {"wall_s", st.wall_s},
      {"iops", s.iops},
      {"mib_s", s.mib_s},
      {"p50_ms", s.p50},
      {"p95_ms", s.p95},
      {"p99_ms", s.p99},
      {"avg_ms", s.avg},
      {"bytes", st.bytes},
  };
  if (!st.stl_type.empty()) {
    j["stl_type"] = st.stl_type;
    j["stl_sync"] = st.stl_sync;
  }
  return j;
}

std::string stl_name_for(const BenchArgs& a, const std::string& type, const std::string& sync,
                         std::size_t size, std::size_t idx) {
  return a.prefix + "/" + type + "/" + sync + "/" + std::to_string(size) + "/" +
         std::to_string(idx);
}

std::string make_payload(std::size_t n, char fill) {
  return std::string(n, fill);
}

// Populate / mutate / read one STL object. Returns approx logical bytes touched.
std::uint64_t stl_do_op(aios::Session& sess, const std::string& type, aios::sync_mode mode,
                        OpKind op, const std::string& name, std::size_t size) {
  const bool async = (mode == aios::sync_mode::async);
  if (type == "string") {
    aios::string s(sess, name, mode, /*flush_on_destroy=*/false);
    if (op == OpKind::Create || op == OpKind::Put) {
      s.assign(make_payload(size, 'a'));
      if (async) s.flush();
    } else if (op == OpKind::Update) {
      if (async) s.load();
      s.assign(make_payload(size, 'b'));
      if (async) s.flush();
    } else if (op == OpKind::Read) {
      if (async) s.load();
      else
        (void)s.size();
    }
    return size;
  }
  if (type == "map") {
    aios::map m(sess, name, mode, false);
    if (op == OpKind::Create || op == OpKind::Put || op == OpKind::Update) {
      if (op == OpKind::Update && async) m.load();
      if (op == OpKind::Update) m.clear();
      for (std::size_t i = 0; i < size; ++i) {
        m.insert_or_assign("k" + std::to_string(i), "v" + std::to_string(i));
      }
      if (async) m.flush();
    } else if (op == OpKind::Read) {
      if (async) m.load();
      else
        (void)m.size();
    }
    return size;
  }
  if (type == "unordered_map") {
    aios::unordered_map m(sess, name, mode, false);
    if (op == OpKind::Create || op == OpKind::Put || op == OpKind::Update) {
      if (op == OpKind::Update && async) m.load();
      if (op == OpKind::Update) m.clear();
      for (std::size_t i = 0; i < size; ++i) {
        m.insert_or_assign("k" + std::to_string(i), "v" + std::to_string(i));
      }
      if (async) m.flush();
    } else if (op == OpKind::Read) {
      if (async) m.load();
      else
        (void)m.size();
    }
    return size;
  }
  if (type == "set") {
    aios::set s(sess, name, mode, false);
    if (op == OpKind::Create || op == OpKind::Put || op == OpKind::Update) {
      if (op == OpKind::Update && async) s.load();
      if (op == OpKind::Update) s.clear();
      for (std::size_t i = 0; i < size; ++i) s.insert("k" + std::to_string(i));
      if (async) s.flush();
    } else if (op == OpKind::Read) {
      if (async) s.load();
      else
        (void)s.size();
    }
    return size;
  }
  if (type == "list") {
    aios::list l(sess, name, mode, false);
    if (op == OpKind::Create || op == OpKind::Put || op == OpKind::Update) {
      if (op == OpKind::Update && async) l.load();
      if (op == OpKind::Update) l.clear();
      for (std::size_t i = 0; i < size; ++i) l.push_back("v" + std::to_string(i));
      if (async) l.flush();
    } else if (op == OpKind::Read) {
      if (async) l.load();
      else
        (void)l.size();
    }
    return size;
  }
  if (type == "deque") {
    aios::deque d(sess, name, mode, false);
    if (op == OpKind::Create || op == OpKind::Put || op == OpKind::Update) {
      if (op == OpKind::Update && async) d.load();
      if (op == OpKind::Update) d.clear();
      for (std::size_t i = 0; i < size; ++i) d.push_back("v" + std::to_string(i));
      if (async) d.flush();
    } else if (op == OpKind::Read) {
      if (async) d.load();
      else
        (void)d.size();
    }
    return size;
  }
  throw std::runtime_error("unknown stl type: " + type);
}

void stl_delete_one(aios::Session& sess, const std::string& type, const std::string& name) {
  const std::string oid = aios::Session::stl_oid(type, name);
  const std::string path = "/o/" + aios::Session::url_encode_oid(oid);
  try {
    sess.request("DELETE", path);
  } catch (...) {
  }
}

PhaseStats run_stl_phase(const BenchArgs& a, const std::string& type, aios::sync_mode mode,
                         OpKind op, std::size_t size, std::size_t total_ops, bool measure,
                         const aios::HttpBenchCancel& cancel) {
  PhaseStats st;
  st.op = op;
  st.size = size;
  st.stl_type = type;
  st.stl_sync = (mode == aios::sync_mode::sync) ? "sync" : "async";

  const std::size_t nthreads = std::min<std::size_t>(a.threads, std::max<std::size_t>(1, total_ops));
  std::atomic<std::size_t> next{0};
  std::mutex mu;
  std::vector<double> lats;
  lats.reserve(total_ops);
  std::atomic<std::size_t> ok{0};
  std::atomic<std::size_t> err{0};
  std::atomic<std::uint64_t> bytes{0};

  aios::SessionConfig cfg;
  cfg.endpoint = a.endpoint;
  cfg.cluster_key = a.cluster_key;
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  workers.reserve(nthreads);

  for (std::size_t t = 0; t < nthreads; ++t) {
    workers.emplace_back([&, t]() {
      (void)t;
      aios::Session sess(cfg);
      for (;;) {
        const std::size_t idx = next.fetch_add(1, std::memory_order_relaxed);
        if (idx >= total_ops || cancelled(cancel)) break;
        const std::string name = stl_name_for(a, type, st.stl_sync, size, idx);
        const auto s0 = std::chrono::steady_clock::now();
        bool success = false;
        std::uint64_t touched = 0;
        try {
          if (op == OpKind::Delete) {
            stl_delete_one(sess, type, name);
            success = true;
          } else {
            touched = stl_do_op(sess, type, mode, op, name, size);
            success = true;
          }
        } catch (...) {
          success = false;
        }
        const auto s1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(s1 - s0).count();
        if (success) {
          ok.fetch_add(1, std::memory_order_relaxed);
          bytes.fetch_add(touched, std::memory_order_relaxed);
        } else {
          err.fetch_add(1, std::memory_order_relaxed);
        }
        if (measure) {
          std::lock_guard<std::mutex> lock(mu);
          lats.push_back(ms);
        }
      }
    });
  }

  for (auto& w : workers) w.join();
  const auto t1 = std::chrono::steady_clock::now();
  st.ok = ok.load();
  st.err = err.load();
  st.bytes = bytes.load();
  st.wall_s = std::chrono::duration<double>(t1 - t0).count();
  st.lat_ms = std::move(lats);
  return st;
}

// Large objects dominate wall time; keep total transferred bytes roughly flat.
std::size_t scaled_ops(std::size_t base, std::size_t size_bytes) {
  std::size_t div = 1;
  if (size_bytes >= 16ull * 1024 * 1024) {
    div = 16;
  } else if (size_bytes >= 4ull * 1024 * 1024) {
    div = 4;
  }
  return std::max<std::size_t>(1, base / div);
}

void record_phase(std::vector<nlohmann::json>& results, PhaseStats st,
                  const aios::HttpBenchProgress& progress) {
  auto j = phase_json(st);
  results.push_back(j);
  if (progress) progress(j);
}

nlohmann::json run_object_bench(const BenchArgs& args, const std::string& host,
                                const std::string& port, const aios::HttpBenchCancel& cancel,
                                const aios::HttpBenchProgress& progress) {
  nlohmann::json out{{"mode", "object"},
                     {"endpoint", args.endpoint},
                     {"threads", args.threads},
                     {"ops", args.ops},
                     {"results", nlohmann::json::array()},
                     {"cancelled", false}};
  std::vector<nlohmann::json> results;
  results.reserve(args.sizes.size() * 3);

  for (std::size_t size : args.sizes) {
    if (cancelled(cancel)) {
      out["cancelled"] = true;
      break;
    }
    const std::size_t ops = scaled_ops(args.ops, size);
    const std::size_t warmup = args.warmup > 0 ? scaled_ops(args.warmup, size) : 0;
    if (warmup > 0 && (args.do_create || args.do_update || args.do_read)) {
      run_phase(args, host, port, OpKind::Put, size, warmup, false, cancel);
      if (args.do_update) {
        run_phase(args, host, port, OpKind::Update, size, warmup, false, cancel);
      }
      if (args.do_read) {
        run_phase(args, host, port, OpKind::Read, size, warmup, false, cancel);
      }
      run_phase(args, host, port, OpKind::Delete, size, warmup, false, cancel);
    }

    if (args.do_create) {
      record_phase(results, run_phase(args, host, port, OpKind::Create, size, ops, true, cancel),
                   progress);
    } else if (args.do_update || args.do_read) {
      run_phase(args, host, port, OpKind::Put, size, ops, false, cancel);
    }

    if (args.do_update) {
      record_phase(results, run_phase(args, host, port, OpKind::Update, size, ops, true, cancel),
                   progress);
    }

    if (args.do_read) {
      record_phase(results, run_phase(args, host, port, OpKind::Read, size, ops, true, cancel),
                   progress);
    }

    if (args.cleanup) {
      run_phase(args, host, port, OpKind::Delete, size, ops, false, cancel);
    }
  }

  out["results"] = std::move(results);
  if (cancelled(cancel)) out["cancelled"] = true;
  return out;
}

nlohmann::json run_stl_bench(const BenchArgs& args, const aios::HttpBenchCancel& cancel,
                             const aios::HttpBenchProgress& progress) {
  nlohmann::json out{{"mode", "stl"},
                     {"endpoint", args.endpoint},
                     {"threads", args.threads},
                     {"ops", args.ops},
                     {"results", nlohmann::json::array()},
                     {"cancelled", false}};
  std::vector<aios::sync_mode> modes;
  if (args.stl_sync == "sync" || args.stl_sync == "both") modes.push_back(aios::sync_mode::sync);
  if (args.stl_sync == "async" || args.stl_sync == "both") modes.push_back(aios::sync_mode::async);

  std::vector<nlohmann::json> results;

  for (const auto& type : args.stl_types) {
    for (aios::sync_mode mode : modes) {
      for (std::size_t size : args.sizes) {
        if (cancelled(cancel)) {
          out["cancelled"] = true;
          out["results"] = std::move(results);
          return out;
        }
        if (args.warmup > 0 && (args.do_create || args.do_update || args.do_read)) {
          run_stl_phase(args, type, mode, OpKind::Put, size, args.warmup, false, cancel);
          if (args.do_update) {
            run_stl_phase(args, type, mode, OpKind::Update, size, args.warmup, false, cancel);
          }
          if (args.do_read) {
            run_stl_phase(args, type, mode, OpKind::Read, size, args.warmup, false, cancel);
          }
          run_stl_phase(args, type, mode, OpKind::Delete, size, args.warmup, false, cancel);
        }

        if (args.do_create) {
          record_phase(
              results, run_stl_phase(args, type, mode, OpKind::Create, size, args.ops, true, cancel),
              progress);
        } else if (args.do_update || args.do_read) {
          run_stl_phase(args, type, mode, OpKind::Put, size, args.ops, false, cancel);
        }

        if (args.do_update) {
          record_phase(
              results, run_stl_phase(args, type, mode, OpKind::Update, size, args.ops, true, cancel),
              progress);
        }

        if (args.do_read) {
          record_phase(
              results, run_stl_phase(args, type, mode, OpKind::Read, size, args.ops, true, cancel),
              progress);
        }

        if (args.cleanup) {
          run_stl_phase(args, type, mode, OpKind::Delete, size, args.ops, false, cancel);
        }
      }
    }
  }

  out["results"] = std::move(results);
  if (cancelled(cancel)) out["cancelled"] = true;
  return out;
}

bool probe_endpoint(const BenchArgs& args, const std::string& host, const std::string& port,
                    std::string& err) {
  HttpResp r;
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  std::thread th([&] {
    HttpSession probe(host, port, args.cluster_key);
    auto local =
        probe.request("GET", "/o/" + url_encode_oid(args.prefix + "/probe"), nullptr, 0, {});
    probe.close();
    {
      std::lock_guard<std::mutex> lk(mu);
      r = std::move(local);
      done = true;
    }
    cv.notify_one();
  });
  {
    std::unique_lock<std::mutex> lk(mu);
    if (!cv.wait_for(lk, std::chrono::seconds(30), [&] { return done; })) {
      err = "probe timed out";
      th.detach();
      return false;
    }
  }
  th.join();
  if (r.status < 0) {
    err = r.error.empty() ? "timeout or I/O error" : r.error;
    return false;
  }
  return true;
}

void normalize_config(BenchArgs& a) {
  for (char& c : a.mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (char& c : a.stl_sync) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (char& c : a.layout) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (a.stl_types.empty()) {
    a.stl_types = {"string", "map", "unordered_map", "set", "list", "deque"};
  }
  if (a.threads == 0) a.threads = std::max(1u, std::thread::hardware_concurrency());
}

}  // namespace

namespace aios {

void http_bench_apply_cli_defaults(HttpBenchConfig& c) {
  if (c.sizes.empty()) {
    if (c.mode == "stl") {
      c.sizes = {16, 64, 256, 1024, 4096};
    } else {
      c.sizes = {1024, 4096, 65536, 262144, 1048576, 4194304, 16777216};
    }
  }
  normalize_config(c);
}

void http_bench_apply_ui_defaults(HttpBenchConfig& c) {
  if (c.sizes.empty()) {
    if (c.mode == "stl") {
      c.sizes = {16, 64, 256};
    } else {
      c.sizes = {1024, 4096, 65536};
    }
  }
  if (c.ops == 0) c.ops = 50;
  if (c.threads == 0) c.threads = 4;
  if (c.mode == "stl" && c.stl_types.empty()) c.stl_types = {"string", "map"};
  normalize_config(c);
}

std::string http_bench_validate(const HttpBenchConfig& c) {
  if (c.cluster_key.empty()) return "cluster_key required";
  if (c.endpoint.empty()) return "endpoint required";
  if (c.mode != "object" && c.mode != "stl") return "mode must be object or stl";
  if (c.sizes.empty()) return "no sizes specified";
  if (!c.do_create && !c.do_update && !c.do_read) return "no operations in ops_mix";
  if (c.stl_sync != "sync" && c.stl_sync != "async" && c.stl_sync != "both") {
    return "stl_sync must be sync, async, or both";
  }
  if (!c.layout.empty() && c.layout != "replica" && c.layout != "ec") {
    return "layout must be replica or ec";
  }
  static const std::unordered_set<std::string> kTypes = {
      "string", "map", "unordered_map", "set", "list", "deque"};
  for (const auto& t : c.stl_types) {
    if (!kTypes.count(t)) return "unknown stl type: " + t;
  }
  if (c.threads == 0) return "threads must be >= 1";
  return {};
}

std::string http_bench_validate_admin(const HttpBenchConfig& c) {
  auto err = http_bench_validate(c);
  if (!err.empty()) return err;
  if (c.threads > 64) return "threads must be <= 64";
  if (c.ops == 0 || c.ops > 2000) return "ops must be 1..2000";
  if (c.warmup > 200) return "warmup must be <= 200";
  if (c.sizes.size() > 12) return "at most 12 sizes";
  constexpr std::size_t kMax = 16ull * 1024ull * 1024ull;
  for (auto s : c.sizes) {
    if (s == 0 || s > kMax) return "each size must be 1..16MiB";
  }
  if (c.prefix.empty() || c.prefix.find("..") != std::string::npos) return "invalid prefix";
  if (c.prefix != "bench" && c.prefix.rfind("bench/", 0) != 0 && c.prefix.rfind("bench-", 0) != 0) {
    return "prefix must start with bench";
  }
  return {};
}

HttpBenchConfig http_bench_from_json(const nlohmann::json& j) {
  HttpBenchConfig c;
  if (!j.is_object()) return c;
  c.endpoint = j.value("endpoint", c.endpoint);
  c.cluster_key = j.value("cluster_key", c.cluster_key);
  if (j.contains("threads") && j["threads"].is_number_unsigned()) {
    c.threads = j["threads"].get<unsigned>();
  }
  if (j.contains("ops") && j["ops"].is_number_unsigned()) c.ops = j["ops"].get<std::size_t>();
  if (j.contains("warmup") && j["warmup"].is_number_unsigned()) {
    c.warmup = j["warmup"].get<std::size_t>();
  }
  c.prefix = j.value("prefix", c.prefix);
  c.mode = j.value("mode", c.mode);
  c.layout = j.value("layout", c.layout);
  c.ec_codec = j.value("ec_codec", c.ec_codec);
  c.stl_sync = j.value("stl_sync", c.stl_sync);
  if (j.contains("ec_k") && j["ec_k"].is_number_integer()) c.ec_k = j["ec_k"].get<int>();
  if (j.contains("ec_m") && j["ec_m"].is_number_integer()) c.ec_m = j["ec_m"].get<int>();
  if (j.contains("cleanup")) c.cleanup = j["cleanup"].is_boolean() ? j["cleanup"].get<bool>() : true;
  if (j.contains("ops_mix")) {
    c.do_create = c.do_update = c.do_read = false;
    auto add = [&](const std::string& tok) {
      if (tok == "create") c.do_create = true;
      else if (tok == "update") c.do_update = true;
      else if (tok == "read") c.do_read = true;
    };
    if (j["ops_mix"].is_array()) {
      for (const auto& x : j["ops_mix"]) {
        if (x.is_string()) add(x.get<std::string>());
      }
    } else if (j["ops_mix"].is_string()) {
      for (const auto& tok : split_csv(j["ops_mix"].get<std::string>())) add(tok);
    }
  }
  if (j.contains("sizes")) {
    c.sizes.clear();
    if (j["sizes"].is_array()) {
      for (const auto& x : j["sizes"]) {
        if (x.is_number_unsigned()) c.sizes.push_back(x.get<std::size_t>());
        else if (x.is_string()) c.sizes.push_back(parse_size(x.get<std::string>()));
      }
    } else if (j["sizes"].is_string()) {
      for (const auto& tok : split_csv(j["sizes"].get<std::string>())) {
        c.sizes.push_back(parse_size(tok));
      }
    }
  }
  if (j.contains("stl_types")) {
    c.stl_types.clear();
    if (j["stl_types"].is_array()) {
      for (const auto& x : j["stl_types"]) {
        if (x.is_string()) c.stl_types.push_back(x.get<std::string>());
      }
    } else if (j["stl_types"].is_string()) {
      c.stl_types = split_csv(j["stl_types"].get<std::string>());
    }
  }
  return c;
}

nlohmann::json http_bench_config_json(const HttpBenchConfig& c) {
  nlohmann::json mix = nlohmann::json::array();
  if (c.do_create) mix.push_back("create");
  if (c.do_update) mix.push_back("update");
  if (c.do_read) mix.push_back("read");
  nlohmann::json sizes = nlohmann::json::array();
  std::vector<std::string> size_labels;
  for (auto s : c.sizes) {
    sizes.push_back(s);
    size_labels.push_back(format_size(s));
  }
  return {{"endpoint", c.endpoint},
          {"threads", c.threads},
          {"ops", c.ops},
          {"warmup", c.warmup},
          {"prefix", c.prefix},
          {"sizes", sizes},
          {"size_labels", size_labels},
          {"ops_mix", mix},
          {"cleanup", c.cleanup},
          {"layout", c.layout},
          {"ec_k", c.ec_k},
          {"ec_m", c.ec_m},
          {"ec_codec", c.ec_codec},
          {"mode", c.mode},
          {"stl_types", c.stl_types},
          {"stl_sync", c.stl_sync}};
}

nlohmann::json run_http_bench(const HttpBenchConfig& cfg, HttpBenchCancel cancel,
                              HttpBenchProgress progress) {
  HttpBenchConfig args = cfg;
  normalize_config(args);
  const auto verr = http_bench_validate(args);
  if (!verr.empty()) return {{"error", verr}, {"results", nlohmann::json::array()}};

  std::string host, port;
  try {
    parse_endpoint(args.endpoint, host, port);
  } catch (const std::exception& e) {
    return {{"error", e.what()}, {"results", nlohmann::json::array()}};
  }

  std::string perr;
  if (!probe_endpoint(args, host, port, perr)) {
    return {{"error", "cannot reach " + args.endpoint + ": " + perr},
            {"results", nlohmann::json::array()}};
  }

  if (args.mode == "stl") return run_stl_bench(args, cancel, progress);
  return run_object_bench(args, host, port, cancel, progress);
}

HttpBenchJob::HttpBenchJob(std::string default_endpoint, std::string cluster_key)
    : default_endpoint_(std::move(default_endpoint)), cluster_key_(std::move(cluster_key)) {}

HttpBenchJob::~HttpBenchJob() {
  cancel_.store(true);
  join_worker();
}

void HttpBenchJob::join_worker() {
  if (worker_.joinable()) worker_.join();
}

nlohmann::json HttpBenchJob::defaults() const {
  HttpBenchConfig c;
  c.endpoint = default_endpoint_;
  c.ops = 50;
  c.warmup = 5;
  c.stl_sync = "async";
  http_bench_apply_ui_defaults(c);
  return {{"state", "defaults"}, {"config", http_bench_config_json(c)}};
}

nlohmann::json HttpBenchJob::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  nlohmann::json j{{"state", state_},
                   {"started_ms", started_ms_},
                   {"finished_ms", finished_ms_},
                   {"config", http_bench_config_json(cfg_)},
                   {"results", results_},
                   {"last_phase", last_phase_.is_null() ? nlohmann::json{} : last_phase_}};
  if (!error_.empty()) j["error"] = error_;
  return j;
}

nlohmann::json HttpBenchJob::stop() {
  cancel_.store(true);
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ == "running") return {{"ok", true}, {"state", "cancelling"}};
  return {{"ok", true}, {"state", state_}};
}

nlohmann::json HttpBenchJob::start(const nlohmann::json& req) {
  std::lock_guard<std::mutex> start_lk(start_mu_);
  join_worker();
  const auto body = req.is_object() ? req : nlohmann::json::object();
  HttpBenchConfig c = http_bench_from_json(body);
  // The admin console is not allowed to point this node's bench client (which
  // signs with the cluster key) at an arbitrary host: the target is always the
  // local HTTP listener.
  c.endpoint = default_endpoint_;
  c.cluster_key = cluster_key_;
  if (!body.contains("ops")) c.ops = 50;
  if (!body.contains("warmup")) c.warmup = 5;
  if (!body.contains("threads")) c.threads = 0;
  http_bench_apply_ui_defaults(c);
  const auto verr = http_bench_validate_admin(c);
  if (!verr.empty()) return {{"ok", false}, {"error", verr}, {"state", "idle"}};

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ == "running") return {{"ok", false}, {"error", "bench already running"}, {"state", "running"}};
    state_ = "running";
    error_.clear();
    results_ = nlohmann::json::array();
    last_phase_ = nlohmann::json{};
    started_ms_ = now_ms();
    finished_ms_ = 0;
    cfg_ = c;
    cancel_.store(false);
  }

  worker_ = std::thread([this, c] {
    auto cancel = [this] { return cancel_.load(); };
    auto progress = [this](const nlohmann::json& phase) {
      std::lock_guard<std::mutex> lock(mu_);
      results_.push_back(phase);
      last_phase_ = phase;
    };
    nlohmann::json out;
    try {
      out = run_http_bench(c, cancel, progress);
    } catch (const std::exception& e) {
      out = {{"error", e.what()}, {"results", nlohmann::json::array()}};
    }
    std::lock_guard<std::mutex> lock(mu_);
    finished_ms_ = now_ms();
    if (out.contains("results") && out["results"].is_array()) results_ = out["results"];
    if (out.contains("error") && out["error"].is_string()) {
      error_ = out["error"].get<std::string>();
      state_ = "error";
    } else if (out.value("cancelled", false) || cancel_.load()) {
      state_ = "cancelled";
    } else {
      state_ = "done";
    }
  });
  return {{"ok", true}, {"state", "running"}, {"config", http_bench_config_json(c)}};
}

}  // namespace aios
