#include "http/http_auth.hpp"
#include "util/log.hpp"

#include <boost/asio.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
  std::string cmd;
  std::vector<std::string> positional;
  std::string out_file;
  std::string prefix;
};

void usage() {
  std::cout
      << "usage: aios --cluster-key KEY [--endpoint HOST:PORT] <cmd> [args]\n"
      << "\n"
      << "Commands:\n"
      << "  put  OID FILE\n"
      << "  get  OID [-o FILE]\n"
      << "  del  OID\n"
      << "  stat OID\n"
      << "  list [--prefix P]\n"
      << "  map\n"
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

}  // namespace

int main(int argc, char** argv) {
  Args args;
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
    std::cerr << "unknown command: " << args.cmd << "\n";
    return 2;
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
