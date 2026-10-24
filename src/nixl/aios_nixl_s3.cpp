#include "nixl/aios_nixl.hpp"

#include "cuobject/cuobject_endpoint.hpp"
#include "http/s3_auth.hpp"
#include "util/auth.hpp"

#include <boost/asio.hpp>

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>
#include <utility>
#include <vector>

namespace aios {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

std::string amz_now() {
  const auto t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

bool split_endpoint(const std::string& ep, std::string& host, std::string& port) {
  const auto colon = ep.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= ep.size()) return false;
  host = ep.substr(0, colon);
  port = ep.substr(colon + 1);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  return !host.empty() && !port.empty();
}

bool split_object_key(const std::string& key, std::string& bucket, std::string& object) {
  const auto slash = key.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= key.size()) return false;
  bucket = key.substr(0, slash);
  object = key.substr(slash + 1);
  return true;
}

// Same segment encoding S3Server applies to the request-target before verifying SigV4.
std::string encode_request_target(const std::string& target) {
  std::vector<std::string> segs;
  std::string cur;
  for (std::size_t i = 1; i < target.size(); ++i) {
    if (target[i] == '/') {
      segs.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(target[i]);
    }
  }
  segs.push_back(cur);
  std::ostringstream pe;
  pe << '/';
  for (std::size_t i = 0; i < segs.size(); ++i) {
    if (i) pe << '/';
    pe << s3_uri_encode(segs[i], true);
  }
  if (target.size() > 1 && target.back() == '/') pe << '/';
  auto out = pe.str();
  return out.empty() ? "/" : out;
}

void sign_s3(const std::string& method, const std::string& canonical_uri,
             const std::string& hostport, const std::string& amz, const std::string& payload_hash,
             const std::string& access, const std::string& secret, const std::string& region,
             std::unordered_map<std::string, std::string>& headers) {
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
  canon << method << '\n'
        << canonical_uri << '\n'
        << '\n'
        << canon_headers.str() << signed_headers << '\n'
        << payload_hash;
  const auto canon_hash = sha256_hex(canon.str());
  std::ostringstream sts;
  sts << "AWS4-HMAC-SHA256\n"
      << amz << '\n'
      << ds << '/' << region << "/s3/aws4_request\n"
      << canon_hash;
  auto k_date = hmac_sha256_raw("AWS4" + secret, ds);
  auto k_region = hmac_sha256_raw(k_date, region);
  auto k_service = hmac_sha256_raw(k_region, "s3");
  auto k_signing = hmac_sha256_raw(k_service, "aws4_request");
  static const char* hexd = "0123456789abcdef";
  const auto raw = hmac_sha256_raw(k_signing, sts.str());
  std::string sig(raw.size() * 2, '\0');
  for (std::size_t i = 0; i < raw.size(); ++i) {
    auto c = static_cast<unsigned char>(raw[i]);
    sig[i * 2] = hexd[c >> 4];
    sig[i * 2 + 1] = hexd[c & 0xf];
  }
  headers["authorization"] = "AWS4-HMAC-SHA256 Credential=" + access + "/" + ds + "/" + region +
                             "/s3/aws4_request, SignedHeaders=" + signed_headers +
                             ", Signature=" + sig;
}

void set_timeout(tcp::socket& sock, int timeout_ms) {
  if (timeout_ms <= 0) return;
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

class S3CuObjectTransport final : public NixlRdmaTransport {
 public:
  explicit S3CuObjectTransport(NixlConfig cfg) : cfg_(std::move(cfg)) {}

  bool read_object(const std::string& key, std::size_t len, const std::string& rdma_token,
                   std::string& err) override {
    return exchange(false, key, len, rdma_token, err);
  }

  bool write_object(const std::string& key, std::size_t len, const std::string& rdma_token,
                    std::string& err) override {
    return exchange(true, key, len, rdma_token, err);
  }

 private:
  bool exchange(bool is_put, const std::string& key, std::size_t len, const std::string& rdma_token,
                std::string& err) {
    std::string bucket;
    std::string object;
    if (!split_object_key(key, bucket, object)) {
      err = "object key must be bucket/key";
      return false;
    }
    const auto built =
        build_s3_rdma_request(cfg_, is_put, bucket, object, len, rdma_token, amz_now());
    if (!built.error.empty()) {
      err = built.error;
      return false;
    }
    try {
      asio::io_context ioc;
      tcp::resolver resolver(ioc);
      boost::system::error_code ec;
      auto endpoints = resolver.resolve(built.host, built.port, ec);
      if (ec) {
        err = "resolve failed: " + ec.message();
        return false;
      }
      tcp::socket sock(ioc);
      asio::connect(sock, endpoints, ec);
      if (ec) {
        err = "connect failed: " + ec.message();
        return false;
      }
      set_timeout(sock, cfg_.socket_timeout_ms);
      std::ostringstream req;
      req << built.method << ' ' << built.target << " HTTP/1.1\r\n";
      req << "Host: " << cfg_.s3_endpoint << "\r\n";
      req << "Connection: close\r\n";
      for (const auto& [k, v] : built.headers) {
        if (k == "host") continue;
        req << k << ": " << v << "\r\n";
      }
      req << "\r\n";
      const auto bytes = req.str();
      asio::write(sock, asio::buffer(bytes), ec);
      if (ec) {
        err = "write failed: " + ec.message();
        return false;
      }
      asio::streambuf buf;
      asio::read_until(sock, buf, "\r\n\r\n", ec);
      if (ec && buf.size() == 0) {
        err = "read failed: " + ec.message();
        return false;
      }
      std::istream is(&buf);
      std::string status_line;
      std::getline(is, status_line);
      if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();
      int status = -1;
      {
        std::istringstream ss(status_line);
        std::string ver;
        ss >> ver >> status;
      }
      std::unordered_map<std::string, std::string> resp;
      std::string line;
      while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto name = line.substr(0, colon);
        auto value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        resp.emplace(std::move(name), std::move(value));
      }
      const auto reply = resp.find(kAmzRdmaReply);
      const bool reply_ok =
          reply != resp.end() && ((is_put && status == 200 && reply->second == "200") ||
                                  (!is_put && (status == 200 || status == 206) &&
                                   reply->second == std::to_string(status)));
      if (!reply_ok) {
        err = "cuObject RDMA was not used (x-amz-rdma-reply must be 200 or 206)";
        return false;
      }
      const auto etag = resp.find("etag");
      if (etag == resp.end() || etag->second.empty()) {
        err = "RDMA response missing etag";
        return false;
      }
      const auto moved = resp.find(kAmzRdmaBytesTransferred);
      unsigned long long reported = 0;
      if (moved == resp.end()) {
        err = "missing x-amz-rdma-bytes-transferred";
        return false;
      }
      try {
        reported = std::stoull(moved->second);
      } catch (...) {
        err = "bad x-amz-rdma-bytes-transferred";
        return false;
      }
      if (reported != len) {
        err = "x-amz-rdma-bytes-transferred does not match the registered buffer";
        return false;
      }
      if (!is_put) {
        const auto cl = resp.find("content-length");
        if (cl != resp.end()) {
          unsigned long long http_len = 0;
          try {
            http_len = std::stoull(cl->second);
          } catch (...) {
            err = "RDMA GET bad content-length";
            return false;
          }
          if (http_len != 0 && http_len != len) {
            err = "RDMA GET content-length does not match the registered buffer";
            return false;
          }
        }
      }
      return true;
    } catch (const std::exception& e) {
      err = e.what();
      return false;
    }
  }

  NixlConfig cfg_;
};

}  // namespace

S3RdmaRequest build_s3_rdma_request(const NixlConfig& cfg, bool is_put, const std::string& bucket,
                                    const std::string& object, std::size_t len,
                                    const std::string& rdma_token, const std::string& amz_date) {
  S3RdmaRequest out;
  if (!split_endpoint(cfg.s3_endpoint, out.host, out.port)) {
    out.error = "s3_endpoint must be host:port";
    return out;
  }
  if (cfg.access_key.empty() || cfg.secret_key.empty()) {
    out.error = "access_key and secret_key are required";
    return out;
  }
  if (bucket.empty() || object.empty() || rdma_token.empty() || amz_date.size() < 8) {
    out.error = "bucket, object, RDMA token, and x-amz-date are required";
    return out;
  }
  out.method = is_put ? "PUT" : "GET";
  out.target = "/" + s3_uri_encode(bucket, true) + "/" + s3_uri_encode(object, false);
  out.canonical_uri = encode_request_target(out.target);
  out.payload_hash = is_put ? "UNSIGNED-PAYLOAD" : sha256_hex("");
  out.headers[kAmzRdmaToken] = rdma_token;
  if (is_put) out.headers["content-length"] = std::to_string(len);
  // NIXL's accelerated GET always sends this range. A whole-object span is
  // reply 200; a prefix of a larger object is reply 206.
  if (!is_put && len > 0) out.headers["range"] = "bytes=0-" + std::to_string(len - 1);
  const auto region = cfg.region.empty() ? "us-east-1" : cfg.region;
  sign_s3(out.method, out.canonical_uri, cfg.s3_endpoint, amz_date, out.payload_hash,
          cfg.access_key, cfg.secret_key, region, out.headers);
  return out;
}

std::shared_ptr<NixlRdmaTransport> make_s3_cuobject_transport(const NixlConfig& cfg) {
  std::string host;
  std::string port;
  if (!split_endpoint(cfg.s3_endpoint, host, port) || cfg.access_key.empty() ||
      cfg.secret_key.empty()) {
    return nullptr;
  }
  return std::make_shared<S3CuObjectTransport>(cfg);
}

}  // namespace aios
