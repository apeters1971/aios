#include "http/http_auth.hpp"
#include "metrics/ops_counters.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kIoChunk = 256u * 1024u;

struct Args {
  std::string endpoint{"127.0.0.1:7480"};
  std::string cluster_key;
  std::string app_label;
  std::string cmd;
  std::vector<std::string> positional;
  std::string out_file;
  std::string prefix;
};

void usage() {
  std::cout
      << "usage: aios --cluster-key KEY [--endpoint HOST:PORT] [--app-label LABEL]\n"
      << "            <cmd> [args]\n"
      << "\n"
      << "Commands:\n"
      << "  put  OID FILE\n"
      << "  get  OID [-o FILE]\n"
      << "  del  OID\n"
      << "  stat OID\n"
      << "  list [--prefix P]\n"
      << "  map\n"
      << "  admin [status|ops|config|cluster|metrics|console|archive|backup|vbd|posix-layout|lifecycle|s3-cred|quota|qos ...]\n"
      << "\n"
      << "Admin commands require the target node to run with admin: true / --admin.\n"
      << "  admin                 interactive console (default)\n"
      << "  admin status|ops|config|cluster|metrics   one-shot\n"
      << "  admin archive show|run|drain|recall OID\n"
      << "  admin backup show|run|snapshot|policy ...\n"
      << "  admin vbd list|delete|backup ...\n"
      << "  admin posix-layout show|set ...\n"
      << "  admin lifecycle show|node|target ...\n"
      << "  admin s3-cred list|create|delete ...\n"
      << "  admin quota show|set|reconcile|project ...\n"
      << "  admin qos show|set|project ...\n"
      << "\n"
      << "Follows HTTP 307 redirects to the primary (Location).\n"
      << "put/get stream file bytes (no full-object client buffer).\n";
}

bool parse_args(int argc, char** argv, Args& a) {
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
    if (arg == "--app-label") {
      const char* v = need("--app-label");
      if (!v) return false;
      a.app_label = v;
      continue;
    }
    if (arg == "-o") {
      const char* v = need("-o");
      if (!v) return false;
      a.out_file = v;
      continue;
    }
    if (arg == "--prefix") {
      const char* v = need("--prefix");
      if (!v) return false;
      a.prefix = v;
      continue;
    }
    if (a.cmd.empty()) {
      a.cmd = arg;
      continue;
    }
    a.positional.push_back(arg);
  }
  if (a.cluster_key.empty()) {
    std::cerr << "--cluster-key is required\n";
    return false;
  }
  if (a.cmd.empty()) {
    std::cerr << "command required\n";
    return false;
  }
  return true;
}

void parse_endpoint(const std::string& ep, std::string& host, std::string& port) {
  auto colon = ep.rfind(':');
  if (colon == std::string::npos) throw std::runtime_error("endpoint must be HOST:PORT");
  host = ep.substr(0, colon);
  port = ep.substr(colon + 1);
}

std::string url_encode_oid(const std::string& oid) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
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

void add_auth(std::unordered_map<std::string, std::string>& headers, const std::string& method,
              const std::string& target, const std::string& cluster_key) {
  const std::string date = std::to_string(aios::now_ms());
  headers["x-aios-date"] = date;
  headers["x-aios-content-sha256"] = "UNSIGNED-PAYLOAD";
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  const auto canon =
      aios::http_canonical(method, target, date, signed_headers, headers, "UNSIGNED-PAYLOAD");
  const auto sig = aios::http_sign(cluster_key, canon);
  headers["authorization"] = "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" +
                             signed_headers + ", Signature=" + sig;
}

// Set by main from --app-label for http_exchange.
std::string g_app_label;

struct HttpResp {
  int status{-1};
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  std::string error;
};

bool parse_location(const std::string& loc, std::string& host, std::string& port,
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

// Stream request body from file (or empty). Stream response to out_stream if non-null,
// else buffer small JSON bodies in resp.body.
HttpResp http_exchange(std::string host, std::string port, const std::string& method,
                       std::string target, std::unordered_map<std::string, std::string> headers,
                       const std::string& body_file, std::ostream* out_stream,
                       const std::string& cluster_key, int max_redirects = 5) {
  HttpResp resp;
  for (int hop = 0; hop <= max_redirects; ++hop) {
    std::uint64_t body_len = 0;
    if (!body_file.empty()) {
      std::error_code ec;
      body_len = fs::file_size(body_file, ec);
      if (ec) {
        resp.error = "stat: " + ec.message();
        return resp;
      }
    }

    headers.erase("authorization");
    headers.erase("x-aios-date");
    headers["content-length"] = std::to_string(body_len);
    if (!g_app_label.empty()) headers["x-aios-app-label"] = g_app_label;
    add_auth(headers, method, target, cluster_key);

    asio::io_context ioc;
    boost::system::error_code ec;
    tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(host, port, ec);
    if (ec) {
      resp.error = "resolve: " + ec.message();
      return resp;
    }
    tcp::socket sock(ioc);
    asio::connect(sock, endpoints, ec);
    if (ec) {
      resp.error = "connect: " + ec.message();
      return resp;
    }

    std::ostringstream req;
    req << method << ' ' << target << " HTTP/1.1\r\n";
    req << "Host: " << host << ':' << port << "\r\n";
    req << "Connection: close\r\n";
    for (const auto& [k, v] : headers) req << k << ": " << v << "\r\n";
    req << "\r\n";
    const auto head = req.str();
    asio::write(sock, asio::buffer(head), ec);
    if (ec) {
      resp.error = "write headers: " + ec.message();
      return resp;
    }

    if (body_len > 0) {
      std::ifstream in(body_file, std::ios::binary);
      if (!in) {
        resp.error = "open body file";
        return resp;
      }
      std::vector<char> buf(kIoChunk);
      std::uint64_t left = body_len;
      while (left > 0 && in) {
        const auto n = static_cast<std::streamsize>(
            std::min<std::uint64_t>(left, static_cast<std::uint64_t>(buf.size())));
        in.read(buf.data(), n);
        const auto got = in.gcount();
        if (got <= 0) break;
        asio::write(sock, asio::buffer(buf.data(), static_cast<std::size_t>(got)), ec);
        if (ec) {
          resp.error = "write body: " + ec.message();
          return resp;
        }
        left -= static_cast<std::uint64_t>(got);
      }
      if (left != 0) {
        resp.error = "short body file";
        return resp;
      }
    }

    asio::streambuf buf;
    asio::read_until(sock, buf, "\r\n\r\n", ec);
    if (ec && ec != asio::error::eof) {
      resp.error = "read headers: " + ec.message();
      return resp;
    }
    std::istream is(&buf);
    std::string status_line;
    std::getline(is, status_line);
    if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();
    {
      std::istringstream ss(status_line);
      std::string ver, reason;
      ss >> ver >> resp.status;
      std::getline(ss, reason);
    }
    resp.headers.clear();
    std::string line;
    std::size_t content_length = 0;
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
        content_length = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
      }
    }

    // Drain / stream body.
    std::size_t have = static_cast<std::size_t>(buf.size());
    if (have > content_length) have = content_length;
    std::string prelude(have, '\0');
    if (have) is.read(prelude.data(), static_cast<std::streamsize>(have));
    std::size_t need = content_length - have;

    const bool stream_out = out_stream && resp.status == 200 && method == "GET";
    if (stream_out) {
      if (have) out_stream->write(prelude.data(), static_cast<std::streamsize>(have));
      std::vector<char> chunk(kIoChunk);
      while (need > 0) {
        const auto n = std::min(need, chunk.size());
        const auto got =
            asio::read(sock, asio::buffer(chunk.data(), n), asio::transfer_exactly(n), ec);
        if (ec) {
          resp.error = "read body: " + ec.message();
          return resp;
        }
        out_stream->write(chunk.data(), static_cast<std::streamsize>(got));
        need -= got;
      }
      resp.body.clear();
    } else {
      resp.body = std::move(prelude);
      resp.body.resize(content_length);
      while (need > 0) {
        const auto n =
            asio::read(sock, asio::buffer(resp.body.data() + (content_length - need), need),
                       asio::transfer_at_least(1), ec);
        if (ec) {
          resp.error = "read body: " + ec.message();
          return resp;
        }
        need -= n;
      }
    }

    if (resp.status == 307 || resp.status == 301 || resp.status == 302) {
      auto it = resp.headers.find("location");
      if (it == resp.headers.end() || hop == max_redirects) return resp;
      std::string new_host = host, new_port = port, new_path;
      if (!parse_location(it->second, new_host, new_port, new_path)) return resp;
      if (it->second.rfind("http://", 0) == 0) {
        host = new_host;
        port = new_port;
      }
      target = new_path;
      continue;
    }
    return resp;
  }
  return resp;
}

HttpResp admin_get(std::string host, std::string port, const std::string& path,
                   const std::string& cluster_key) {
  return http_exchange(std::move(host), std::move(port), "GET", path, {}, {}, nullptr,
                       cluster_key);
}

void print_ops_table(const nlohmann::json& ops, const std::string& label) {
  std::cout << label << "\n";
  if (!ops.is_object()) {
    std::cout << "  (no ops)\n";
    return;
  }
  static const char* keys[] = {
      "http_requests", "put",           "put_range",      "append",         "get",
      "head",          "del",           "list",           "put_bytes",      "get_bytes",
      "append_bytes",  "compress_puts", "compress_skipped","compress_logical_bytes",
      "compress_stored_bytes", "lock_acquire",  "watch",          "pubsub_publish",
      "gossip_rounds", "repair_scanned","repair_repaired","repair_failed", "errors"};
  for (const char* k : keys) {
    if (!ops.contains(k)) continue;
    std::cout << "  " << std::left << std::setw(22) << k << " " << ops[k] << "\n";
  }
}

void print_compression(const nlohmann::json& j) {
  if (!j.contains("compression") || !j["compression"].is_object()) return;
  const auto& c = j["compression"];
  std::cout << "compression:\n";
  std::cout << "  " << std::left << std::setw(22) << "puts" << " " << c.value("puts", 0) << "\n";
  std::cout << "  " << std::left << std::setw(22) << "skipped" << " " << c.value("skipped", 0)
            << "\n";
  std::cout << "  " << std::left << std::setw(22) << "logical_bytes" << " "
            << c.value("logical_bytes", 0) << "\n";
  std::cout << "  " << std::left << std::setw(22) << "stored_bytes" << " "
            << c.value("stored_bytes", 0) << "\n";
  const double ratio = c.value("ratio", 0.0);
  std::cout << "  " << std::left << std::setw(22) << "ratio" << " ";
  if (c.value("stored_bytes", 0ull) > 0)
    std::cout << std::fixed << std::setprecision(2) << ratio << "x\n";
  else
    std::cout << "—\n";
}

int cmd_admin_status(std::string host, std::string port, const std::string& key) {
  auto r = admin_get(std::move(host), std::move(port), "/admin/status", key);
  if (r.status < 0) {
    std::cerr << r.error << "\n";
    return 1;
  }
  if (r.status != 200) {
    std::cerr << "admin status failed status=" << r.status << " " << r.body << "\n";
    return 1;
  }
  try {
    auto j = nlohmann::json::parse(r.body);
    std::cout << "node_id:        " << j.value("node_id", "") << "\n"
              << "listen:         " << j.value("listen", "") << "\n"
              << "http_listen:    " << j.value("http_listen", "") << "\n"
              << "admin:          " << (j.value("admin", false) ? "true" : "false") << "\n"
              << "map_epoch:      " << j.value("map_epoch", 0) << "\n"
              << "map_targets:    " << j.value("map_targets", 0) << "\n"
              << "members:        " << j.value("members", 0)
              << " (alive " << j.value("members_alive", 0) << ")\n";
    if (j.contains("ops")) print_ops_table(j["ops"], "ops:");
  } catch (...) {
    std::cout << r.body << "\n";
  }
  return 0;
}

int cmd_admin_ops(std::string host, std::string port, const std::string& key) {
  auto r = admin_get(std::move(host), std::move(port), "/admin/ops", key);
  if (r.status < 0) {
    std::cerr << r.error << "\n";
    return 1;
  }
  if (r.status != 200) {
    std::cerr << "admin ops failed status=" << r.status << " " << r.body << "\n";
    return 1;
  }
  try {
    auto j = nlohmann::json::parse(r.body);
    print_ops_table(j.value("ops", nlohmann::json::object()),
                    "ops (" + j.value("node_id", std::string("?")) + "):");
    print_compression(j);
    if (j.contains("ops_by_label") && j["ops_by_label"].is_object()) {
      for (auto it = j["ops_by_label"].begin(); it != j["ops_by_label"].end(); ++it) {
        print_ops_table(it.value(), "ops_by_label[" + it.key() + "]:");
      }
    }
  } catch (...) {
    std::cout << r.body << "\n";
  }
  return 0;
}

int cmd_admin_config(std::string host, std::string port, const std::string& key) {
  auto r = admin_get(std::move(host), std::move(port), "/admin/config", key);
  if (r.status < 0) {
    std::cerr << r.error << "\n";
    return 1;
  }
  if (r.status != 200) {
    std::cerr << "admin config failed status=" << r.status << " " << r.body << "\n";
    return 1;
  }
  try {
    auto j = nlohmann::json::parse(r.body);
    std::cout << j.dump(2) << "\n";
    std::cout << "\n(note: runtime config changes require restart; cluster_key redacted)\n";
  } catch (...) {
    std::cout << r.body << "\n";
  }
  return 0;
}

int cmd_admin_metrics(std::string host, std::string port, const std::string& key) {
  auto r = admin_get(std::move(host), std::move(port), "/metrics", key);
  if (r.status < 0) {
    std::cerr << r.error << "\n";
    return 1;
  }
  if (r.status != 200) {
    std::cerr << "metrics failed status=" << r.status << " " << r.body << "\n";
    return 1;
  }
  std::cout << r.body;
  if (!r.body.empty() && r.body.back() != '\n') std::cout << '\n';
  return 0;
}

int cmd_admin_cluster(std::string host, std::string port, const std::string& key) {
  auto r = admin_get(host, port, "/admin/cluster", key);
  if (r.status < 0) {
    std::cerr << r.error << "\n";
    return 1;
  }
  if (r.status != 200) {
    std::cerr << "admin cluster failed status=" << r.status << " " << r.body << "\n";
    return 1;
  }
  aios::OpsCounters total;
  try {
    auto j = nlohmann::json::parse(r.body);
    std::cout << "coordinator: " << j.value("node_id", "") << "\n";
    if (j.contains("status") && j["status"].contains("ops")) {
      aios::OpsCounters local;
      local.load_json(j["status"]["ops"]);
      total.add_from(local);
    }
    const auto peers = j.value("admin_peers", nlohmann::json::array());
    std::cout << "alive http peers: " << peers.size() << "\n";
    for (const auto& p : peers) {
      const std::string nid = p.value("node_id", "");
      const std::string http = p.value("http_addr", "");
      const bool self = p.value("self", false);
      std::cout << "  - " << nid << "  http=" << http << (self ? " (self)" : "") << "\n";
      if (self || http.empty()) continue;
      std::string ph, pp;
      try {
        parse_endpoint(http, ph, pp);
      } catch (...) {
        continue;
      }
      auto pr = admin_get(ph, pp, "/admin/ops", key);
      if (pr.status != 200) {
        std::cout << "      ops: unavailable (status=" << pr.status << ")\n";
        continue;
      }
      try {
        auto oj = nlohmann::json::parse(pr.body);
        aios::OpsCounters local;
        local.load_json(oj.value("ops", nlohmann::json::object()));
        total.add_from(local);
        std::cout << "      put=" << local.put.load() << " get=" << local.get.load()
                  << " http=" << local.http_requests.load() << "\n";
      } catch (...) {
        std::cout << "      ops: bad json\n";
      }
    }
    print_ops_table(total.to_json(), "cluster ops (sum of reachable admin nodes):");
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    std::cout << r.body << "\n";
    return 1;
  }
  return 0;
}

void admin_console_help() {
  std::cout << "commands: status | ops | config | cluster | metrics | archive | backup | "
               "vbd | posix-layout | lifecycle | help | quit\n"
            << "  archive [show|run|drain]\n"
            << "  backup [show|run]\n"
            << "  vbd [list|delete|backup ...]\n"
            << "  posix-layout [show|set ...]\n"
            << "  lifecycle [show|node|target ...]\n";
}

HttpResp admin_exchange(std::string host, std::string port, const std::string& method,
                        const std::string& path, const std::string& json_body,
                        const std::string& cluster_key) {
  std::unordered_map<std::string, std::string> headers;
  std::string tmp;
  if (!json_body.empty()) {
    headers["content-type"] = "application/json";
    tmp = (fs::temp_directory_path() / ("aios-admin-" + std::to_string(::getpid()) + ".json"))
              .string();
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out << json_body;
    }
  }
  auto r = http_exchange(std::move(host), std::move(port), method, path, headers, tmp, nullptr,
                         cluster_key);
  if (!tmp.empty()) {
    std::error_code ec;
    fs::remove(tmp, ec);
  }
  return r;
}

int cmd_admin_archive(std::string host, std::string port, const std::string& key,
                      const std::vector<std::string>& args) {
  // args[0]=="archive"; args[1]=action
  const std::string action = args.size() >= 2 ? args[1] : "show";
  if (action == "show") {
    auto r = admin_get(std::move(host), std::move(port), "/admin/api/archive", key);
    if (r.status != 200) {
      std::cerr << "archive show failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  if (action == "run") {
    auto r = admin_exchange(std::move(host), std::move(port), "POST", "/admin/api/archive/run",
                            "{}", key);
    if (r.status != 200) {
      std::cerr << "archive run failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  if (action == "drain") {
    auto r = admin_exchange(std::move(host), std::move(port), "POST",
                            "/admin/api/archive/drain", "{}", key);
    if (r.status != 200) {
      std::cerr << "archive drain failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  if (action == "recall") {
    if (args.size() < 3) {
      std::cerr << "usage: admin archive recall OID\n";
      return 2;
    }
    nlohmann::json body{{"oid", args[2]}};
    auto r = admin_exchange(std::move(host), std::move(port), "POST",
                            "/admin/api/archive/recall", body.dump(), key);
    if (r.status != 200) {
      std::cerr << "archive recall failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  std::cerr << "usage: admin archive show|run|drain|recall OID\n";
  return 2;
}

int cmd_admin_backup(std::string host, std::string port, const std::string& key,
                     const std::vector<std::string>& args) {
  const std::string action = args.size() >= 2 ? args[1] : "show";
  if (action == "show") {
    auto r = admin_get(std::move(host), std::move(port), "/admin/api/backup", key);
    if (r.status != 200) {
      std::cerr << "backup show failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  if (action == "run") {
    auto r = admin_exchange(std::move(host), std::move(port), "POST", "/admin/api/backup/run",
                            "{}", key);
    if (r.status != 200) {
      std::cerr << "backup run failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  if (action == "snapshot") {
    if (args.size() < 3) {
      std::cerr << "usage: admin backup snapshot posix --volume VOL [--path /subdir]\n"
                << "       admin backup snapshot vbd --pool POOL --name NAME [--dest DEST]\n";
      return 2;
    }
    const std::string kind = args[2];
    nlohmann::json body{{"kind", kind}};
    for (std::size_t i = 3; i < args.size(); ++i) {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for " << args[i] << "\n";
        return 2;
      }
      if (args[i] == "--volume") body["volume"] = args[++i];
      else if (args[i] == "--path") body["path"] = args[++i];
      else if (args[i] == "--pool") body["pool"] = args[++i];
      else if (args[i] == "--name") body["name"] = args[++i];
      else if (args[i] == "--dest") body["dest"] = args[++i];
      else {
        std::cerr << "unknown flag: " << args[i] << "\n";
        return 2;
      }
    }
    if (kind == "posix" && !body.contains("volume")) {
      std::cerr << "backup snapshot posix requires --volume\n";
      return 2;
    }
    if (kind == "vbd" && (!body.contains("pool") || !body.contains("name"))) {
      std::cerr << "backup snapshot vbd requires --pool and --name\n";
      return 2;
    }
    auto r = admin_exchange(std::move(host), std::move(port), "POST",
                            "/admin/api/backup/snapshot", body.dump(), key);
    if (r.status != 200) {
      std::cerr << "backup snapshot failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  if (action == "policy") {
    const std::string sub = args.size() >= 3 ? args[2] : "list";
    if (sub == "list") {
      auto r = admin_get(std::move(host), std::move(port), "/admin/api/backup/policies", key);
      if (r.status != 200) {
        std::cerr << "backup policy list failed status=" << r.status << " " << r.body << "\n";
        return 1;
      }
      std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
      return 0;
    }
    if (sub == "rm" || sub == "delete") {
      if (args.size() < 4) {
        std::cerr << "usage: admin backup policy rm ID\n";
        return 2;
      }
      auto r = admin_exchange(std::move(host), std::move(port), "DELETE",
                              "/admin/api/backup/policies/" + args[3], "", key);
      if (r.status != 200) {
        std::cerr << "backup policy rm failed status=" << r.status << " " << r.body << "\n";
        return 1;
      }
      std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
      return 0;
    }
    if (sub == "set") {
      nlohmann::json body{{"kind", "posix"},
                          {"enabled", true},
                          {"path", "/"},
                          {"schedule", {{"at", "00:00"}, {"tz", "UTC"}}},
                          {"retain", {{"keep_days", 7}, {"keep_monthly", 12}}},
                          {"staging_class", "archive"}};
      for (std::size_t i = 3; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--enable") {
          body["enabled"] = true;
          continue;
        }
        if (a == "--disable") {
          body["enabled"] = false;
          continue;
        }
        if (i + 1 >= args.size()) {
          std::cerr << "missing value for " << a << "\n";
          return 2;
        }
        const std::string v = args[++i];
        if (a == "--id") body["id"] = v;
        else if (a == "--kind") body["kind"] = v;
        else if (a == "--volume") body["volume"] = v;
        else if (a == "--path") body["path"] = v;
        else if (a == "--pool") body["pool"] = v;
        else if (a == "--name") body["name"] = v;
        else if (a == "--at") body["schedule"]["at"] = v;
        else if (a == "--keep-days") body["retain"]["keep_days"] = std::stoi(v);
        else if (a == "--keep-monthly") body["retain"]["keep_monthly"] = std::stoi(v);
        else if (a == "--from") body["from"] = v;
        else if (a == "--staging-class") body["staging_class"] = v;
        else if (a == "--tape-sink") body["tape_sink"] = v;
        else if (a == "--tape-root") body["tape_root"] = v;
        else if (a == "--tape-uri-prefix") body["tape_uri_prefix"] = v;
        else if (a == "--tape-bin") body["tape_bin"] = v;
        else if (a == "--tape-s3-endpoint") body["tape_s3_endpoint"] = v;
        else if (a == "--tape-put-cmd") body["tape_put_cmd"] = v;
        else if (a == "--tape-get-cmd") body["tape_get_cmd"] = v;
        else if (a == "--bag-compression") body["bag_compression"] = v;
        else if (a == "--bag-compression-level") body["bag_compression_level"] = std::stoi(v);
        else if (a == "--bag-encryption") body["bag_encryption"] = v;
        else {
          std::cerr << "unknown flag: " << a << "\n";
          return 2;
        }
      }
      if (body.value("kind", "") == "posix" && body.value("volume", "").empty()) {
        std::cerr << "backup policy set posix requires --volume\n";
        return 2;
      }
      auto r = admin_exchange(std::move(host), std::move(port), "POST",
                              "/admin/api/backup/policies", body.dump(), key);
      if (r.status != 200) {
        std::cerr << "backup policy set failed status=" << r.status << " " << r.body << "\n";
        return 1;
      }
      std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
      return 0;
    }
    std::cerr << "usage: admin backup policy list|set|rm ...\n";
    return 2;
  }
  std::cerr << "usage: admin backup show|run|snapshot|policy ...\n";
  return 2;
}

int cmd_admin_vbd(std::string host, std::string port, const std::string& key,
                  const std::vector<std::string>& args) {
  const std::string action = args.size() >= 2 ? args[1] : "list";
  if (action == "list" || action == "show") {
    auto r = admin_get(std::move(host), std::move(port), "/admin/api/vbd", key);
    if (r.status != 200) {
      std::cerr << "vbd list failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  if (action == "delete" || action == "rm") {
    std::string pool, name;
    for (std::size_t i = 2; i < args.size(); ++i) {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for " << args[i] << "\n";
        return 2;
      }
      if (args[i] == "--pool") pool = args[++i];
      else if (args[i] == "--name") name = args[++i];
      else {
        std::cerr << "unknown flag: " << args[i] << "\n";
        return 2;
      }
    }
    if (pool.empty() || name.empty()) {
      std::cerr << "usage: admin vbd delete --pool POOL --name NAME\n";
      return 2;
    }
    auto r = admin_exchange(std::move(host), std::move(port), "DELETE",
                            "/admin/api/vbd/" + pool + "/" + name, "", key);
    if (r.status != 200) {
      std::cerr << "vbd delete failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  if (action == "backup") {
    std::string pool, name, dest;
    for (std::size_t i = 2; i < args.size(); ++i) {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for " << args[i] << "\n";
        return 2;
      }
      if (args[i] == "--pool") pool = args[++i];
      else if (args[i] == "--name") name = args[++i];
      else if (args[i] == "--dest") dest = args[++i];
      else {
        std::cerr << "unknown flag: " << args[i] << "\n";
        return 2;
      }
    }
    if (pool.empty() || name.empty()) {
      std::cerr << "usage: admin vbd backup --pool POOL --name NAME [--dest DEST]\n";
      return 2;
    }
    nlohmann::json body = nlohmann::json::object();
    if (!dest.empty()) body["dest"] = dest;
    auto r = admin_exchange(std::move(host), std::move(port), "POST",
                            "/admin/api/vbd/" + pool + "/" + name + "/backup", body.dump(), key);
    if (r.status != 200) {
      std::cerr << "vbd backup failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  std::cerr << "usage: admin vbd list|delete|backup ...\n";
  return 2;
}

int cmd_admin_lifecycle(std::string host, std::string port, const std::string& key,
                        const std::vector<std::string>& args) {
  const std::string action = args.size() >= 2 ? args[1] : "show";
  if (action == "show") {
    auto r = admin_get(std::move(host), std::move(port), "/admin/api/lifecycle", key);
    if (r.status != 200) {
      std::cerr << "lifecycle show failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  if (action == "node") {
    if (args.size() < 3) {
      std::cerr << "usage: admin lifecycle node up|drain|off\n";
      return 2;
    }
    nlohmann::json body{{"state", args[2]}};
    auto r = admin_exchange(std::move(host), std::move(port), "PUT",
                            "/admin/api/lifecycle/node", body.dump(), key);
    if (r.status != 200) {
      std::cerr << "lifecycle node failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  if (action == "target") {
    // admin lifecycle target --mount PATH|--aios-path PATH [--state S] [--weight N]
    std::string mount, aios_path, state;
    std::optional<int> weight;
    for (std::size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--mount" && i + 1 < args.size()) {
        mount = args[++i];
      } else if ((args[i] == "--aios-path" || args[i] == "--aios_path") && i + 1 < args.size()) {
        aios_path = args[++i];
      } else if (args[i] == "--state" && i + 1 < args.size()) {
        state = args[++i];
      } else if (args[i] == "--weight" && i + 1 < args.size()) {
        weight = std::stoi(args[++i]);
      } else {
        std::cerr << "usage: admin lifecycle target --mount PATH|--aios-path PATH "
                     "[--state up|drain|off] [--weight N]\n";
        return 2;
      }
    }
    if (mount.empty() && aios_path.empty()) {
      std::cerr << "usage: admin lifecycle target --mount PATH|--aios-path PATH "
                   "[--state up|drain|off] [--weight N]\n";
      return 2;
    }
    if (state.empty() && !weight) {
      std::cerr << "lifecycle target requires --state and/or --weight\n";
      return 2;
    }
    nlohmann::json body;
    if (!mount.empty()) body["mount"] = mount;
    if (!aios_path.empty()) body["aios_path"] = aios_path;
    if (!state.empty()) body["state"] = state;
    if (weight) body["weight"] = *weight;
    auto r = admin_exchange(std::move(host), std::move(port), "PUT",
                            "/admin/api/lifecycle/target", body.dump(), key);
    if (r.status != 200) {
      std::cerr << "lifecycle target failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  std::cerr << "usage: admin lifecycle show|node|target ...\n";
  return 2;
}

int cmd_admin_posix_layout(std::string host, std::string port, const std::string& key,
                           const std::vector<std::string>& args) {
  const std::string action = args.size() >= 2 ? args[1] : "show";
  if (action == "show") {
    auto r = admin_get(std::move(host), std::move(port), "/admin/api/posix-layout", key);
    if (r.status != 200) {
      std::cerr << "posix-layout show failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  if (action == "set") {
    std::string raw;
    for (std::size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--file") {
        if (i + 1 >= args.size()) {
          std::cerr << "usage: admin posix-layout set --file PATH.json\n";
          return 2;
        }
        std::ifstream in(args[++i]);
        if (!in) {
          std::cerr << "cannot read " << args[i] << "\n";
          return 1;
        }
        raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        break;
      }
      if (args[i] == "-") {
        raw.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        break;
      }
      raw = args[i];
      break;
    }
    if (raw.empty()) {
      std::cerr << "usage: admin posix-layout set --file PATH.json\n"
                << "       admin posix-layout set '{\"posix_layout_rules\":[...]}'\n"
                << "       admin posix-layout set -   # JSON on stdin\n";
      return 2;
    }
    auto r = admin_exchange(std::move(host), std::move(port), "PUT", "/admin/api/posix-layout",
                            raw, key);
    if (r.status != 200) {
      std::cerr << "posix-layout set failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << nlohmann::json::parse(r.body).dump(2) << "\n";
    return 0;
  }
  std::cerr << "usage: admin posix-layout show|set ...\n";
  return 2;
}

int cmd_admin_s3_cred(std::string host, std::string port, const std::string& key,
                      const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::cerr << "usage: admin s3-cred list|create|delete ...\n";
    return 2;
  }
  const std::string action = args[1];
  if (action == "list") {
    auto r = admin_get(std::move(host), std::move(port), "/admin/api/s3/credentials", key);
    if (r.status < 0) {
      std::cerr << r.error << "\n";
      return 1;
    }
    if (r.status != 200) {
      std::cerr << "s3-cred list failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    try {
      auto j = nlohmann::json::parse(r.body);
      auto creds = j.value("credentials", nlohmann::json::array());
      if (creds.empty()) {
        std::cout << "(no S3 credentials)\n";
        return 0;
      }
      for (const auto& c : creds) {
        std::cout << c.value("access_key_id", "") << "  uid=" << c.value("uid", 0)
                  << " gid=" << c.value("gid", 0) << "  buckets=";
        if (c.contains("buckets") && c["buckets"].is_array()) {
          bool first = true;
          for (const auto& b : c["buckets"]) {
            if (!first) std::cout << ',';
            first = false;
            std::cout << b.get<std::string>();
          }
        }
        std::cout << "\n";
      }
    } catch (...) {
      std::cout << r.body << "\n";
    }
    return 0;
  }

  std::string id, buckets;
  std::uint32_t uid = 0, gid = 0;
  bool have_uid = false, have_gid = false;
  for (std::size_t i = 2; i < args.size(); ++i) {
    const auto& a = args[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return args[++i].c_str();
    };
    if (a == "--id") {
      const char* v = need("--id");
      if (!v) return 2;
      id = v;
    } else if (a == "--uid") {
      const char* v = need("--uid");
      if (!v) return 2;
      uid = static_cast<std::uint32_t>(std::stoul(v));
      have_uid = true;
    } else if (a == "--gid") {
      const char* v = need("--gid");
      if (!v) return 2;
      gid = static_cast<std::uint32_t>(std::stoul(v));
      have_gid = true;
    } else if (a == "--buckets") {
      const char* v = need("--buckets");
      if (!v) return 2;
      buckets = v;
    } else {
      std::cerr << "unknown flag: " << a << "\n";
      return 2;
    }
  }

  if (action == "create") {
    if (id.empty() || buckets.empty() || !have_uid || !have_gid) {
      std::cerr << "create requires --id --uid --gid --buckets\n";
      return 2;
    }
    nlohmann::json body{{"access_key_id", id}, {"uid", uid}, {"gid", gid}, {"buckets", buckets}};
    auto r = admin_exchange(std::move(host), std::move(port), "POST",
                            "/admin/api/s3/credentials", body.dump(), key);
    if (r.status < 0) {
      std::cerr << r.error << "\n";
      return 1;
    }
    if (r.status != 201 && r.status != 200) {
      std::cerr << "s3-cred create failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    try {
      auto j = nlohmann::json::parse(r.body);
      std::cout << "access_key_id: " << j.value("access_key_id", "") << "\n"
                << "secret:        " << j.value("secret", "") << "\n"
                << "(store the secret now; it is not shown again)\n";
    } catch (...) {
      std::cout << r.body << "\n";
    }
    return 0;
  }

  if (action == "delete") {
    if (id.empty()) {
      std::cerr << "delete requires --id\n";
      return 2;
    }
    auto r = admin_exchange(std::move(host), std::move(port), "DELETE",
                            "/admin/api/s3/credentials/" + url_encode_oid(id), {}, key);
    if (r.status < 0) {
      std::cerr << r.error << "\n";
      return 1;
    }
    if (r.status != 200) {
      std::cerr << "s3-cred delete failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << "deleted " << id << "\n";
    return 0;
  }

  std::cerr << "unknown s3-cred action: " << action << "\n";
  return 2;
}

std::optional<std::uint64_t> parse_bytes_arg(const std::string& s) {
  if (s.empty() || s == "clear" || s == "none" || s == "-") return std::nullopt;
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) return std::nullopt;
  std::uint64_t mul = 1;
  if (*end == 'K' || *end == 'k') mul = 1024ull;
  else if (*end == 'M' || *end == 'm') mul = 1024ull * 1024ull;
  else if (*end == 'G' || *end == 'g') mul = 1024ull * 1024ull * 1024ull;
  else if (*end == 'T' || *end == 't') mul = 1024ull * 1024ull * 1024ull * 1024ull;
  else if (*end != '\0') return std::nullopt;
  return static_cast<std::uint64_t>(v * static_cast<double>(mul));
}

int cmd_admin_quota(std::string host, std::string port, const std::string& key,
                    const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::cerr << "usage: admin quota show|set|reconcile|project ...\n";
    return 2;
  }
  const std::string action = args[1];
  if (action == "show") {
    auto r = admin_get(std::move(host), std::move(port), "/admin/api/quota", key);
    if (r.status != 200) {
      std::cerr << "quota show failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << r.body << "\n";
    return 0;
  }
  if (action == "reconcile") {
    auto r = admin_exchange(std::move(host), std::move(port), "POST",
                            "/admin/api/quota/reconcile", "{}", key);
    if (r.status != 200) {
      std::cerr << "quota reconcile failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << r.body << "\n";
    return 0;
  }
  if (action == "set") {
    std::optional<std::uint32_t> uid, gid;
    std::optional<std::uint64_t> bytes;
    bool clear = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--uid" && i + 1 < args.size()) uid = static_cast<std::uint32_t>(std::stoul(args[++i]));
      else if (args[i] == "--gid" && i + 1 < args.size())
        gid = static_cast<std::uint32_t>(std::stoul(args[++i]));
      else if (args[i] == "--bytes" && i + 1 < args.size()) {
        auto b = parse_bytes_arg(args[++i]);
        if (!b) clear = true;
        else bytes = b;
      } else if (args[i] == "--clear") clear = true;
    }
    if (!uid && !gid) {
      std::cerr << "set requires --uid or --gid\n";
      return 2;
    }
    nlohmann::json body;
    if (uid) body["uid"] = *uid;
    if (gid) body["gid"] = *gid;
    if (clear) body["bytes"] = nullptr;
    else if (bytes) body["bytes"] = *bytes;
    else {
      std::cerr << "set requires --bytes SIZE or --clear\n";
      return 2;
    }
    auto r = admin_exchange(std::move(host), std::move(port), "PUT", "/admin/api/quota/limits",
                            body.dump(), key);
    if (r.status != 200) {
      std::cerr << "quota set failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << "ok\n";
    return 0;
  }
  if (action == "project") {
    if (args.size() < 3) {
      std::cerr << "usage: admin quota project create|delete|set ...\n";
      return 2;
    }
    const std::string sub = args[2];
    if (sub == "create") {
      std::string name;
      std::uint64_t root_ino = 0;
      std::optional<std::uint64_t> bytes;
      for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--name" && i + 1 < args.size()) name = args[++i];
        else if (args[i] == "--root-ino" && i + 1 < args.size())
          root_ino = std::stoull(args[++i]);
        else if (args[i] == "--bytes" && i + 1 < args.size()) bytes = parse_bytes_arg(args[++i]);
      }
      nlohmann::json body{{"name", name}, {"root_ino", root_ino}};
      if (bytes) body["bytes"] = *bytes;
      auto r = admin_exchange(std::move(host), std::move(port), "POST",
                              "/admin/api/quota/projects", body.dump(), key);
      if (r.status != 201 && r.status != 200) {
        std::cerr << "project create failed status=" << r.status << " " << r.body << "\n";
        return 1;
      }
      std::cout << r.body << "\n";
      return 0;
    }
    if (sub == "delete") {
      std::uint32_t id = 0;
      for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--id" && i + 1 < args.size()) id = static_cast<std::uint32_t>(std::stoul(args[++i]));
      }
      auto r = admin_exchange(std::move(host), std::move(port), "DELETE",
                              "/admin/api/quota/projects/" + std::to_string(id), {}, key);
      if (r.status != 200) {
        std::cerr << "project delete failed status=" << r.status << " " << r.body << "\n";
        return 1;
      }
      std::cout << "ok\n";
      return 0;
    }
    if (sub == "set") {
      std::uint32_t id = 0;
      std::optional<std::uint32_t> uid;
      std::optional<std::uint64_t> bytes;
      bool clear = false;
      for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--id" && i + 1 < args.size())
          id = static_cast<std::uint32_t>(std::stoul(args[++i]));
        else if (args[i] == "--uid" && i + 1 < args.size())
          uid = static_cast<std::uint32_t>(std::stoul(args[++i]));
        else if (args[i] == "--bytes" && i + 1 < args.size()) {
          auto b = parse_bytes_arg(args[++i]);
          if (!b) clear = true;
          else bytes = b;
        } else if (args[i] == "--clear")
          clear = true;
      }
      if (id == 0 || !uid) {
        std::cerr << "usage: admin quota project set --id ID --uid UID --bytes SIZE|--clear\n";
        return 2;
      }
      nlohmann::json body{{"uid", *uid}};
      if (clear) body["bytes"] = nullptr;
      else if (bytes) body["bytes"] = *bytes;
      else {
        std::cerr << "project set requires --bytes SIZE or --clear\n";
        return 2;
      }
      auto r = admin_exchange(std::move(host), std::move(port), "PUT",
                              "/admin/api/quota/projects/" + std::to_string(id), body.dump(), key);
      if (r.status != 200) {
        std::cerr << "project set failed status=" << r.status << " " << r.body << "\n";
        return 1;
      }
      std::cout << "ok\n";
      return 0;
    }
  }
  std::cerr << "unknown quota action\n";
  return 2;
}

int cmd_admin_qos(std::string host, std::string port, const std::string& key,
                  const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::cerr << "usage: admin qos show|set|project ...\n";
    return 2;
  }
  const std::string action = args[1];
  if (action == "show") {
    auto r = admin_get(std::move(host), std::move(port), "/admin/api/qos", key);
    if (r.status != 200) {
      std::cerr << "qos show failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << r.body << "\n";
    return 0;
  }
  if (action == "set") {
    std::optional<std::uint32_t> uid, gid;
    std::optional<std::uint64_t> iops, bps;
    bool clear = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--uid" && i + 1 < args.size())
        uid = static_cast<std::uint32_t>(std::stoul(args[++i]));
      else if (args[i] == "--gid" && i + 1 < args.size())
        gid = static_cast<std::uint32_t>(std::stoul(args[++i]));
      else if (args[i] == "--iops" && i + 1 < args.size())
        iops = std::stoull(args[++i]);
      else if (args[i] == "--bps" && i + 1 < args.size())
        bps = parse_bytes_arg(args[++i]);
      else if (args[i] == "--clear")
        clear = true;
    }
    if (!uid && !gid) {
      std::cerr << "set requires --uid or --gid\n";
      return 2;
    }
    if (!clear && !iops && !bps) {
      std::cerr << "set requires --iops and/or --bps, or --clear\n";
      return 2;
    }
    nlohmann::json body;
    if (uid) body["uid"] = *uid;
    if (gid) body["gid"] = *gid;
    if (clear) body["clear"] = true;
    if (iops) body["iops"] = *iops;
    if (bps) body["bps"] = *bps;
    auto r = admin_exchange(std::move(host), std::move(port), "PUT", "/admin/api/qos/limits",
                            body.dump(), key);
    if (r.status != 200) {
      std::cerr << "qos set failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << "ok\n";
    return 0;
  }
  if (action == "project") {
    if (args.size() < 3 || args[2] != "set") {
      std::cerr << "usage: admin qos project set --id ID [--uid UID] --iops N|--bps SIZE|--clear\n";
      return 2;
    }
    std::uint32_t id = 0;
    std::optional<std::uint32_t> uid;
    std::optional<std::uint64_t> iops, bps;
    bool clear = false;
    for (std::size_t i = 3; i < args.size(); ++i) {
      if (args[i] == "--id" && i + 1 < args.size())
        id = static_cast<std::uint32_t>(std::stoul(args[++i]));
      else if (args[i] == "--uid" && i + 1 < args.size())
        uid = static_cast<std::uint32_t>(std::stoul(args[++i]));
      else if (args[i] == "--iops" && i + 1 < args.size())
        iops = std::stoull(args[++i]);
      else if (args[i] == "--bps" && i + 1 < args.size())
        bps = parse_bytes_arg(args[++i]);
      else if (args[i] == "--clear")
        clear = true;
    }
    if (id == 0) {
      std::cerr << "project set requires --id\n";
      return 2;
    }
    if (!clear && !iops && !bps) {
      std::cerr << "project set requires --iops and/or --bps, or --clear\n";
      return 2;
    }
    nlohmann::json body{{"project_id", id}};
    if (uid) body["uid"] = *uid;
    if (clear) body["clear"] = true;
    if (iops) body["iops"] = *iops;
    if (bps) body["bps"] = *bps;
    auto r = admin_exchange(std::move(host), std::move(port), "PUT", "/admin/api/qos/limits",
                            body.dump(), key);
    if (r.status != 200) {
      std::cerr << "qos project set failed status=" << r.status << " " << r.body << "\n";
      return 1;
    }
    std::cout << "ok\n";
    return 0;
  }
  std::cerr << "unknown qos action\n";
  return 2;
}

int run_admin_console(std::string host, std::string port, const std::string& key) {
  std::cout << "AIOS admin console  endpoint=" << host << ':' << port << "\n";
  admin_console_help();
  std::string line;
  while (true) {
    std::cout << "admin> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    // trim
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
      line.erase(line.begin());
    }
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
      line.pop_back();
    }
    if (line.empty()) continue;
    if (line == "quit" || line == "exit" || line == "q") break;
    if (line == "help" || line == "?") {
      admin_console_help();
      continue;
    }
    int rc = 0;
    if (line == "status") rc = cmd_admin_status(host, port, key);
    else if (line == "ops") rc = cmd_admin_ops(host, port, key);
    else if (line == "config") rc = cmd_admin_config(host, port, key);
    else if (line == "cluster") rc = cmd_admin_cluster(host, port, key);
    else if (line == "metrics") rc = cmd_admin_metrics(host, port, key);
    else if (line == "archive" || line.rfind("archive ", 0) == 0) {
      std::vector<std::string> a{"archive"};
      if (line.size() > 7) {
        std::istringstream iss(line.substr(8));
        std::string tok;
        while (iss >> tok) a.push_back(tok);
      }
      rc = cmd_admin_archive(host, port, key, a);
    } else if (line == "backup" || line.rfind("backup ", 0) == 0) {
      std::vector<std::string> a{"backup"};
      if (line.size() > 6) {
        std::istringstream iss(line.substr(7));
        std::string tok;
        while (iss >> tok) a.push_back(tok);
      }
      rc = cmd_admin_backup(host, port, key, a);
    } else if (line == "vbd" || line.rfind("vbd ", 0) == 0) {
      std::vector<std::string> a{"vbd"};
      if (line.size() > 3) {
        std::istringstream iss(line.substr(4));
        std::string tok;
        while (iss >> tok) a.push_back(tok);
      }
      rc = cmd_admin_vbd(host, port, key, a);
    } else if (line == "posix-layout" || line.rfind("posix-layout ", 0) == 0) {
      std::vector<std::string> a{"posix-layout"};
      if (line.size() > 12) {
        std::istringstream iss(line.substr(13));
        std::string tok;
        while (iss >> tok) a.push_back(tok);
      }
      rc = cmd_admin_posix_layout(host, port, key, a);
    } else if (line == "lifecycle" || line.rfind("lifecycle ", 0) == 0) {
      std::vector<std::string> a{"lifecycle"};
      if (line.size() > 9) {
        std::istringstream iss(line.substr(10));
        std::string tok;
        while (iss >> tok) a.push_back(tok);
      }
      rc = cmd_admin_lifecycle(host, port, key, a);
    } else {
      std::cout << "unknown: " << line << "\n";
      admin_console_help();
      continue;
    }
    if (rc != 0) std::cout << "(command failed)\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    usage();
    return 2;
  }
  g_app_label = args.app_label;
  std::string host, port;
  try {
    parse_endpoint(args.endpoint, host, port);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 2;
  }

  try {
    if (args.cmd == "map") {
      auto r = http_exchange(host, port, "GET", "/map", {}, {}, nullptr, args.cluster_key);
      if (r.status < 0) {
        std::cerr << r.error << "\n";
        return 1;
      }
      std::cout << r.body << "\n";
      return r.status == 200 ? 0 : 1;
    }
    if (args.cmd == "list") {
      std::string target = "/o";
      if (!args.prefix.empty()) target += "?prefix=" + url_encode_oid(args.prefix);
      auto r = http_exchange(host, port, "GET", target, {}, {}, nullptr, args.cluster_key);
      if (r.status < 0) {
        std::cerr << r.error << "\n";
        return 1;
      }
      std::cout << r.body << "\n";
      return r.status == 200 ? 0 : 1;
    }
    if (args.cmd == "put") {
      if (args.positional.size() < 2) {
        std::cerr << "put requires OID FILE\n";
        return 2;
      }
      auto r = http_exchange(host, port, "PUT", "/o/" + url_encode_oid(args.positional[0]),
                             {}, args.positional[1], nullptr, args.cluster_key);
      if (r.status < 0) {
        std::cerr << r.error << "\n";
        return 1;
      }
      if (r.status != 204) {
        std::cerr << "PUT failed status=" << r.status << " " << r.body << "\n";
        return 1;
      }
      return 0;
    }
    if (args.cmd == "get") {
      if (args.positional.empty()) {
        std::cerr << "get requires OID\n";
        return 2;
      }
      if (!args.out_file.empty()) {
        std::ofstream out(args.out_file, std::ios::binary | std::ios::trunc);
        if (!out) {
          std::cerr << "cannot open " << args.out_file << "\n";
          return 1;
        }
        auto r = http_exchange(host, port, "GET",
                               "/o/" + url_encode_oid(args.positional[0]), {}, {}, &out,
                               args.cluster_key);
        if (r.status < 0) {
          std::cerr << r.error << "\n";
          return 1;
        }
        if (r.status != 200) {
          std::cerr << "GET failed status=" << r.status << " " << r.body << "\n";
          return 1;
        }
        return 0;
      }
      auto r = http_exchange(host, port, "GET", "/o/" + url_encode_oid(args.positional[0]),
                             {}, {}, &std::cout, args.cluster_key);
      if (r.status < 0) {
        std::cerr << r.error << "\n";
        return 1;
      }
      if (r.status != 200) {
        std::cerr << "GET failed status=" << r.status << " " << r.body << "\n";
        return 1;
      }
      return 0;
    }
    if (args.cmd == "del") {
      if (args.positional.empty()) {
        std::cerr << "del requires OID\n";
        return 2;
      }
      auto r = http_exchange(host, port, "DELETE",
                             "/o/" + url_encode_oid(args.positional[0]), {}, {}, nullptr,
                             args.cluster_key);
      if (r.status < 0) {
        std::cerr << r.error << "\n";
        return 1;
      }
      if (r.status != 204) {
        std::cerr << "DELETE failed status=" << r.status << " " << r.body << "\n";
        return 1;
      }
      return 0;
    }
    if (args.cmd == "stat") {
      if (args.positional.empty()) {
        std::cerr << "stat requires OID\n";
        return 2;
      }
      auto r = http_exchange(host, port, "HEAD", "/o/" + url_encode_oid(args.positional[0]),
                             {}, {}, nullptr, args.cluster_key);
      if (r.status < 0) {
        std::cerr << r.error << "\n";
        return 1;
      }
      if (r.status != 200) {
        std::cerr << "HEAD failed status=" << r.status << "\n";
        return 1;
      }
      for (const auto& [k, v] : r.headers) {
        if (k.rfind("x-aios-", 0) == 0 || k == "content-length") {
          std::cout << k << ": " << v << "\n";
        }
      }
      return 0;
    }
    if (args.cmd == "admin") {
      std::string sub = args.positional.empty() ? "console" : args.positional[0];
      if (sub == "console" || sub == "shell" || sub == "repl") {
        return run_admin_console(host, port, args.cluster_key);
      }
      if (sub == "status") return cmd_admin_status(host, port, args.cluster_key);
      if (sub == "ops") return cmd_admin_ops(host, port, args.cluster_key);
      if (sub == "config") return cmd_admin_config(host, port, args.cluster_key);
      if (sub == "cluster") return cmd_admin_cluster(host, port, args.cluster_key);
      if (sub == "metrics") return cmd_admin_metrics(host, port, args.cluster_key);
      if (sub == "archive")
        return cmd_admin_archive(host, port, args.cluster_key, args.positional);
      if (sub == "backup")
        return cmd_admin_backup(host, port, args.cluster_key, args.positional);
      if (sub == "vbd") return cmd_admin_vbd(host, port, args.cluster_key, args.positional);
      if (sub == "posix-layout")
        return cmd_admin_posix_layout(host, port, args.cluster_key, args.positional);
      if (sub == "lifecycle")
        return cmd_admin_lifecycle(host, port, args.cluster_key, args.positional);
      if (sub == "s3-cred")
        return cmd_admin_s3_cred(host, port, args.cluster_key, args.positional);
      if (sub == "quota") return cmd_admin_quota(host, port, args.cluster_key, args.positional);
      if (sub == "qos") return cmd_admin_qos(host, port, args.cluster_key, args.positional);
      std::cerr << "unknown admin subcommand: " << sub << "\n";
      return 2;
    }
    std::cerr << "unknown command: " << args.cmd << "\n";
    return 2;
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
