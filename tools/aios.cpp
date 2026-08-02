#include "http/http_auth.hpp"
#include "util/log.hpp"

#include <boost/asio.hpp>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

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
      << "Follows HTTP 307 redirects to the primary (Location).\n";
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

struct HttpResp {
  int status{-1};
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  std::string error;
};

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

HttpResp http_request(std::string host, std::string port, const std::string& method,
                      const std::string& target,
                      std::unordered_map<std::string, std::string> headers,
                      const std::uint8_t* body, std::size_t body_len,
                      const std::string& cluster_key, int max_redirects = 5) {
  HttpResp resp;
  for (int hop = 0; hop <= max_redirects; ++hop) {
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
    if (!ec && body_len > 0) asio::write(sock, asio::buffer(body, body_len), ec);
    if (ec) {
      resp.error = "write: " + ec.message();
      return resp;
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
    resp.body.resize(content_length);
    std::size_t have = static_cast<std::size_t>(buf.size());
    if (have > content_length) have = content_length;
    if (have) is.read(resp.body.data(), static_cast<std::streamsize>(have));
    std::size_t need = content_length - have;
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

    if (resp.status == 307 || resp.status == 301 || resp.status == 302) {
      auto it = resp.headers.find("location");
      if (it == resp.headers.end() || hop == max_redirects) return resp;
      // Absolute http://host:port/path or relative /path
      const auto& loc = it->second;
      if (loc.rfind("http://", 0) == 0) {
        auto rest = loc.substr(7);
        auto slash = rest.find('/');
        auto hp = slash == std::string::npos ? rest : rest.substr(0, slash);
        auto path = slash == std::string::npos ? std::string("/") : rest.substr(slash);
        auto colon = hp.rfind(':');
        if (colon == std::string::npos) {
          host = hp;
          port = "80";
        } else {
          host = hp.substr(0, colon);
          port = hp.substr(colon + 1);
        }
        return http_request(host, port, method, path, headers, body, body_len, cluster_key,
                            max_redirects - hop - 1);
      }
      return http_request(host, port, method, loc, headers, body, body_len, cluster_key,
                          max_redirects - hop - 1);
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
      auto r = http_request(host, port, "GET", "/map", {}, nullptr, 0, args.cluster_key);
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
      auto r = http_request(host, port, "GET", target, {}, nullptr, 0, args.cluster_key);
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
      const auto& oid = args.positional[0];
      std::ifstream in(args.positional[1], std::ios::binary);
      if (!in) {
        std::cerr << "cannot open " << args.positional[1] << "\n";
        return 1;
      }
      std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), {});
      auto r = http_request(host, port, "PUT", "/o/" + url_encode_oid(oid), {}, data.data(),
                            data.size(), args.cluster_key);
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
      auto r = http_request(host, port, "GET", "/o/" + url_encode_oid(args.positional[0]), {},
                            nullptr, 0, args.cluster_key);
      if (r.status < 0) {
        std::cerr << r.error << "\n";
        return 1;
      }
      if (r.status != 200) {
        std::cerr << "GET failed status=" << r.status << " " << r.body << "\n";
        return 1;
      }
      if (!args.out_file.empty()) {
        std::ofstream out(args.out_file, std::ios::binary);
        out.write(r.body.data(), static_cast<std::streamsize>(r.body.size()));
      } else {
        std::cout.write(r.body.data(), static_cast<std::streamsize>(r.body.size()));
      }
      return 0;
    }
    if (args.cmd == "del") {
      if (args.positional.empty()) {
        std::cerr << "del requires OID\n";
        return 2;
      }
      auto r = http_request(host, port, "DELETE", "/o/" + url_encode_oid(args.positional[0]),
                            {}, nullptr, 0, args.cluster_key);
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
      auto r = http_request(host, port, "HEAD", "/o/" + url_encode_oid(args.positional[0]),
                            {}, nullptr, 0, args.cluster_key);
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
