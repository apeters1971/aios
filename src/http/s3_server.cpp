#include "http/s3_server.hpp"

#include "cuobject/cuobject_endpoint.hpp"
#include "cuobject/cuobject_s3_xfer.hpp"
#include "http/s3_auth.hpp"
#include "posix/aios_posix.h"
#include "util/auth.hpp"
#include "util/log.hpp"

#include <openssl/evp.h>

#include <errno.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aios {
namespace {

using tcp = boost::asio::ip::tcp;

constexpr uint64_t kRootIno = 1;
constexpr const char* kMultipartDir = ".s3multipart";
constexpr const char* kXattrContentType = "user.aios.s3.content-type";
constexpr const char* kXattrMetaPrefix = "user.aios.s3.meta.";

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string header_get(const std::unordered_map<std::string, std::string>& h, const std::string& n) {
  auto it = h.find(lower(n));
  return it == h.end() ? std::string{} : it->second;
}

std::string xml_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&apos;"; break;
      default: o.push_back(c);
    }
  }
  return o;
}

std::string url_decode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size()) {
      auto hex = in.substr(i + 1, 2);
      char* end = nullptr;
      const long v = std::strtol(hex.c_str(), &end, 16);
      if (end && *end == '\0') {
        out.push_back(static_cast<char>(v));
        i += 2;
        continue;
      }
    }
    if (in[i] == '+') out.push_back(' ');
    else out.push_back(in[i]);
  }
  return out;
}

std::unordered_map<std::string, std::string> parse_query(const std::string& q) {
  std::unordered_map<std::string, std::string> out;
  std::size_t i = 0;
  while (i < q.size()) {
    auto amp = q.find('&', i);
    if (amp == std::string::npos) amp = q.size();
    auto eq = q.find('=', i);
    if (eq != std::string::npos && eq < amp) {
      out[url_decode(q.substr(i, eq - i))] = url_decode(q.substr(eq + 1, amp - eq - 1));
    } else if (amp > i) {
      out[url_decode(q.substr(i, amp - i))] = "";
    }
    i = amp + 1;
  }
  return out;
}

// SigV4 canonical query: sorted, URI-encoded keys/values, joined with &.
std::string canonical_query_string(const std::string& raw_query) {
  if (raw_query.empty()) return {};
  auto q = parse_query(raw_query);
  std::vector<std::pair<std::string, std::string>> items(q.begin(), q.end());
  std::sort(items.begin(), items.end());
  std::ostringstream oss;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) oss << '&';
    oss << s3_uri_encode(items[i].first, true) << '=' << s3_uri_encode(items[i].second, true);
  }
  return oss.str();
}

std::string md5_hex(const std::uint8_t* data, std::size_t len) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return {};
  if (EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1 ||
      EVP_DigestUpdate(ctx, data, len) != 1 || EVP_DigestFinal_ex(ctx, md, &md_len) != 1) {
    EVP_MD_CTX_free(ctx);
    return {};
  }
  EVP_MD_CTX_free(ctx);
  static const char* hexd = "0123456789abcdef";
  std::string out(md_len * 2, '\0');
  for (unsigned int i = 0; i < md_len; ++i) {
    out[i * 2] = hexd[md[i] >> 4];
    out[i * 2 + 1] = hexd[md[i] & 0xf];
  }
  return out;
}

std::string iso8601_from_ns(uint64_t ns) {
  const auto secs = static_cast<std::time_t>(ns / 1000000000ull);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &secs);
#else
  gmtime_r(&secs, &tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
  return buf;
}

void write_http(tcp::socket& sock, int status, const std::string& reason,
                const std::unordered_map<std::string, std::string>& headers,
                const std::string& body) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
  bool has_cl = false;
  for (const auto& [k, v] : headers) {
    if (lower(k) == "content-length") {
      has_cl = true;
      break;
    }
  }
  // Allow callers (RDMA GET) to advertise logical size while sending an empty TCP body.
  if (!has_cl) oss << "Content-Length: " << body.size() << "\r\n";
  oss << "Connection: close\r\n";
  for (const auto& [k, v] : headers) oss << k << ": " << v << "\r\n";
  oss << "\r\n";
  boost::system::error_code ec;
  auto head = oss.str();
  boost::asio::write(sock, boost::asio::buffer(head), ec);
  if (!ec && !body.empty()) boost::asio::write(sock, boost::asio::buffer(body), ec);
}

void write_s3_error(tcp::socket& sock, int status, const std::string& code,
                    const std::string& message, const std::string& resource = {}) {
  std::ostringstream xml;
  xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      << "<Error><Code>" << xml_escape(code) << "</Code><Message>" << xml_escape(message)
      << "</Message>";
  if (!resource.empty()) xml << "<Resource>" << xml_escape(resource) << "</Resource>";
  xml << "</Error>";
  write_http(sock, status, code, {{"Content-Type", "application/xml"}}, xml.str());
}

bool valid_bucket_name(const std::string& b) {
  if (b.size() < 3 || b.size() > 63) return false;
  if (b.front() == '.' || b.back() == '.' || b.front() == '-' || b.back() == '-') return false;
  for (char c : b) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
  }
  if (b == kMultipartDir || b[0] == '.') return false;
  return true;
}

std::vector<std::string> split_key(const std::string& key) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : key) {
    if (c == '/') {
      if (!cur.empty()) {
        parts.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  return parts;
}

int lookup_path(aios_posix_fs* fs, const std::vector<std::string>& parts, uint64_t* ino_out,
                aios_posix_stat* st_out) {
  uint64_t ino = kRootIno;
  aios_posix_stat st{};
  int err = aios_posix_getattr(fs, ino, &st);
  if (err) return err;
  for (const auto& p : parts) {
    err = aios_posix_lookup(fs, ino, p.c_str(), &st);
    if (err) return err;
    ino = st.ino;
  }
  if (ino_out) *ino_out = ino;
  if (st_out) *st_out = st;
  return 0;
}

void apply_owner(aios_posix_fs* fs, uint64_t ino, bool set_owner, uint32_t uid, uint32_t gid) {
  if (!set_owner) return;
  aios_posix_stat st{};
  st.uid = uid;
  st.gid = gid;
  aios_posix_setattr(fs, ino, &st, AIOS_POSIX_SET_UID | AIOS_POSIX_SET_GID);
}

bool write_posix_err(tcp::socket& sock, int err, const std::string& path) {
  if (err == -EDQUOT) {
    write_s3_error(sock, 403, "QuotaExceeded", "quota exceeded", path);
    return true;
  }
  if (err == -EAGAIN) {
    write_s3_error(sock, 503, "SlowDown", "qos rate limit exceeded", path);
    return true;
  }
  return false;
}

int mkdir_p(aios_posix_fs* fs, const std::vector<std::string>& parts, uint64_t* ino_out,
            bool set_owner = false, uint32_t uid = 0, uint32_t gid = 0) {
  uint64_t ino = kRootIno;
  for (const auto& p : parts) {
    aios_posix_stat st{};
    int err = aios_posix_lookup(fs, ino, p.c_str(), &st);
    if (err == -ENOENT) {
      err = aios_posix_mkdir(fs, ino, p.c_str(), 0755, &st);
      if (err) return err;
      apply_owner(fs, st.ino, set_owner, uid, gid);
    } else if (err) {
      return err;
    } else if (!S_ISDIR(st.mode)) {
      return -ENOTDIR;
    }
    ino = st.ino;
  }
  if (ino_out) *ino_out = ino;
  return 0;
}

int resolve_parent(aios_posix_fs* fs, const std::string& bucket, const std::string& key,
                   bool create_dirs, uint64_t* parent_out, std::string* name_out,
                   uint64_t* bucket_ino_out = nullptr, bool set_owner = false, uint32_t uid = 0,
                   uint32_t gid = 0) {
  if (!valid_bucket_name(bucket)) return -EINVAL;
  aios_posix_stat bst{};
  int err = aios_posix_lookup(fs, kRootIno, bucket.c_str(), &bst);
  if (err) return err;
  if (!S_ISDIR(bst.mode)) return -ENOTDIR;
  if (bucket_ino_out) *bucket_ino_out = bst.ino;

  auto parts = split_key(key);
  if (parts.empty()) return -EINVAL;
  *name_out = parts.back();
  parts.pop_back();
  uint64_t parent = bst.ino;
  if (!parts.empty()) {
    if (create_dirs) {
      // relative to bucket
      uint64_t cur = bst.ino;
      for (const auto& p : parts) {
        aios_posix_stat st{};
        err = aios_posix_lookup(fs, cur, p.c_str(), &st);
        if (err == -ENOENT) {
          err = aios_posix_mkdir(fs, cur, p.c_str(), 0755, &st);
          if (err) return err;
          apply_owner(fs, st.ino, set_owner, uid, gid);
        } else if (err) {
          return err;
        } else if (!S_ISDIR(st.mode)) {
          return -ENOTDIR;
        }
        cur = st.ino;
      }
      parent = cur;
    } else {
      std::vector<std::string> full = {bucket};
      full.insert(full.end(), parts.begin(), parts.end());
      err = lookup_path(fs, full, &parent, nullptr);
      if (err) return err;
    }
  }
  *parent_out = parent;
  return 0;
}

int ensure_file(aios_posix_fs* fs, uint64_t parent, const std::string& name, uint64_t* ino_out,
                bool set_owner = false, uint32_t uid = 0, uint32_t gid = 0) {
  aios_posix_stat st{};
  int err = aios_posix_lookup(fs, parent, name.c_str(), &st);
  if (err == -ENOENT) {
    err = aios_posix_create(fs, parent, name.c_str(), 0644, &st);
    if (err) return err;
    apply_owner(fs, st.ino, set_owner, uid, gid);
  } else if (err) {
    return err;
  } else if (!S_ISREG(st.mode)) {
    return -EISDIR;
  }
  *ino_out = st.ino;
  return 0;
}

bool dir_empty(aios_posix_fs* fs, uint64_t ino) {
  uint64_t off = 0;
  aios_posix_dirent ents[8];
  while (true) {
    int n = aios_posix_readdir(fs, ino, &off, ents, 8);
    if (n < 0) return false;
    if (n == 0) return true;
    for (int i = 0; i < n; ++i) {
      if (std::strcmp(ents[i].name, ".") == 0 || std::strcmp(ents[i].name, "..") == 0) continue;
      return false;
    }
  }
}

std::string random_upload_id() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<uint64_t> d;
  std::ostringstream oss;
  oss << std::hex << d(rng) << d(rng);
  return oss.str();
}

struct ListEntry {
  std::string key;
  uint64_t size{0};
  uint64_t mtime_ns{0};
  bool is_dir{false};
};

void collect_list(aios_posix_fs* fs, uint64_t dir_ino, const std::string& prefix_path,
                  const std::string& prefix_filter, const std::string& delimiter,
                  std::vector<ListEntry>& keys, std::vector<std::string>& common,
                  int max_keys) {
  if (static_cast<int>(keys.size() + common.size()) >= max_keys) return;
  uint64_t off = 0;
  aios_posix_dirent ents[64];
  for (;;) {
    int n = aios_posix_readdir(fs, dir_ino, &off, ents, 64);
    if (n <= 0) break;
    for (int i = 0; i < n; ++i) {
      if (ents[i].name[0] == '.' &&
          (ents[i].name[1] == '\0' ||
           (ents[i].name[1] == '.' && ents[i].name[2] == '\0')))
        continue;
      std::string name = ents[i].name;
      std::string full = prefix_path.empty() ? name : prefix_path + "/" + name;
      // relative key under bucket: strip leading
      std::string key = full;
      if (!prefix_filter.empty() && key.rfind(prefix_filter, 0) != 0) {
        // may still need to recurse if prefix is under this path
        if (S_ISDIR(ents[i].mode) && prefix_filter.rfind(key + "/", 0) == 0) {
          collect_list(fs, ents[i].ino, key, prefix_filter, delimiter, keys, common, max_keys);
        }
        continue;
      }
      if (!delimiter.empty()) {
        auto rest = key.substr(prefix_filter.size());
        auto slash = rest.find(delimiter);
        if (slash != std::string::npos) {
          std::string cp = prefix_filter + rest.substr(0, slash + delimiter.size());
          if (std::find(common.begin(), common.end(), cp) == common.end()) common.push_back(cp);
          continue;
        }
      }
      if (S_ISDIR(ents[i].mode)) {
        if (delimiter.empty()) {
          collect_list(fs, ents[i].ino, key, prefix_filter, delimiter, keys, common, max_keys);
        }
      } else {
        aios_posix_stat st{};
        if (aios_posix_getattr(fs, ents[i].ino, &st) == 0) {
          keys.push_back({key, st.size, st.mtime_ns, false});
        }
      }
      if (static_cast<int>(keys.size() + common.size()) >= max_keys) return;
    }
  }
}

}  // namespace

std::string s3_loopback_http_endpoint(const std::string& http_listen) {
  std::string host, port;
  if (!split_host_port(http_listen, host, port)) return {};
  if (host == "0.0.0.0" || host == "*" || host == "::" || host.empty()) host = "127.0.0.1";
  return host + ":" + port;
}

S3Server::S3Server(boost::asio::io_context& ioc, Config cfg, std::string posix_http_endpoint,
                   std::shared_ptr<S3IamStore> iam, std::shared_ptr<CuObjectEndpoint> cuobject)
    : ioc_(ioc),
      cfg_(std::move(cfg)),
      posix_endpoint_(std::move(posix_http_endpoint)),
      iam_(std::move(iam)),
      cuobject_(cuobject ? std::move(cuobject) : make_cuobject_endpoint(cfg_)),
      acceptor_(ioc) {}

S3Server::~S3Server() {
  if (fs_) {
    aios_posix_unmount(fs_);
    fs_ = nullptr;
  }
}

void S3Server::start() {
  aios_posix_config pcfg{};
  pcfg.endpoint = posix_endpoint_.c_str();
  pcfg.cluster_key = cfg_.cluster_key.c_str();
  pcfg.volume = cfg_.s3_volume.c_str();
  pcfg.app_label = "s3";
  int err = 0;
  fs_ = aios_posix_mount(&pcfg, &err);
  if (!fs_) {
    throw std::runtime_error("S3: aios_posix_mount failed: " + std::to_string(err));
  }
  // Ensure multipart staging root exists.
  aios_posix_stat st{};
  if (aios_posix_lookup(fs_, kRootIno, kMultipartDir, &st) == -ENOENT) {
    aios_posix_mkdir(fs_, kRootIno, kMultipartDir, 0700, &st);
  }

  std::string host, port;
  if (!split_host_port(cfg_.s3_listen, host, port)) {
    throw std::runtime_error("bad s3_listen: " + cfg_.s3_listen);
  }
  tcp::resolver resolver(ioc_);
  auto eps = resolver.resolve(host, port);
  acceptor_.open(eps.begin()->endpoint().protocol());
  acceptor_.set_option(tcp::acceptor::reuse_address(true));
  acceptor_.bind(eps.begin()->endpoint());
  acceptor_.listen();
  AIOS_LOG_INFO("S3 API listening on ", cfg_.s3_listen, " volume=", cfg_.s3_volume,
                " (posix via ", posix_endpoint_, ")",
                cuobject_ && cuobject_->available()
                    ? (cfg_.cuobject_listen.empty() ? " cuobject=on"
                                                   : (" cuobject=" + cfg_.cuobject_listen))
                    : " cuobject=off");
  do_accept();
}

void S3Server::do_accept() {
  auto sock = std::make_shared<tcp::socket>(ioc_);
  acceptor_.async_accept(*sock, [this, sock](const boost::system::error_code& ec) {
    if (!ec) {
      // Run off the io_context thread: handle_session blocks in libaios_posix, which
      // performs synchronous HTTP back to http_listen on the same ioc.
      std::thread([this, sock] { handle_session(sock); }).detach();
    }
    if (acceptor_.is_open()) do_accept();
  });
}

void S3Server::handle_session(std::shared_ptr<tcp::socket> sock) {
  try {
    boost::asio::streambuf buf;
    boost::system::error_code ec;
    boost::asio::read_until(*sock, buf, "\r\n\r\n", ec);
    if (ec) return;

    std::istream is(&buf);
    std::string req_line;
    std::getline(is, req_line);
    if (!req_line.empty() && req_line.back() == '\r') req_line.pop_back();
    std::string method, target, version;
    {
      std::istringstream ls(req_line);
      ls >> method >> target >> version;
    }
    std::unordered_map<std::string, std::string> headers;
    std::string line;
    while (std::getline(is, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) break;
      auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      auto name = lower(line.substr(0, colon));
      auto val = line.substr(colon + 1);
      while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front())))
        val.erase(val.begin());
      headers[name] = val;
    }

    std::string path = target, query;
    auto qpos = target.find('?');
    if (qpos != std::string::npos) {
      path = target.substr(0, qpos);
      query = target.substr(qpos + 1);
    }
    path = url_decode(path);
    auto qmap = parse_query(query);

    // Read body. RDMA PUT uses Content-Length as object size with an empty TCP body.
    const std::string rdma_token = header_get(headers, kAmzRdmaToken);
    std::size_t content_len = 0;
    if (auto cl = header_get(headers, "content-length"); !cl.empty()) {
      content_len = static_cast<std::size_t>(std::stoull(cl));
    }
    const bool rdma_put =
        method == "PUT" && !rdma_token.empty() && qmap.find("uploadId") == qmap.end();
    std::size_t rdma_object_size = 0;
    std::string body;
    if (rdma_put) {
      rdma_object_size = content_len;
      // Do not wait for TCP payload bytes.
    } else {
      body.resize(content_len);
      std::size_t already = buf.size();
      std::size_t got = 0;
      if (already && content_len) {
        got = std::min(already, content_len);
        is.read(body.data(), static_cast<std::streamsize>(got));
      }
      while (got < content_len) {
        auto n = sock->read_some(boost::asio::buffer(body.data() + got, content_len - got), ec);
        if (ec) break;
        got += n;
      }
      body.resize(got);
    }

    std::string payload_hash = header_get(headers, "x-amz-content-sha256");
    if (payload_hash.empty()) {
      payload_hash = sha256_hex(body);
      headers["x-amz-content-sha256"] = payload_hash;
    }

    // Canonical URI: encode path but keep slashes (S3 style).
    std::string canon_uri = path.empty() ? "/" : path;
    if (canon_uri[0] != '/') canon_uri.insert(canon_uri.begin(), '/');
    // Encode each segment
    {
      std::string encoded = "/";
      auto parts = split_key(canon_uri.substr(1));
      // preserve trailing slash meaning empty last? for path /bucket/key
      bool abs = canon_uri.size() > 1;
      (void)abs;
      std::ostringstream pe;
      pe << '/';
      // Re-split keeping empties for leading
      std::vector<std::string> segs;
      std::string cur;
      for (std::size_t i = 1; i < canon_uri.size(); ++i) {
        if (canon_uri[i] == '/') {
          segs.push_back(cur);
          cur.clear();
        } else
          cur.push_back(canon_uri[i]);
      }
      segs.push_back(cur);
      for (std::size_t i = 0; i < segs.size(); ++i) {
        if (i) pe << '/';
        pe << s3_uri_encode(segs[i], true);
      }
      if (canon_uri.size() > 1 && canon_uri.back() == '/') pe << '/';
      canon_uri = pe.str();
      if (canon_uri.empty()) canon_uri = "/";
    }

    const std::string akid = s3_sigv4_access_key(headers);
    bool is_root = false;
    std::string secret;
    std::optional<S3Credential> iam_cred;
    if (!akid.empty() && akid == cfg_.s3_access_key) {
      is_root = true;
      secret = cfg_.cluster_key;
    } else if (iam_ && !akid.empty()) {
      iam_cred = iam_->find(akid);
      if (iam_cred) secret = iam_cred->secret;
    }
    if (secret.empty()) {
      write_s3_error(*sock, 403, "InvalidAccessKeyId", "Unknown access key", path);
      return;
    }
    auto auth = s3_sigv4_verify(method, canon_uri, canonical_query_string(query), headers,
                                payload_hash, akid, secret, cfg_.auth_skew_ms);
    if (!auth.ok) {
      write_s3_error(*sock, 403, "SignatureDoesNotMatch", auth.error, path);
      return;
    }
    const bool set_owner = !is_root && iam_cred.has_value();
    const uint32_t own_uid = set_owner ? iam_cred->uid : 0;
    const uint32_t own_gid = set_owner ? iam_cred->gid : 0;
    auto allow_bucket = [&](const std::string& b) -> bool {
      if (is_root) return true;
      return iam_cred && iam_->allows_bucket(*iam_cred, b);
    };

    // Parse /bucket/key
    std::string bucket, key;
    {
      auto p = path;
      if (!p.empty() && p[0] == '/') p.erase(p.begin());
      auto slash = p.find('/');
      if (slash == std::string::npos) {
        bucket = p;
        key.clear();
      } else {
        bucket = p.substr(0, slash);
        key = p.substr(slash + 1);
      }
    }

    // ----- ListBuckets -----
    if (method == "GET" && bucket.empty()) {
      std::ostringstream xml;
      xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
          << "<ListAllMyBucketsResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
          << "<Owner><ID>" << xml_escape(akid)
          << "</ID><DisplayName>" << xml_escape(akid) << "</DisplayName></Owner><Buckets>";
      uint64_t off = 0;
      aios_posix_dirent ents[64];
      for (;;) {
        int n = aios_posix_readdir(fs_, kRootIno, &off, ents, 64);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
          if (ents[i].name[0] == '.') continue;
          if (!S_ISDIR(ents[i].mode)) continue;
          if (!allow_bucket(ents[i].name)) continue;
          aios_posix_stat st{};
          aios_posix_getattr(fs_, ents[i].ino, &st);
          xml << "<Bucket><Name>" << xml_escape(ents[i].name) << "</Name><CreationDate>"
              << iso8601_from_ns(st.ctime_ns) << "</CreationDate></Bucket>";
        }
      }
      xml << "</Buckets></ListAllMyBucketsResult>";
      write_http(*sock, 200, "OK", {{"Content-Type", "application/xml"}}, xml.str());
      return;
    }

    if (bucket.empty()) {
      write_s3_error(*sock, 400, "InvalidRequest", "bucket required", path);
      return;
    }
    if (!allow_bucket(bucket)) {
      write_s3_error(*sock, 403, "AccessDenied", "bucket not allowed for this access key", path);
      return;
    }

    // ----- CreateBucket -----
    if (method == "PUT" && key.empty() && qmap.count("uploads") == 0) {
      if (!valid_bucket_name(bucket)) {
        write_s3_error(*sock, 400, "InvalidBucketName", "invalid bucket name", path);
        return;
      }
      aios_posix_stat st{};
      int err = aios_posix_lookup(fs_, kRootIno, bucket.c_str(), &st);
      if (err == 0) {
        write_s3_error(*sock, 409, "BucketAlreadyOwnedByYou", "bucket exists", path);
        return;
      }
      err = aios_posix_mkdir(fs_, kRootIno, bucket.c_str(), 0755, &st);
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "mkdir failed", path);
        return;
      }
      apply_owner(fs_, st.ino, set_owner, own_uid, own_gid);
      write_http(*sock, 200, "OK", {{"Location", "/" + bucket}}, {});
      return;
    }

    // ----- HeadBucket / DeleteBucket -----
    if (key.empty() && (method == "HEAD" || method == "DELETE")) {
      aios_posix_stat st{};
      int err = aios_posix_lookup(fs_, kRootIno, bucket.c_str(), &st);
      if (err || !S_ISDIR(st.mode)) {
        write_s3_error(*sock, 404, "NoSuchBucket", "The specified bucket does not exist", path);
        return;
      }
      if (method == "HEAD") {
        write_http(*sock, 200, "OK", {}, {});
        return;
      }
      if (!dir_empty(fs_, st.ino)) {
        write_s3_error(*sock, 409, "BucketNotEmpty", "The bucket you tried to delete is not empty",
                       path);
        return;
      }
      err = aios_posix_rmdir(fs_, kRootIno, bucket.c_str());
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "rmdir failed", path);
        return;
      }
      write_http(*sock, 204, "No Content", {}, {});
      return;
    }

    // ----- ListObjectsV2 -----
    if (method == "GET" && key.empty() &&
        (qmap.count("list-type") || qmap.count("prefix") || qmap.count("delimiter") ||
         qmap.empty() || qmap.count("max-keys"))) {
      aios_posix_stat bst{};
      int err = aios_posix_lookup(fs_, kRootIno, bucket.c_str(), &bst);
      if (err || !S_ISDIR(bst.mode)) {
        write_s3_error(*sock, 404, "NoSuchBucket", "The specified bucket does not exist", path);
        return;
      }
      std::string prefix = qmap.count("prefix") ? qmap["prefix"] : "";
      std::string delimiter = qmap.count("delimiter") ? qmap["delimiter"] : "";
      int max_keys = 1000;
      if (qmap.count("max-keys")) {
        try {
          max_keys = std::stoi(qmap["max-keys"]);
        } catch (...) {
        }
        if (max_keys < 1) max_keys = 1;
        if (max_keys > 1000) max_keys = 1000;
      }
      // Start listing from bucket root or prefix directory.
      uint64_t start_ino = bst.ino;
      std::string start_path;
      if (!prefix.empty()) {
        auto pp = split_key(prefix);
        if (!prefix.empty() && prefix.back() == '/' && !pp.empty()) {
          // prefix is a directory path
          std::vector<std::string> full{bucket};
          full.insert(full.end(), pp.begin(), pp.end());
          aios_posix_stat st{};
          if (lookup_path(fs_, full, &start_ino, &st) == 0 && S_ISDIR(st.mode)) {
            start_path = prefix;
            if (!start_path.empty() && start_path.back() == '/') start_path.pop_back();
          }
        }
      }
      std::vector<ListEntry> keys;
      std::vector<std::string> common;
      collect_list(fs_, start_ino, start_path, prefix, delimiter, keys, common, max_keys);

      std::ostringstream xml;
      xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
          << "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
          << "<Name>" << xml_escape(bucket) << "</Name>"
          << "<Prefix>" << xml_escape(prefix) << "</Prefix>"
          << "<MaxKeys>" << max_keys << "</MaxKeys>"
          << "<IsTruncated>false</IsTruncated>"
          << "<KeyCount>" << (keys.size() + common.size()) << "</KeyCount>";
      if (!delimiter.empty()) xml << "<Delimiter>" << xml_escape(delimiter) << "</Delimiter>";
      for (const auto& e : keys) {
        xml << "<Contents><Key>" << xml_escape(e.key) << "</Key><LastModified>"
            << iso8601_from_ns(e.mtime_ns) << "</LastModified><ETag>&quot;size-" << e.size
            << "&quot;</ETag><Size>" << e.size
            << "</Size><StorageClass>STANDARD</StorageClass></Contents>";
      }
      for (const auto& cp : common) {
        xml << "<CommonPrefixes><Prefix>" << xml_escape(cp) << "</Prefix></CommonPrefixes>";
      }
      xml << "</ListBucketResult>";
      write_http(*sock, 200, "OK", {{"Content-Type", "application/xml"}}, xml.str());
      return;
    }

    // Need object key for remaining ops
    if (key.empty()) {
      write_s3_error(*sock, 400, "InvalidRequest", "object key required", path);
      return;
    }

    // Folder marker: trailing slash Put → mkdir
    if (method == "PUT" && !key.empty() && key.back() == '/' && qmap.count("uploads") == 0 &&
        header_get(headers, "x-amz-copy-source").empty()) {
      auto parts = split_key(bucket + "/" + key);
      int err = mkdir_p(fs_, parts, nullptr, set_owner, own_uid, own_gid);
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "mkdir failed", path);
        return;
      }
      write_http(*sock, 200, "OK", {}, {});
      return;
    }

    // ----- CreateMultipartUpload -----
    if (method == "POST" && qmap.count("uploads")) {
      aios_posix_stat bst{};
      if (aios_posix_lookup(fs_, kRootIno, bucket.c_str(), &bst) || !S_ISDIR(bst.mode)) {
        write_s3_error(*sock, 404, "NoSuchBucket", "The specified bucket does not exist", path);
        return;
      }
      std::string upload_id = random_upload_id();
      aios_posix_stat mst{};
      aios_posix_lookup(fs_, kRootIno, kMultipartDir, &mst);
      aios_posix_stat ust{};
      int err = aios_posix_mkdir(fs_, mst.ino, upload_id.c_str(), 0700, &ust);
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "multipart init failed", path);
        return;
      }
      // Store target key as xattr on upload dir
      aios_posix_setxattr(fs_, ust.ino, "user.aios.s3.upload.bucket", bucket.data(), bucket.size(),
                          0);
      aios_posix_setxattr(fs_, ust.ino, "user.aios.s3.upload.key", key.data(), key.size(), 0);
      std::ostringstream xml;
      xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
          << "<InitiateMultipartUploadResult>"
          << "<Bucket>" << xml_escape(bucket) << "</Bucket><Key>" << xml_escape(key)
          << "</Key><UploadId>" << xml_escape(upload_id) << "</UploadId>"
          << "</InitiateMultipartUploadResult>";
      write_http(*sock, 200, "OK", {{"Content-Type", "application/xml"}}, xml.str());
      return;
    }

    // ----- UploadPart -----
    if (method == "PUT" && qmap.count("uploadId") && qmap.count("partNumber")) {
      std::string upload_id = qmap["uploadId"];
      std::string part = qmap["partNumber"];
      aios_posix_stat mst{}, ust{};
      if (aios_posix_lookup(fs_, kRootIno, kMultipartDir, &mst) ||
          aios_posix_lookup(fs_, mst.ino, upload_id.c_str(), &ust)) {
        write_s3_error(*sock, 404, "NoSuchUpload", "upload not found", path);
        return;
      }
      uint64_t fino = 0;
      int err = ensure_file(fs_, ust.ino, part.c_str(), &fino);
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "part create failed", path);
        return;
      }
      aios_posix_truncate(fs_, fino, 0);
      size_t wrote = 0;
      err = aios_posix_write(fs_, fino, 0, body.data(), body.size(), &wrote);
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "part write failed", path);
        return;
      }
      auto etag = md5_hex(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
      write_http(*sock, 200, "OK", {{"ETag", "\"" + etag + "\""}}, {});
      return;
    }

    // ----- CompleteMultipartUpload -----
    if (method == "POST" && qmap.count("uploadId") && !qmap.count("uploads")) {
      std::string upload_id = qmap["uploadId"];
      aios_posix_stat mst{}, ust{};
      if (aios_posix_lookup(fs_, kRootIno, kMultipartDir, &mst) ||
          aios_posix_lookup(fs_, mst.ino, upload_id.c_str(), &ust)) {
        write_s3_error(*sock, 404, "NoSuchUpload", "upload not found", path);
        return;
      }
      char bbuf[256]{}, kbuf[1024]{};
      int blen = aios_posix_getxattr(fs_, ust.ino, "user.aios.s3.upload.bucket", bbuf, sizeof(bbuf));
      int klen = aios_posix_getxattr(fs_, ust.ino, "user.aios.s3.upload.key", kbuf, sizeof(kbuf));
      if (blen < 0 || klen < 0) {
        write_s3_error(*sock, 500, "InternalError", "upload meta missing", path);
        return;
      }
      std::string tbucket(bbuf, blen), tkey(kbuf, klen);
      // Collect part files sorted by name (part numbers)
      std::vector<std::pair<int, uint64_t>> parts;
      uint64_t off = 0;
      aios_posix_dirent ents[64];
      for (;;) {
        int n = aios_posix_readdir(fs_, ust.ino, &off, ents, 64);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
          if (ents[i].name[0] == '.') continue;
          try {
            parts.push_back({std::stoi(ents[i].name), ents[i].ino});
          } catch (...) {
          }
        }
      }
      std::sort(parts.begin(), parts.end());
      uint64_t parent = 0;
      std::string name;
      int err =
          resolve_parent(fs_, tbucket, tkey, true, &parent, &name, nullptr, set_owner, own_uid,
                         own_gid);
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "resolve target failed", path);
        return;
      }
      uint64_t fino = 0;
      err = ensure_file(fs_, parent, name, &fino, set_owner, own_uid, own_gid);
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "create target failed", path);
        return;
      }
      aios_posix_truncate(fs_, fino, 0);
      uint64_t woff = 0;
      for (const auto& [pn, pino] : parts) {
        (void)pn;
        aios_posix_stat pst{};
        aios_posix_getattr(fs_, pino, &pst);
        std::vector<char> chunk(static_cast<std::size_t>(std::min<uint64_t>(pst.size, 1 << 20)));
        uint64_t roff = 0;
        while (roff < pst.size) {
          size_t got = 0;
          size_t want = static_cast<size_t>(std::min<uint64_t>(chunk.size(), pst.size - roff));
          err = aios_posix_read(fs_, pino, roff, chunk.data(), want, &got);
          if (err) break;
          size_t wrote = 0;
          err = aios_posix_write(fs_, fino, woff, chunk.data(), got, &wrote);
          if (err) break;
          roff += got;
          woff += wrote;
        }
        if (err) break;
      }
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "assemble failed", path);
        return;
      }
      // Cleanup multipart dir
      off = 0;
      for (;;) {
        int n = aios_posix_readdir(fs_, ust.ino, &off, ents, 64);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
          if (ents[i].name[0] == '.') continue;
          aios_posix_unlink(fs_, ust.ino, ents[i].name);
        }
      }
      aios_posix_rmdir(fs_, mst.ino, upload_id.c_str());
      std::ostringstream xml;
      xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
          << "<CompleteMultipartUploadResult><Bucket>" << xml_escape(tbucket) << "</Bucket><Key>"
          << xml_escape(tkey) << "</Key><ETag>&quot;multipart-" << woff
          << "&quot;</ETag></CompleteMultipartUploadResult>";
      write_http(*sock, 200, "OK", {{"Content-Type", "application/xml"}}, xml.str());
      return;
    }

    // ----- AbortMultipartUpload -----
    if (method == "DELETE" && qmap.count("uploadId")) {
      std::string upload_id = qmap["uploadId"];
      aios_posix_stat mst{}, ust{};
      if (aios_posix_lookup(fs_, kRootIno, kMultipartDir, &mst) == 0 &&
          aios_posix_lookup(fs_, mst.ino, upload_id.c_str(), &ust) == 0) {
        uint64_t off = 0;
        aios_posix_dirent ents[64];
        for (;;) {
          int n = aios_posix_readdir(fs_, ust.ino, &off, ents, 64);
          if (n <= 0) break;
          for (int i = 0; i < n; ++i) {
            if (ents[i].name[0] == '.') continue;
            aios_posix_unlink(fs_, ust.ino, ents[i].name);
          }
        }
        aios_posix_rmdir(fs_, mst.ino, upload_id.c_str());
      }
      write_http(*sock, 204, "No Content", {}, {});
      return;
    }

    // ----- CopyObject -----
    if (method == "PUT" && !header_get(headers, "x-amz-copy-source").empty()) {
      auto src = url_decode(header_get(headers, "x-amz-copy-source"));
      if (!src.empty() && src[0] == '/') src.erase(src.begin());
      auto slash = src.find('/');
      if (slash == std::string::npos) {
        write_s3_error(*sock, 400, "InvalidArgument", "bad copy source", path);
        return;
      }
      std::string sbucket = src.substr(0, slash), skey = src.substr(slash + 1);
      if (!allow_bucket(sbucket)) {
        write_s3_error(*sock, 403, "AccessDenied", "source bucket not allowed", path);
        return;
      }
      uint64_t sp = 0;
      std::string sname;
      int err = resolve_parent(fs_, sbucket, skey, false, &sp, &sname);
      if (err) {
        write_s3_error(*sock, 404, "NoSuchKey", "source not found", path);
        return;
      }
      aios_posix_stat sst{};
      err = aios_posix_lookup(fs_, sp, sname.c_str(), &sst);
      if (err || !S_ISREG(sst.mode)) {
        write_s3_error(*sock, 404, "NoSuchKey", "source not found", path);
        return;
      }
      uint64_t dp = 0;
      std::string dname;
      err = resolve_parent(fs_, bucket, key, true, &dp, &dname, nullptr, set_owner, own_uid,
                          own_gid);
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "dest resolve failed", path);
        return;
      }
      uint64_t dino = 0;
      err = ensure_file(fs_, dp, dname, &dino, set_owner, own_uid, own_gid);
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "dest create failed", path);
        return;
      }
      aios_posix_truncate(fs_, dino, 0);
      std::vector<char> chunk(1 << 20);
      uint64_t roff = 0, woff = 0;
      while (roff < sst.size) {
        size_t got = 0;
        size_t want = static_cast<size_t>(std::min<uint64_t>(chunk.size(), sst.size - roff));
        err = aios_posix_read(fs_, sst.ino, roff, chunk.data(), want, &got);
        if (err) break;
        size_t wrote = 0;
        err = aios_posix_write(fs_, dino, woff, chunk.data(), got, &wrote);
        if (err) break;
        roff += got;
        woff += wrote;
      }
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "copy failed", path);
        return;
      }
      std::ostringstream xml;
      xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
          << "<CopyObjectResult><LastModified>" << iso8601_from_ns(sst.mtime_ns)
          << "</LastModified><ETag>&quot;copy-" << woff << "&quot;</ETag></CopyObjectResult>";
      write_http(*sock, 200, "OK", {{"Content-Type", "application/xml"}}, xml.str());
      return;
    }

    // ----- PutObject -----
    if (method == "PUT") {
      aios_posix_stat bst{};
      if (aios_posix_lookup(fs_, kRootIno, bucket.c_str(), &bst) || !S_ISDIR(bst.mode)) {
        write_s3_error(*sock, 404, "NoSuchBucket", "The specified bucket does not exist", path);
        return;
      }
      uint64_t parent = 0;
      std::string name;
      int err = resolve_parent(fs_, bucket, key, true, &parent, &name, nullptr, set_owner, own_uid,
                              own_gid);
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "path resolve failed", path);
        return;
      }
      uint64_t ino = 0;
      err = ensure_file(fs_, parent, name, &ino, set_owner, own_uid, own_gid);
      if (err) {
        write_s3_error(*sock, 500, "InternalError", "create failed", path);
        return;
      }

      std::string put_data;
      std::string rdma_reply;
      if (rdma_put) {
        put_data.resize(rdma_object_size);
        std::string rdma_err;
        if (!s3_try_rdma_put(cuobject_.get(), bucket + "/" + key, rdma_token, put_data.data(),
                             put_data.size(), rdma_reply, rdma_err)) {
          int code = 500;
          const char* ename = "InternalError";
          if (!cuobject_ || !cuobject_->available()) {
            code = 501;
            ename = "NotImplemented";
          } else if (rdma_object_size == 0 || rdma_object_size > kCuObjectMaxTransferBytes) {
            code = 400;
            ename = "InvalidArgument";
          }
          write_s3_error(*sock, code, ename,
                         "RDMA PUT failed: " + rdma_err + "; retry without " +
                             std::string(kAmzRdmaToken),
                         path);
          return;
        }
      } else {
        put_data = std::move(body);
      }

      aios_posix_truncate(fs_, ino, 0);
      size_t wrote = 0;
      err = aios_posix_write(fs_, ino, 0, put_data.data(), put_data.size(), &wrote);
      if (err) {
        if (!write_posix_err(*sock, err, path))
          write_s3_error(*sock, 500, "InternalError", "write failed", path);
        return;
      }
      if (auto ct = header_get(headers, "content-type"); !ct.empty()) {
        aios_posix_setxattr(fs_, ino, kXattrContentType, ct.data(), ct.size(), 0);
      }
      for (const auto& [hk, hv] : headers) {
        if (hk.rfind("x-amz-meta-", 0) == 0) {
          std::string xn = std::string(kXattrMetaPrefix) + hk.substr(11);
          aios_posix_setxattr(fs_, ino, xn.c_str(), hv.data(), hv.size(), 0);
        }
      }
      auto etag = md5_hex(reinterpret_cast<const std::uint8_t*>(put_data.data()), put_data.size());
      std::unordered_map<std::string, std::string> rh{{"ETag", "\"" + etag + "\""}};
      if (!rdma_reply.empty()) rh[kAmzRdmaReply] = rdma_reply;
      write_http(*sock, 200, "OK", rh, {});
      return;
    }

    // ----- GetObject / HeadObject -----
    if (method == "GET" || method == "HEAD") {
      uint64_t parent = 0;
      std::string name;
      int err = resolve_parent(fs_, bucket, key, false, &parent, &name);
      if (err) {
        write_s3_error(*sock, 404, "NoSuchKey", "The specified key does not exist", path);
        return;
      }
      aios_posix_stat st{};
      err = aios_posix_lookup(fs_, parent, name.c_str(), &st);
      if (err || !S_ISREG(st.mode)) {
        write_s3_error(*sock, 404, "NoSuchKey", "The specified key does not exist", path);
        return;
      }
      uint64_t start = 0, end = st.size ? st.size - 1 : 0;
      bool ranged = false;
      if (auto rg = header_get(headers, "range"); !rg.empty() && method == "GET") {
        // bytes=START-END
        if (rg.rfind("bytes=", 0) == 0) {
          auto rest = rg.substr(6);
          auto dash = rest.find('-');
          if (dash != std::string::npos) {
            try {
              start = rest.substr(0, dash).empty() ? 0 : std::stoull(rest.substr(0, dash));
              if (dash + 1 < rest.size()) end = std::stoull(rest.substr(dash + 1));
              else end = st.size ? st.size - 1 : 0;
              ranged = true;
            } catch (...) {
            }
          }
        }
      }
      if (st.size == 0) {
        start = 0;
        end = 0;
      } else if (start >= st.size) {
        write_s3_error(*sock, 416, "InvalidRange", "Requested range not satisfiable", path);
        return;
      }
      if (end >= st.size) end = st.size - 1;
      uint64_t len = st.size == 0 ? 0 : (end - start + 1);

      std::unordered_map<std::string, std::string> rh = {
          {"Content-Type", "application/octet-stream"},
          {"Accept-Ranges", "bytes"},
          {"ETag", "\"size-" + std::to_string(st.size) + "\""},
          {"Last-Modified", iso8601_from_ns(st.mtime_ns)},
      };
      char ctbuf[256];
      int ctl = aios_posix_getxattr(fs_, st.ino, kXattrContentType, ctbuf, sizeof(ctbuf));
      if (ctl > 0) rh["Content-Type"] = std::string(ctbuf, ctl);

      if (method == "HEAD") {
        rh["Content-Length"] = std::to_string(st.size);
        write_http(*sock, 200, "OK", rh, {});
        return;
      }

      std::string out;
      out.resize(static_cast<std::size_t>(len));
      if (len) {
        size_t got = 0;
        err = aios_posix_read(fs_, st.ino, start, out.data(), static_cast<size_t>(len), &got);
        if (err) {
          write_s3_error(*sock, 500, "InternalError", "read failed", path);
          return;
        }
        out.resize(got);
      }

      // Whole-object RDMA GET (no Range): push host buffer to client GPU/system memory.
      if (s3_want_rdma_get(rdma_token, ranged, method == "GET")) {
        std::string rdma_reply;
        std::string rdma_err;
        if (s3_try_rdma_get(cuobject_.get(), bucket + "/" + key, rdma_token, out.data(), out.size(),
                            rdma_reply, rdma_err)) {
          rh["Content-Length"] = std::to_string(out.size());
          rh[kAmzRdmaReply] = rdma_reply;
          write_http(*sock, 200, "OK", rh, {});
          return;
        }
        AIOS_LOG_WARN("S3 RDMA GET failed (", rdma_err, "); falling back to TCP body");
      }

      if (ranged) {
        rh["Content-Range"] = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                              std::to_string(st.size);
        write_http(*sock, 206, "Partial Content", rh, out);
      } else {
        write_http(*sock, 200, "OK", rh, out);
      }
      return;
    }

    // ----- DeleteObject -----
    if (method == "DELETE") {
      uint64_t parent = 0;
      std::string name;
      int err = resolve_parent(fs_, bucket, key, false, &parent, &name);
      if (err) {
        // S3 delete is idempotent
        write_http(*sock, 204, "No Content", {}, {});
        return;
      }
      aios_posix_unlink(fs_, parent, name.c_str());
      write_http(*sock, 204, "No Content", {}, {});
      return;
    }

    write_s3_error(*sock, 405, "MethodNotAllowed", "method not supported", path);
  } catch (const std::exception& e) {
    AIOS_LOG_WARN("S3 session error: ", e.what());
    try {
      write_s3_error(*sock, 500, "InternalError", e.what());
    } catch (...) {
    }
  }
}

}  // namespace aios
