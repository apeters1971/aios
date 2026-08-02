#include "http/http_auth.hpp"
#include "util/log.hpp"

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
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
#include <vector>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

struct BenchArgs {
  std::string endpoint{"127.0.0.1:7480"};
  std::string cluster_key;
  unsigned threads{0};  // 0 = hardware_concurrency
  std::size_t ops{200};
  std::size_t warmup{10};
  std::string prefix{"bench"};
  std::vector<std::size_t> sizes;  // bytes
  bool do_create{true};
  bool do_update{true};
  bool do_read{true};
  bool cleanup{true};
  bool json{false};
  // Per-PUT layout headers (empty = cluster default).
  std::string layout;  // replica | ec
  int ec_k{0};         // 0 = omit (use server default)
  int ec_m{0};
  std::string ec_codec;
};

void usage() {
  std::cout
      << "usage: aios-bench --cluster-key KEY [options]\n"
      << "\n"
      << "Multithreaded HTTP client benchmark (create / update / read).\n"
      << "\n"
      << "  --endpoint HOST:PORT   HTTP API (default 127.0.0.1:7480)\n"
      << "  --cluster-key KEY      required shared secret\n"
      << "  --threads N            worker threads (default: hardware concurrency)\n"
      << "  --ops N                operations per size per phase (default 200)\n"
      << "  --warmup N             discarded ops per size per phase (default 10)\n"
      << "  --sizes LIST           comma list: 1k,4k,64k,256k,1M,4M,16M (default all)\n"
      << "  --ops-mix LIST         create,update,read (default all three)\n"
      << "  --prefix STR           oid prefix (default bench)\n"
      << "  --layout replica|ec    per-PUT x-aios-layout (default: cluster)\n"
      << "  --ec-k N               x-aios-ec-k when --layout=ec\n"
      << "  --ec-m N               x-aios-ec-m when --layout=ec\n"
      << "  --ec-codec xor|isal    x-aios-ec-codec when --layout=ec\n"
      << "  --no-cleanup          leave objects after the run\n"
      << "  --json                machine-readable summary\n"
      << "\n"
      << "Reports IOPS, bandwidth, and latency p50/p95/p99 per size and op.\n";
}

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

bool parse_args(int argc, char** argv, BenchArgs& a) {
  a.sizes = {1024, 4096, 65536, 262144, 1048576, 4194304, 16777216};
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      usage();
      std::exit(0);
    }
    if (arg == "--endpoint") {
      const char* v = need("--endpoint");
      if (!v) return false;
      a.endpoint = v;
      continue;
    }
    if (arg == "--cluster-key") {
      const char* v = need("--cluster-key");
      if (!v) return false;
      a.cluster_key = v;
      continue;
    }
    if (arg == "--threads") {
      const char* v = need("--threads");
      if (!v) return false;
      a.threads = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
      continue;
    }
    if (arg == "--ops") {
      const char* v = need("--ops");
      if (!v) return false;
      a.ops = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
      continue;
    }
    if (arg == "--warmup") {
      const char* v = need("--warmup");
      if (!v) return false;
      a.warmup = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
      continue;
    }
    if (arg == "--prefix") {
      const char* v = need("--prefix");
      if (!v) return false;
      a.prefix = v;
      continue;
    }
    if (arg == "--sizes") {
      const char* v = need("--sizes");
      if (!v) return false;
      a.sizes.clear();
      try {
        for (const auto& tok : split_csv(v)) a.sizes.push_back(parse_size(tok));
      } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return false;
      }
      continue;
    }
    if (arg == "--ops-mix") {
      const char* v = need("--ops-mix");
      if (!v) return false;
      a.do_create = a.do_update = a.do_read = false;
      for (const auto& tok : split_csv(v)) {
        if (tok == "create") a.do_create = true;
        else if (tok == "update") a.do_update = true;
        else if (tok == "read") a.do_read = true;
        else {
          std::cerr << "unknown op in --ops-mix: " << tok << "\n";
          return false;
        }
      }
      continue;
    }
    if (arg == "--layout") {
      const char* v = need("--layout");
      if (!v) return false;
      a.layout = v;
      for (char& c : a.layout) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (a.layout != "replica" && a.layout != "ec") {
        std::cerr << "--layout must be replica or ec\n";
        return false;
      }
      continue;
    }
    if (arg == "--ec-k") {
      const char* v = need("--ec-k");
      if (!v) return false;
      a.ec_k = std::stoi(v);
      continue;
    }
    if (arg == "--ec-m") {
      const char* v = need("--ec-m");
      if (!v) return false;
      a.ec_m = std::stoi(v);
      continue;
    }
    if (arg == "--ec-codec") {
      const char* v = need("--ec-codec");
      if (!v) return false;
      a.ec_codec = v;
      continue;
    }
    if (arg == "--no-cleanup") {
      a.cleanup = false;
      continue;
    }
    if (arg == "--json") {
      a.json = true;
      continue;
    }
    std::cerr << "unknown arg: " << arg << "\n";
    return false;
  }
  if (a.cluster_key.empty()) {
    std::cerr << "--cluster-key is required\n";
    return false;
  }
  if (a.sizes.empty()) {
    std::cerr << "no sizes specified\n";
    return false;
  }
  if (!a.do_create && !a.do_update && !a.do_read) {
    std::cerr << "no operations in --ops-mix\n";
    return false;
  }
  if ((a.do_update || a.do_read) && !a.do_create && a.ops > 0) {
    // update/read need objects; allow if user only wants those after prior run with --no-cleanup
  }
  if (a.threads == 0) {
    a.threads = std::max(1u, std::thread::hardware_concurrency());
  }
  return true;
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

std::string url_encode_oid(const std::string& oid) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(oid.size() * 3);
  for (unsigned char c : oid) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xf]);
    }
  }
  return out;
}

struct HttpResp {
  int status{-1};
  std::string body;
  std::string error;
};

class HttpSession {
 public:
  HttpSession(std::string host, std::string port, std::string cluster_key)
      : host_(std::move(host)),
        port_(std::move(port)),
        cluster_key_(std::move(cluster_key)),
        resolver_(ioc_),
        sock_(ioc_) {}

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
    return true;
  }

  void close() {
    boost::system::error_code ec;
    sock_.shutdown(tcp::socket::shutdown_both, ec);
    sock_.close(ec);
  }

  HttpResp request(const std::string& method, const std::string& target,
                   const std::uint8_t* body, std::size_t body_len,
                   const std::unordered_map<std::string, std::string>& extra_headers) {
    HttpResp resp;
    for (int attempt = 0; attempt < 2; ++attempt) {
      std::string err;
      if (!ensure_connected(err)) {
        resp.error = err;
        resp.status = -1;
        close();
        continue;
      }
      resp = do_request(method, target, body, body_len, extra_headers);
      if (resp.status >= 0) return resp;
      close();
    }
    return resp;
  }

 private:
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

  HttpResp do_request(const std::string& method, const std::string& target,
                      const std::uint8_t* body, std::size_t body_len,
                      const std::unordered_map<std::string, std::string>& extra_headers) {
    HttpResp resp;
    std::unordered_map<std::string, std::string> headers = extra_headers;
    headers["content-length"] = std::to_string(body_len);
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
    if (!ec && body_len > 0) {
      asio::write(sock_, asio::buffer(body, body_len), ec);
    }
    if (ec) {
      resp.status = -1;
      resp.error = "write: " + ec.message();
      return resp;
    }

    asio::streambuf buf;
    asio::read_until(sock_, buf, "\r\n\r\n", ec);
    if (ec && ec != asio::error::eof) {
      resp.status = -1;
      resp.error = "read headers: " + ec.message();
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
      }
    }

    resp.body.resize(content_length);
    std::size_t have = buf.size();
    if (have > content_length) have = content_length;
    if (have > 0) {
      is.read(resp.body.data(), static_cast<std::streamsize>(have));
    }
    std::size_t need = content_length - have;
    while (need > 0) {
      const auto n = asio::read(sock_, asio::buffer(resp.body.data() + (content_length - need), need),
                                asio::transfer_at_least(1), ec);
      if (ec) {
        resp.status = -1;
        resp.error = "read body: " + ec.message();
        return resp;
      }
      need -= n;
    }

    if (close_conn) close();
    return resp;
  }

  std::string host_;
  std::string port_;
  std::string cluster_key_;
  asio::io_context ioc_;
  tcp::resolver resolver_;
  tcp::socket sock_;
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
                     OpKind op, std::size_t size, std::size_t total_ops, bool measure) {
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

  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  workers.reserve(nthreads);

  for (std::size_t t = 0; t < nthreads; ++t) {
    workers.emplace_back([&, t]() {
      HttpSession sess(host, port, a.cluster_key);
      std::vector<std::uint8_t> buf(size);
      for (std::size_t i = 0; i < size; ++i) {
        buf[i] = static_cast<std::uint8_t>((i + t) & 0xff);
      }

      for (;;) {
        const std::size_t idx = next.fetch_add(1, std::memory_order_relaxed);
        if (idx >= total_ops) break;

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
          resp = sess.request("GET", target, nullptr, 0, {});
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
          success = (resp.status == 200 && resp.body.size() == size);
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

void print_human(const PhaseStats& st) {
  const auto s = summarize(st);
  std::cout << std::left << std::setw(8) << format_size(st.size) << std::setw(8) << op_name(st.op)
            << std::right << std::setw(8) << st.ok << std::setw(6) << st.err << std::setw(10)
            << std::fixed << std::setprecision(1) << s.iops << std::setw(10) << std::setprecision(2)
            << s.mib_s << std::setw(10) << std::setprecision(3) << s.p50 << std::setw(10) << s.p95
            << std::setw(10) << s.p99 << std::setw(10) << s.avg << "\n";
}

void print_json_row(std::ostream& os, const PhaseStats& st, bool first) {
  const auto s = summarize(st);
  if (!first) os << ",\n";
  os << "  {\"size\":" << st.size << ",\"op\":\"" << op_name(st.op) << "\",\"ok\":" << st.ok
     << ",\"err\":" << st.err << ",\"wall_s\":" << st.wall_s << ",\"iops\":" << s.iops
     << ",\"mib_s\":" << s.mib_s << ",\"p50_ms\":" << s.p50 << ",\"p95_ms\":" << s.p95
     << ",\"p99_ms\":" << s.p99 << ",\"avg_ms\":" << s.avg << ",\"bytes\":" << st.bytes << "}";
}

}  // namespace

int main(int argc, char** argv) {
  BenchArgs args;
  if (!parse_args(argc, argv, args)) {
    usage();
    return 2;
  }

  std::string host, port;
  try {
    parse_endpoint(args.endpoint, host, port);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 2;
  }

  // Probe connectivity (404 on missing oid is fine).
  {
    HttpSession probe(host, port, args.cluster_key);
    auto r = probe.request("GET", "/o/" + url_encode_oid(args.prefix + "/probe"), nullptr, 0, {});
    if (r.status < 0) {
      std::cerr << "cannot reach " << args.endpoint << ": " << r.error << "\n";
      return 1;
    }
    probe.close();
  }

  if (!args.json) {
    std::cout << "aios-bench endpoint=" << args.endpoint << " threads=" << args.threads
              << " ops=" << args.ops << " warmup=" << args.warmup << "\n";
    std::cout << std::left << std::setw(8) << "size" << std::setw(8) << "op" << std::right
              << std::setw(8) << "ok" << std::setw(6) << "err" << std::setw(10) << "iops"
              << std::setw(10) << "MiB/s" << std::setw(10) << "p50_ms" << std::setw(10) << "p95_ms"
              << std::setw(10) << "p99_ms" << std::setw(10) << "avg_ms" << "\n";
  }

  std::vector<PhaseStats> results;
  results.reserve(args.sizes.size() * 3);

  for (std::size_t size : args.sizes) {
    // Warmup against real paths, then delete so create measures If-None-Match cleanly.
    if (args.warmup > 0 && (args.do_create || args.do_update || args.do_read)) {
      run_phase(args, host, port, OpKind::Put, size, args.warmup, false);
      if (args.do_update) {
        run_phase(args, host, port, OpKind::Update, size, args.warmup, false);
      }
      if (args.do_read) {
        run_phase(args, host, port, OpKind::Read, size, args.warmup, false);
      }
      run_phase(args, host, port, OpKind::Delete, size, args.warmup, false);
    }

    if (args.do_create) {
      auto st = run_phase(args, host, port, OpKind::Create, size, args.ops, true);
      if (!args.json) print_human(st);
      results.push_back(std::move(st));
    } else if (args.do_update || args.do_read) {
      // Seed objects for update/read-only runs (unconditional PUT).
      run_phase(args, host, port, OpKind::Put, size, args.ops, false);
    }

    if (args.do_update) {
      auto st = run_phase(args, host, port, OpKind::Update, size, args.ops, true);
      if (!args.json) print_human(st);
      results.push_back(std::move(st));
    }

    if (args.do_read) {
      auto st = run_phase(args, host, port, OpKind::Read, size, args.ops, true);
      if (!args.json) print_human(st);
      results.push_back(std::move(st));
    }

    if (args.cleanup) {
      run_phase(args, host, port, OpKind::Delete, size, args.ops, false);
    }
  }

  if (args.json) {
    std::cout << "{\n\"endpoint\":\"" << args.endpoint << "\",\"threads\":" << args.threads
              << ",\"ops\":" << args.ops << ",\"results\":[\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
      print_json_row(std::cout, results[i], i == 0);
    }
    std::cout << "\n]}\n";
  }

  std::size_t total_err = 0;
  for (const auto& r : results) total_err += r.err;
  return total_err > 0 ? 1 : 0;
}
