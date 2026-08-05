// Thin SigV4 S3 client with optional x-amz-rdma-token for cuObject GPUDirect.
// Without the NVIDIA client SDK, --rdma uses a synthetic token (works with
// StubCuObjectEndpoint in tests); production hosts should build with cuObjClient
// or use --tcp for classic body transfer.

#include "cuobject/cuobject_endpoint.hpp"
#include "util/auth.hpp"

#include <boost/asio.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

void usage() {
  std::cerr
      << "usage: aios-cuobj-s3 get|put --endpoint HOST:PORT --access-key KEY --secret SECRET\n"
      << "         s3://bucket/key --file PATH [--rdma|--tcp] [--region REGION]\n"
      << "\n"
      << "  --tcp   Force classic HTTP body transfer (default when --rdma omitted).\n"
      << "  --rdma  Send x-amz-rdma-token (synthetic without cuObjClient SDK).\n";
}

std::string amz_now() {
  const auto t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

std::string hex_lower(const std::string& raw) {
  static const char* hexd = "0123456789abcdef";
  std::string out(raw.size() * 2, '\0');
  for (std::size_t i = 0; i < raw.size(); ++i) {
    auto c = static_cast<unsigned char>(raw[i]);
    out[i * 2] = hexd[c >> 4];
    out[i * 2 + 1] = hexd[c & 0xf];
  }
  return out;
}

void sign_s3(const std::string& method, const std::string& uri, const std::string& hostport,
             const std::string& amz, const std::string& payload_hash, const std::string& access,
             const std::string& secret, const std::string& region,
             std::unordered_map<std::string, std::string>& headers) {
  using namespace aios;
  const auto ds = amz.substr(0, 8);
  headers["x-amz-date"] = amz;
  headers["x-amz-content-sha256"] = payload_hash;
  headers["host"] = hostport;
  std::vector<std::pair<std::string, std::string>> amz_hdrs;
  for (const auto& [k, v] : headers) {
    std::string lk = k;
    for (char& c : lk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lk == "host" || lk.rfind("x-amz-", 0) == 0) amz_hdrs.emplace_back(lk, v);
  }
  std::sort(amz_hdrs.begin(), amz_hdrs.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::ostringstream canon_headers;
  std::string signed_headers;
  for (std::size_t i = 0; i < amz_hdrs.size(); ++i) {
    if (i) signed_headers.push_back(';');
    signed_headers += amz_hdrs[i].first;
    canon_headers << amz_hdrs[i].first << ':' << amz_hdrs[i].second << '\n';
  }
  std::ostringstream canon;
  canon << method << '\n' << uri << '\n' << '\n' << canon_headers.str() << signed_headers << '\n'
        << payload_hash;
  const auto canon_hash = sha256_hex(canon.str());
  std::ostringstream sts;
  sts << "AWS4-HMAC-SHA256\n" << amz << '\n' << ds << '/' << region << "/s3/aws4_request\n"
      << canon_hash;
  auto k_date = hmac_sha256_raw("AWS4" + secret, ds);
  auto k_region = hmac_sha256_raw(k_date, region);
  auto k_service = hmac_sha256_raw(k_region, "s3");
  auto k_signing = hmac_sha256_raw(k_service, "aws4_request");
  const auto sig = hex_lower(hmac_sha256_raw(k_signing, sts.str()));
  headers["authorization"] =
      "AWS4-HMAC-SHA256 Credential=" + access + "/" + ds + "/" + region +
      "/s3/aws4_request, SignedHeaders=" + signed_headers + ", Signature=" + sig;
}

bool split_host_port(const std::string& ep, std::string& host, std::string& port) {
  auto colon = ep.rfind(':');
  if (colon == std::string::npos) return false;
  host = ep.substr(0, colon);
  port = ep.substr(colon + 1);
  return !host.empty() && !port.empty();
}

bool parse_s3_uri(const std::string& uri, std::string& bucket, std::string& key) {
  if (uri.rfind("s3://", 0) != 0) return false;
  auto rest = uri.substr(5);
  auto slash = rest.find('/');
  if (slash == std::string::npos || slash == 0) return false;
  bucket = rest.substr(0, slash);
  key = rest.substr(slash + 1);
  return !bucket.empty() && !key.empty();
}

struct HttpResp {
  int status{-1};
  std::string body;
  std::unordered_map<std::string, std::string> headers;
};

HttpResp http_req(const std::string& host, const std::string& port, const std::string& method,
                  const std::string& target, std::unordered_map<std::string, std::string> headers,
                  const std::string& body) {
  HttpResp resp;
  if (!body.empty() && headers.find("content-length") == headers.end()) {
    headers["content-length"] = std::to_string(body.size());
  }
  asio::io_context ioc;
  tcp::resolver resolver(ioc);
  boost::system::error_code ec;
  auto endpoints = resolver.resolve(host, port, ec);
  if (ec) {
    std::cerr << "resolve failed: " << ec.message() << "\n";
    return resp;
  }
  tcp::socket sock(ioc);
  asio::connect(sock, endpoints, ec);
  if (ec) {
    std::cerr << "connect failed: " << ec.message() << "\n";
    return resp;
  }
  std::ostringstream req;
  req << method << ' ' << target << " HTTP/1.1\r\n";
  req << "Host: " << host << ':' << port << "\r\n";
  req << "Connection: close\r\n";
  for (const auto& [k, v] : headers) {
    if (k == "host" || k == "Host") continue;
    req << k << ": " << v << "\r\n";
  }
  req << "\r\n";
  auto head = req.str();
  asio::write(sock, asio::buffer(head), ec);
  if (!body.empty()) asio::write(sock, asio::buffer(body), ec);
  asio::streambuf buf;
  asio::read_until(sock, buf, "\r\n\r\n", ec);
  std::istream is(&buf);
  std::string status_line;
  std::getline(is, status_line);
  {
    std::istringstream ss(status_line);
    std::string ver;
    ss >> ver >> resp.status;
  }
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
    if (name == "content-length") content_length = static_cast<std::size_t>(std::stoull(value));
  }
  if (resp.headers.count(aios::kAmzRdmaReply)) {
    resp.body.clear();
    return resp;
  }
  std::size_t have = static_cast<std::size_t>(buf.size());
  if (have > content_length) have = content_length;
  resp.body.assign(have, '\0');
  if (have) is.read(resp.body.data(), static_cast<std::streamsize>(have));
  std::size_t need = content_length > have ? content_length - have : 0;
  resp.body.resize(content_length);
  while (need > 0) {
    const auto n =
        asio::read(sock, asio::buffer(resp.body.data() + (content_length - need), need),
                   asio::transfer_at_least(1), ec);
    if (ec) break;
    need -= n;
  }
  return resp;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool write_file(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
  std::string cmd;
  std::string endpoint;
  std::string access = "aios";
  std::string secret;
  std::string region = "us-east-1";
  std::string s3uri;
  std::string file;
  bool want_rdma = false;
  bool force_tcp = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "get" || a == "put") {
      cmd = a;
    } else if (a == "--endpoint" && i + 1 < argc) {
      endpoint = argv[++i];
    } else if (a == "--access-key" && i + 1 < argc) {
      access = argv[++i];
    } else if (a == "--secret" && i + 1 < argc) {
      secret = argv[++i];
    } else if (a == "--region" && i + 1 < argc) {
      region = argv[++i];
    } else if (a == "--file" && i + 1 < argc) {
      file = argv[++i];
    } else if (a == "--rdma") {
      want_rdma = true;
    } else if (a == "--tcp") {
      force_tcp = true;
      want_rdma = false;
    } else if (a.rfind("s3://", 0) == 0) {
      s3uri = a;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      usage();
      return 2;
    }
  }

  if (cmd.empty() || endpoint.empty() || secret.empty() || s3uri.empty() || file.empty()) {
    usage();
    return 2;
  }
  if (force_tcp) want_rdma = false;

  std::string host, port, bucket, key;
  if (!split_host_port(endpoint, host, port) || !parse_s3_uri(s3uri, bucket, key)) {
    std::cerr << "bad endpoint or s3 uri\n";
    return 2;
  }
  const std::string hostport = host + ":" + port;
  const std::string uri = "/" + bucket + "/" + key;

  if (cmd == "put") {
    auto body = read_file(file);
    if (body.empty() && !std::ifstream(file)) {
      std::cerr << "cannot read " << file << "\n";
      return 1;
    }
    std::unordered_map<std::string, std::string> sh;
    const auto amz = amz_now();
    std::string payload_hash;
    std::string tcp_body;
    if (want_rdma) {
      // Synthetic token when cuObjClient is not linked; real builds replace this.
      sh[aios::kAmzRdmaToken] = "aios-synthetic-rdma-token";
      sh["content-length"] = std::to_string(body.size());
      payload_hash = "UNSIGNED-PAYLOAD";
      // Host-side note: without GPU RDMA, server stub fills from its own payload.
      // Real cuObjClient registers `body` and generates a fabric token here.
      (void)body;
    } else {
      tcp_body = body;
      payload_hash = aios::sha256_hex(tcp_body);
    }
    sign_s3("PUT", uri, hostport, amz, payload_hash, access, secret, region, sh);
    auto r = http_req(host, port, "PUT", uri, sh, tcp_body);
    if (r.status != 200) {
      std::cerr << "PUT failed status=" << r.status << "\n" << r.body << "\n";
      return 1;
    }
    if (want_rdma) {
      auto it = r.headers.find(aios::kAmzRdmaReply);
      if (it == r.headers.end()) {
        std::cerr << "warning: no " << aios::kAmzRdmaReply
                  << " (server fell back or RDMA unavailable)\n";
      } else {
        std::cout << "rdma-reply: " << it->second << "\n";
      }
    }
    std::cout << "ok put " << s3uri << " (" << body.size() << " bytes)\n";
    return 0;
  }

  // GET
  {
    std::unordered_map<std::string, std::string> sh;
    const auto amz = amz_now();
    if (want_rdma) sh[aios::kAmzRdmaToken] = "aios-synthetic-rdma-token";
    sign_s3("GET", uri, hostport, amz, aios::sha256_hex(""), access, secret, region, sh);
    auto r = http_req(host, port, "GET", uri, sh, "");
    if (r.status != 200 && r.status != 206) {
      std::cerr << "GET failed status=" << r.status << "\n" << r.body << "\n";
      return 1;
    }
    auto reply = r.headers.find(aios::kAmzRdmaReply);
    if (want_rdma && reply != r.headers.end()) {
      std::cout << "rdma-reply: " << reply->second << "\n";
      // Without cuObjClient, data is already in GPU/host via RDMA; we only see empty HTTP body.
      // Write a marker file so scripts can detect offload success.
      if (r.body.empty()) {
        if (!write_file(file, std::string("# aios-cuobj-s3: payload delivered via RDMA; "
                                          "Content-Length=") +
                                  r.headers["content-length"] + "\n")) {
          std::cerr << "cannot write " << file << "\n";
          return 1;
        }
        std::cout << "ok get " << s3uri << " via RDMA (marker written to " << file << ")\n";
        return 0;
      }
    }
    if (want_rdma && reply == r.headers.end() && !r.body.empty()) {
      std::cerr << "note: RDMA requested but TCP body returned (fallback)\n";
    }
    if (!write_file(file, r.body)) {
      std::cerr << "cannot write " << file << "\n";
      return 1;
    }
    std::cout << "ok get " << s3uri << " (" << r.body.size() << " bytes) -> " << file << "\n";
    return 0;
  }
}
