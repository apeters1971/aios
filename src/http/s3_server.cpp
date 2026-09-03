#include "http/s3_server.hpp"

#include "cuobject/cuobject_endpoint.hpp"
#include "cuobject/cuobject_s3_xfer.hpp"
#include "http/s3_auth.hpp"
#include "http/s3_range.hpp"
#include "http/sock_io.hpp"
#include "http/tls_stream.hpp"
#include "posix/aios_posix.h"
#include "util/auth.hpp"
#include "util/log.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <errno.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <future>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aios {
namespace {

using tcp = boost::asio::ip::tcp;

constexpr uint64_t kRootIno = 1;
// Request headers arrive before any authentication, so they need a hard ceiling.
constexpr std::size_t kMaxRequestHeaderBytes = 64u * 1024u;
// One thread per live session; beyond this new connections get 503 SlowDown.
constexpr int kMaxSessions = 256;
constexpr const char* kMultipartDir = ".s3multipart";
constexpr const char* kXattrContentType = "user.aios.s3.content-type";
constexpr const char* kXattrMetaPrefix = "user.aios.s3.meta.";
constexpr const char* kXattrUploadBucket = "user.aios.s3.upload.bucket";
constexpr const char* kXattrUploadKey = "user.aios.s3.upload.key";
constexpr const char* kXattrUploadOwner = "user.aios.s3.upload.owner";
constexpr const char* kUnsignedPayload = "UNSIGNED-PAYLOAD";
constexpr const char* kStreamingPayload = "STREAMING-AWS4-HMAC-SHA256-PAYLOAD";

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

// Percent-decoding only for paths ('+' is a literal key byte); query strings
// additionally read '+' as a space.
std::string url_decode(const std::string& in, bool plus_is_space = false) {
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
    if (plus_is_space && in[i] == '+') out.push_back(' ');
    else out.push_back(in[i]);
  }
  return out;
}

// Decoded (key, value) pairs in wire order; duplicates preserved.
std::vector<std::pair<std::string, std::string>> parse_query_pairs(const std::string& q) {
  std::vector<std::pair<std::string, std::string>> out;
  std::size_t i = 0;
  while (i < q.size()) {
    auto amp = q.find('&', i);
    if (amp == std::string::npos) amp = q.size();
    auto eq = q.find('=', i);
    if (eq != std::string::npos && eq < amp) {
      out.emplace_back(url_decode(q.substr(i, eq - i), true),
                       url_decode(q.substr(eq + 1, amp - eq - 1), true));
    } else if (amp > i) {
      out.emplace_back(url_decode(q.substr(i, amp - i), true), "");
    }
    i = amp + 1;
  }
  return out;
}

std::unordered_map<std::string, std::string> parse_query(const std::string& q) {
  std::unordered_map<std::string, std::string> out;
  for (auto& [k, v] : parse_query_pairs(q)) out[k] = v;
  return out;
}

// SigV4 canonical query: every (key, value) pair URI-encoded, stable-sorted by
// encoded key then encoded value, joined with &. Duplicate keys stay distinct.
std::string canonical_query_string(const std::string& raw_query) {
  if (raw_query.empty()) return {};
  std::vector<std::pair<std::string, std::string>> items;
  for (const auto& [k, v] : parse_query_pairs(raw_query)) {
    items.emplace_back(s3_uri_encode(k, true), s3_uri_encode(v, true));
  }
  std::stable_sort(items.begin(), items.end());
  std::ostringstream oss;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) oss << '&';
    oss << items[i].first << '=' << items[i].second;
  }
  return oss.str();
}

bool is_hex_sha256(const std::string& s) {
  if (s.size() != 64) return false;
  return std::all_of(s.begin(), s.end(),
                     [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// Reads the header block (through the blank line) with raw recv so SO_RCVTIMEO
// applies. Bytes past the blank line are returned in `rest`.
enum class HeaderRead { Ok, TooLarge, Closed };
HeaderRead read_request_head(TlsStream& conn, std::string& head, std::string& rest) {
  std::string buf;
  char chunk[4096];
  while (true) {
    int err = 0;
    const long n = conn.read_some(chunk, sizeof(chunk), err);
    if (n <= 0) return HeaderRead::Closed;
    buf.append(chunk, static_cast<std::size_t>(n));
    const auto pos = buf.find("\r\n\r\n");
    if (pos != std::string::npos) {
      head = buf.substr(0, pos + 4);
      rest = buf.substr(pos + 4);
      return HeaderRead::Ok;
    }
    if (buf.size() > kMaxRequestHeaderBytes) return HeaderRead::TooLarge;
  }
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

void write_http(TlsStream& sock, int status, const std::string& reason,
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
  auto head = oss.str();
  int err = 0;
  if (!sock.write_all(head.data(), head.size(), err)) return;
  if (!body.empty()) sock.write_all(body.data(), body.size(), err);
}

void write_s3_error(TlsStream& sock, int status, const std::string& code,
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

bool write_posix_err(TlsStream& sock, int err, const std::string& path) {
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
    // S3 promises the key is listable everywhere once we answer 200: commit the
    // dentry queued under the directory lease before returning.
    if (int se = aios_posix_fsyncdir(fs, parent)) return se;
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

// Upload ids are bearer capabilities until the owner check below, so they come
// from the CSPRNG rather than a seeded mt19937.
std::string random_upload_id() {
  unsigned char raw[16];
  if (RAND_bytes(raw, sizeof(raw)) != 1) return {};
  static const char* hexd = "0123456789abcdef";
  std::string out(sizeof(raw) * 2, '\0');
  for (std::size_t i = 0; i < sizeof(raw); ++i) {
    out[i * 2] = hexd[raw[i] >> 4];
    out[i * 2 + 1] = hexd[raw[i] & 0xf];
  }
  return out;
}

std::string get_xattr_string(aios_posix_fs* fs, uint64_t ino, const char* name) {
  char buf[1024];
  const int n = aios_posix_getxattr(fs, ino, name, buf, sizeof(buf));
  if (n < 0) return {};
  return std::string(buf, static_cast<std::size_t>(n));
}

// Multipart uploads belong to the access key that created them.
bool upload_owned_by(aios_posix_fs* fs, uint64_t upload_ino, const std::string& akid) {
  const auto owner = get_xattr_string(fs, upload_ino, kXattrUploadOwner);
  return !owner.empty() && owner == akid;
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

S3Server::~S3Server() { stop(); }

void S3Server::stop() {
  bool expected = false;
  if (!stopping_.compare_exchange_strong(expected, true)) {
    std::unique_lock lock(stop_mu_);
    stop_cv_.wait(lock, [this] { return sessions_.load() == 0 && fs_ == nullptr; });
    return;
  }

  auto close_acceptor = [this] {
    boost::system::error_code ec;
    acceptor_.close(ec);
  };

  // Cancel accept on the ioc thread, then drain so the completion handler cannot
  // observe a destroyed S3Server (it captures this). When start() never reached
  // listen() (bad TLS files, bad address) there is no handler to drain, and the
  // ioc may never have run, so a posted round trip would wait forever.
  if (!acceptor_.is_open()) {
    // nothing to cancel
  } else if (!ioc_.stopped()) {
    std::promise<void> drained;
    auto fut = drained.get_future();
    boost::asio::post(ioc_, [this, close_acceptor, &drained] {
      close_acceptor();
      boost::asio::post(ioc_, [&drained] { drained.set_value(); });
    });
    fut.wait();
  } else {
    close_acceptor();
  }

  {
    std::unique_lock lock(stop_mu_);
    stop_cv_.wait(lock, [this] { return sessions_.load() == 0; });
  }

  if (fs_) {
    aios_posix_unmount(fs_);
    fs_ = nullptr;
  }
  stop_cv_.notify_all();
}

void S3Server::start() {
  if (!cfg_.s3_tls_cert.empty() || !cfg_.s3_tls_key.empty()) {
    if (cfg_.s3_tls_cert.empty() || cfg_.s3_tls_key.empty()) {
      throw std::runtime_error("S3: s3_tls_cert and s3_tls_key must be set together");
    }
    std::string terr;
    tls_ = TlsServerContext::load(cfg_.s3_tls_cert, cfg_.s3_tls_key, cfg_.s3_tls_chain, terr);
    if (!tls_) throw std::runtime_error("S3 TLS: " + terr);
  }

  aios_posix_config pcfg{};
  pcfg.endpoint = posix_endpoint_.c_str();
  pcfg.cluster_key = cfg_.cluster_key.c_str();
  pcfg.volume = cfg_.s3_volume.c_str();
  pcfg.app_label = "s3";
  pcfg.rstat_interval_ms = 60000;
  int err = 0;
  fs_ = aios_posix_mount(&pcfg, &err);
  if (!fs_) {
    throw std::runtime_error("S3: aios_posix_mount failed: " + std::to_string(err));
  }
  // Shared volume root: sticky + world-writable so IAM callers can CreateBucket
  // while only owners (or root) may remove others' buckets.
  {
    aios_posix_stat rst{};
    if (aios_posix_getattr(fs_, kRootIno, &rst) == 0) {
      rst.mode = (rst.mode & S_IFMT) | 01777;
      aios_posix_setattr(fs_, kRootIno, &rst, AIOS_POSIX_SET_MODE);
    }
  }
  // Multipart staging: world-accessible so IAM principals can upload parts.
  aios_posix_stat st{};
  if (aios_posix_lookup(fs_, kRootIno, kMultipartDir, &st) == -ENOENT) {
    aios_posix_mkdir(fs_, kRootIno, kMultipartDir, 0777, &st);
  } else {
    st.mode = (st.mode & S_IFMT) | 0777;
    aios_posix_setattr(fs_, st.ino, &st, AIOS_POSIX_SET_MODE);
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
  AIOS_LOG_INFO("S3 API listening on ", cfg_.s3_listen, tls_ ? " (https)" : " (http)",
                " volume=", cfg_.s3_volume, " (posix via ", posix_endpoint_, ")",
                cuobject_ && cuobject_->available()
                    ? (cfg_.cuobject_listen.empty() ? " cuobject=on"
                                                   : (" cuobject=" + cfg_.cuobject_listen))
                    : " cuobject=off");
  do_accept();
}

void S3Server::do_accept() {
  if (stopping_.load()) return;
  auto sock = std::make_shared<tcp::socket>(ioc_);
  acceptor_.async_accept(*sock, [this, sock](const boost::system::error_code& ec) {
    if (!ec && !stopping_.load()) {
      // Run off the io_context thread: handle_session blocks in libaios_posix, which
      // performs synchronous HTTP back to http_listen on the same ioc.
      set_fd_timeouts(static_cast<int>(sock->native_handle()), cfg_.http_idle_timeout_ms);
      const int live = sessions_.fetch_add(1) + 1;
      std::thread([this, sock, live] {
        try {
          // The handshake runs on the session thread (it blocks) and under the
          // same idle timeout as everything else on the socket.
          const int fd = static_cast<int>(sock->native_handle());
          TlsStream conn(fd, tls_);  // plain when tls_ is null
          std::string herr;
          if (!conn.accept(herr)) {
            AIOS_LOG_DEBUG("S3 TLS handshake failed: ", herr);
          } else if (live > kMaxSessions) {
            write_s3_error(conn, 503, "SlowDown", "too many concurrent connections", "/");
          } else {
            handle_session(conn);
          }
        } catch (...) {
        }
        boost::system::error_code cec;
        sock->close(cec);
        sessions_.fetch_sub(1);
        stop_cv_.notify_all();
      }).detach();
    }
    if (!stopping_.load() && acceptor_.is_open()) do_accept();
  });
}

void S3Server::handle_session(TlsStream& conn) {
  try {
    std::string head, rest;
    switch (read_request_head(conn, head, rest)) {
      case HeaderRead::Ok:
        break;
      case HeaderRead::TooLarge:
        write_s3_error(conn, 431, "RequestHeaderSectionTooLarge",
                       "Request header section too large", "/");
        return;
      case HeaderRead::Closed:
        return;
    }

    std::istringstream is(head);
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

    // RDMA PUT uses Content-Length as object size with an empty TCP body.
    const std::string rdma_token = header_get(headers, kAmzRdmaToken);
    std::size_t content_len = 0;
    if (auto cl = header_get(headers, "content-length"); !cl.empty()) {
      try {
        content_len = static_cast<std::size_t>(std::stoull(cl));
      } catch (...) {
        write_s3_error(conn, 400, "InvalidArgument", "Invalid Content-Length", path);
        return;
      }
    }
    if (content_len > cfg_.max_object_bytes) {
      write_s3_error(conn, 413, "EntityTooLarge", "Body exceeds max_object_bytes", path);
      return;
    }
    if (!header_get(headers, "transfer-encoding").empty()) {
      write_s3_error(conn, 501, "NotImplemented", "Transfer-Encoding is not supported", path);
      return;
    }
    const bool rdma_put =
        method == "PUT" && !rdma_token.empty() && qmap.find("uploadId") == qmap.end();
    std::size_t rdma_object_size = 0;
    if (rdma_put) rdma_object_size = content_len;
    const bool has_tcp_body = !rdma_put && content_len > 0;

    // The payload hash is a signed header, so the signature is checked from the
    // headers alone; the body is only read (and hashed) for a caller who proved
    // possession of the secret.
    std::string payload_hash = header_get(headers, "x-amz-content-sha256");
    if (payload_hash.empty()) {
      if (has_tcp_body) {
        write_s3_error(conn, 400, "InvalidRequest",
                       "x-amz-content-sha256 is required for requests with a body", path);
        return;
      }
      payload_hash = sha256_hex(std::string{});
      headers["x-amz-content-sha256"] = payload_hash;
    }
    const bool streaming_payload = payload_hash.rfind(kStreamingPayload, 0) == 0;
    if (!streaming_payload && payload_hash != kUnsignedPayload && !is_hex_sha256(payload_hash)) {
      write_s3_error(conn, 400, "InvalidArgument", "Invalid x-amz-content-sha256", path);
      return;
    }
    if (has_tcp_body && content_len > cfg_.s3_max_body_bytes) {
      write_s3_error(conn, 413, "EntityTooLarge",
                     "Body exceeds s3_max_body_bytes; use multipart upload", path);
      return;
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
      write_s3_error(conn, 403, "InvalidAccessKeyId", "Unknown access key", path);
      return;
    }
    auto auth = s3_sigv4_verify(method, canon_uri, canonical_query_string(query), headers,
                                payload_hash, akid, secret, cfg_.auth_skew_ms);
    if (!auth.ok) {
      write_s3_error(conn, 403, "SignatureDoesNotMatch", auth.error, path);
      return;
    }

    // Body: only now, and only up to the buffered ceiling checked above.
    std::string body;
    if (has_tcp_body) {
      if (streaming_payload) {
        write_s3_error(conn, 501, "NotImplemented",
                       "aws-chunked (STREAMING-AWS4-HMAC-SHA256-PAYLOAD) uploads are not "
                       "supported; send a concrete x-amz-content-sha256 or UNSIGNED-PAYLOAD",
                       path);
        return;
      }
      body = std::move(rest);
      if (body.size() > content_len) body.resize(content_len);
      body.reserve(content_len);
      char chunk[64u * 1024u];
      while (body.size() < content_len) {
        const std::size_t want = std::min(sizeof(chunk), content_len - body.size());
        int rerr = 0;
        const long n = conn.read_some(chunk, want, rerr);
        if (n <= 0) return;
        body.append(chunk, static_cast<std::size_t>(n));
      }
      if (payload_hash != kUnsignedPayload && lower(payload_hash) != sha256_hex(body)) {
        write_s3_error(conn, 400, "XAmzContentSHA256Mismatch",
                       "The provided 'x-amz-content-sha256' header does not match what was "
                       "computed.",
                       path);
        return;
      }
    }

    // Every logical write ends with fsync so the inode size is published before
    // the 200 goes out; other mounts otherwise read the object back as size 0.
    auto fsync_or_fail = [&](uint64_t ino) -> bool {
      const int ferr = aios_posix_fsync(fs_, ino);
      if (ferr == 0) return true;
      if (!write_posix_err(conn, ferr, path))
        write_s3_error(conn, 500, "InternalError", "fsync failed", path);
      return false;
    };

    const bool set_owner = !is_root && iam_cred.has_value();
    const uint32_t own_uid = set_owner ? iam_cred->uid : 0;
    const uint32_t own_gid = set_owner ? iam_cred->gid : 0;
    // Thread-scoped POSIX caller: IAM principal or root (0:0).
    aios_posix_set_caller(fs_, is_root ? 0u : own_uid, is_root ? 0u : own_gid);
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
      write_http(conn, 200, "OK", {{"Content-Type", "application/xml"}}, xml.str());
      return;
    }

    if (bucket.empty()) {
      write_s3_error(conn, 400, "InvalidRequest", "bucket required", path);
      return;
    }
    if (!allow_bucket(bucket)) {
      write_s3_error(conn, 403, "AccessDenied", "bucket not allowed for this access key", path);
      return;
    }

    // ----- CreateBucket -----
    if (method == "PUT" && key.empty() && qmap.count("uploads") == 0) {
      if (!valid_bucket_name(bucket)) {
        write_s3_error(conn, 400, "InvalidBucketName", "invalid bucket name", path);
        return;
      }
      aios_posix_stat st{};
      int err = aios_posix_lookup(fs_, kRootIno, bucket.c_str(), &st);
      if (err == 0) {
        write_s3_error(conn, 409, "BucketAlreadyOwnedByYou", "bucket exists", path);
        return;
      }
      err = aios_posix_mkdir(fs_, kRootIno, bucket.c_str(), 0755, &st);
      if (!err) err = aios_posix_fsyncdir(fs_, kRootIno);
      if (err) {
        write_s3_error(conn, 500, "InternalError", "mkdir failed", path);
        return;
      }
      apply_owner(fs_, st.ino, set_owner, own_uid, own_gid);
      write_http(conn, 200, "OK", {{"Location", "/" + bucket}}, {});
      return;
    }

    // ----- HeadBucket / DeleteBucket -----
    if (key.empty() && (method == "HEAD" || method == "DELETE")) {
      aios_posix_stat st{};
      int err = aios_posix_lookup(fs_, kRootIno, bucket.c_str(), &st);
      if (err || !S_ISDIR(st.mode)) {
        write_s3_error(conn, 404, "NoSuchBucket", "The specified bucket does not exist", path);
        return;
      }
      if (method == "HEAD") {
        write_http(conn, 200, "OK", {}, {});
        return;
      }
      if (!dir_empty(fs_, st.ino)) {
        write_s3_error(conn, 409, "BucketNotEmpty", "The bucket you tried to delete is not empty",
                       path);
        return;
      }
      err = aios_posix_rmdir(fs_, kRootIno, bucket.c_str());
      if (!err) err = aios_posix_fsyncdir(fs_, kRootIno);
      if (err) {
        write_s3_error(conn, 500, "InternalError", "rmdir failed", path);
        return;
      }
      write_http(conn, 204, "No Content", {}, {});
      return;
    }

    // ----- ListObjectsV2 -----
    if (method == "GET" && key.empty() &&
        (qmap.count("list-type") || qmap.count("prefix") || qmap.count("delimiter") ||
         qmap.empty() || qmap.count("max-keys"))) {
      aios_posix_stat bst{};
      int err = aios_posix_lookup(fs_, kRootIno, bucket.c_str(), &bst);
      if (err || !S_ISDIR(bst.mode)) {
        write_s3_error(conn, 404, "NoSuchBucket", "The specified bucket does not exist", path);
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
      write_http(conn, 200, "OK", {{"Content-Type", "application/xml"}}, xml.str());
      return;
    }

    // Need object key for remaining ops
    if (key.empty()) {
      write_s3_error(conn, 400, "InvalidRequest", "object key required", path);
      return;
    }

    // Folder marker: trailing slash Put → mkdir
    if (method == "PUT" && !key.empty() && key.back() == '/' && qmap.count("uploads") == 0 &&
        header_get(headers, "x-amz-copy-source").empty()) {
      auto parts = split_key(bucket + "/" + key);
      int err = mkdir_p(fs_, parts, nullptr, set_owner, own_uid, own_gid);
      if (err) {
        write_s3_error(conn, 500, "InternalError", "mkdir failed", path);
        return;
      }
      write_http(conn, 200, "OK", {}, {});
      return;
    }

    // ----- CreateMultipartUpload -----
    if (method == "POST" && qmap.count("uploads")) {
      aios_posix_stat bst{};
      if (aios_posix_lookup(fs_, kRootIno, bucket.c_str(), &bst) || !S_ISDIR(bst.mode)) {
        write_s3_error(conn, 404, "NoSuchBucket", "The specified bucket does not exist", path);
        return;
      }
      std::string upload_id = random_upload_id();
      if (upload_id.empty()) {
        write_s3_error(conn, 500, "InternalError", "multipart init failed", path);
        return;
      }
      aios_posix_stat mst{};
      aios_posix_lookup(fs_, kRootIno, kMultipartDir, &mst);
      aios_posix_stat ust{};
      int err = aios_posix_mkdir(fs_, mst.ino, upload_id.c_str(), 0700, &ust);
      if (err) {
        write_s3_error(conn, 500, "InternalError", "multipart init failed", path);
        return;
      }
      // Target key and owner live on the upload dir; UploadPart/Complete/Abort
      // check the owner so an uploadId is not a bearer token.
      aios_posix_setxattr(fs_, ust.ino, kXattrUploadBucket, bucket.data(), bucket.size(), 0);
      aios_posix_setxattr(fs_, ust.ino, kXattrUploadKey, key.data(), key.size(), 0);
      if (aios_posix_setxattr(fs_, ust.ino, kXattrUploadOwner, akid.data(), akid.size(), 0) != 0) {
        aios_posix_rmdir(fs_, mst.ino, upload_id.c_str());
        write_s3_error(conn, 500, "InternalError", "multipart init failed", path);
        return;
      }
      std::ostringstream xml;
      xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
          << "<InitiateMultipartUploadResult>"
          << "<Bucket>" << xml_escape(bucket) << "</Bucket><Key>" << xml_escape(key)
          << "</Key><UploadId>" << xml_escape(upload_id) << "</UploadId>"
          << "</InitiateMultipartUploadResult>";
      write_http(conn, 200, "OK", {{"Content-Type", "application/xml"}}, xml.str());
      return;
    }

    // ----- UploadPart -----
    if (method == "PUT" && qmap.count("uploadId") && qmap.count("partNumber")) {
      std::string upload_id = qmap["uploadId"];
      std::string part = qmap["partNumber"];
      aios_posix_stat mst{}, ust{};
      if (aios_posix_lookup(fs_, kRootIno, kMultipartDir, &mst) ||
          aios_posix_lookup(fs_, mst.ino, upload_id.c_str(), &ust)) {
        write_s3_error(conn, 404, "NoSuchUpload", "upload not found", path);
        return;
      }
      if (!upload_owned_by(fs_, ust.ino, akid)) {
        write_s3_error(conn, 403, "AccessDenied", "upload belongs to another principal", path);
        return;
      }
      int pn = 0;
      try {
        pn = std::stoi(part);
      } catch (...) {
        pn = 0;
      }
      if (pn < 1 || pn > 10000 || std::to_string(pn) != part) {
        write_s3_error(conn, 400, "InvalidArgument", "partNumber must be 1..10000", path);
        return;
      }
      uint64_t fino = 0;
      int err = ensure_file(fs_, ust.ino, part.c_str(), &fino);
      if (err) {
        write_s3_error(conn, 500, "InternalError", "part create failed", path);
        return;
      }
      aios_posix_truncate(fs_, fino, 0);
      size_t wrote = 0;
      err = aios_posix_write(fs_, fino, 0, body.data(), body.size(), &wrote);
      if (err) {
        write_s3_error(conn, 500, "InternalError", "part write failed", path);
        return;
      }
      if (!fsync_or_fail(fino)) return;
      auto etag = md5_hex(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
      write_http(conn, 200, "OK", {{"ETag", "\"" + etag + "\""}}, {});
      return;
    }

    // ----- CompleteMultipartUpload -----
    if (method == "POST" && qmap.count("uploadId") && !qmap.count("uploads")) {
      std::string upload_id = qmap["uploadId"];
      aios_posix_stat mst{}, ust{};
      if (aios_posix_lookup(fs_, kRootIno, kMultipartDir, &mst) ||
          aios_posix_lookup(fs_, mst.ino, upload_id.c_str(), &ust)) {
        write_s3_error(conn, 404, "NoSuchUpload", "upload not found", path);
        return;
      }
      if (!upload_owned_by(fs_, ust.ino, akid)) {
        write_s3_error(conn, 403, "AccessDenied", "upload belongs to another principal", path);
        return;
      }
      const std::string tbucket = get_xattr_string(fs_, ust.ino, kXattrUploadBucket);
      const std::string tkey = get_xattr_string(fs_, ust.ino, kXattrUploadKey);
      if (tbucket.empty() || tkey.empty()) {
        write_s3_error(conn, 500, "InternalError", "upload meta missing", path);
        return;
      }
      // The target may differ from the request path; the principal's bucket
      // allow-list applies to where the object lands.
      if (!allow_bucket(tbucket)) {
        write_s3_error(conn, 403, "AccessDenied", "target bucket not allowed for this access key",
                       path);
        return;
      }
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
        write_s3_error(conn, 500, "InternalError", "resolve target failed", path);
        return;
      }
      uint64_t fino = 0;
      err = ensure_file(fs_, parent, name, &fino, set_owner, own_uid, own_gid);
      if (err) {
        write_s3_error(conn, 500, "InternalError", "create target failed", path);
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
        write_s3_error(conn, 500, "InternalError", "assemble failed", path);
        return;
      }
      if (!fsync_or_fail(fino)) return;
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
      write_http(conn, 200, "OK", {{"Content-Type", "application/xml"}}, xml.str());
      return;
    }

    // ----- AbortMultipartUpload -----
    if (method == "DELETE" && qmap.count("uploadId")) {
      std::string upload_id = qmap["uploadId"];
      aios_posix_stat mst{}, ust{};
      if (aios_posix_lookup(fs_, kRootIno, kMultipartDir, &mst) == 0 &&
          aios_posix_lookup(fs_, mst.ino, upload_id.c_str(), &ust) == 0) {
        if (!upload_owned_by(fs_, ust.ino, akid)) {
          write_s3_error(conn, 403, "AccessDenied", "upload belongs to another principal", path);
          return;
        }
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
      write_http(conn, 204, "No Content", {}, {});
      return;
    }

    // ----- CopyObject -----
    if (method == "PUT" && !header_get(headers, "x-amz-copy-source").empty()) {
      auto src = url_decode(header_get(headers, "x-amz-copy-source"));
      if (!src.empty() && src[0] == '/') src.erase(src.begin());
      auto slash = src.find('/');
      if (slash == std::string::npos) {
        write_s3_error(conn, 400, "InvalidArgument", "bad copy source", path);
        return;
      }
      std::string sbucket = src.substr(0, slash), skey = src.substr(slash + 1);
      if (!allow_bucket(sbucket)) {
        write_s3_error(conn, 403, "AccessDenied", "source bucket not allowed", path);
        return;
      }
      uint64_t sp = 0;
      std::string sname;
      int err = resolve_parent(fs_, sbucket, skey, false, &sp, &sname);
      if (err) {
        write_s3_error(conn, 404, "NoSuchKey", "source not found", path);
        return;
      }
      aios_posix_stat sst{};
      err = aios_posix_lookup(fs_, sp, sname.c_str(), &sst);
      if (err || !S_ISREG(sst.mode)) {
        write_s3_error(conn, 404, "NoSuchKey", "source not found", path);
        return;
      }
      uint64_t dp = 0;
      std::string dname;
      err = resolve_parent(fs_, bucket, key, true, &dp, &dname, nullptr, set_owner, own_uid,
                          own_gid);
      if (err) {
        write_s3_error(conn, 500, "InternalError", "dest resolve failed", path);
        return;
      }
      uint64_t dino = 0;
      err = ensure_file(fs_, dp, dname, &dino, set_owner, own_uid, own_gid);
      if (err) {
        write_s3_error(conn, 500, "InternalError", "dest create failed", path);
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
        write_s3_error(conn, 500, "InternalError", "copy failed", path);
        return;
      }
      if (!fsync_or_fail(dino)) return;
      std::ostringstream xml;
      xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
          << "<CopyObjectResult><LastModified>" << iso8601_from_ns(sst.mtime_ns)
          << "</LastModified><ETag>&quot;copy-" << woff << "&quot;</ETag></CopyObjectResult>";
      write_http(conn, 200, "OK", {{"Content-Type", "application/xml"}}, xml.str());
      return;
    }

    // ----- PutObject -----
    if (method == "PUT") {
      aios_posix_stat bst{};
      if (aios_posix_lookup(fs_, kRootIno, bucket.c_str(), &bst) || !S_ISDIR(bst.mode)) {
        write_s3_error(conn, 404, "NoSuchBucket", "The specified bucket does not exist", path);
        return;
      }
      uint64_t parent = 0;
      std::string name;
      int err = resolve_parent(fs_, bucket, key, true, &parent, &name, nullptr, set_owner, own_uid,
                              own_gid);
      if (err) {
        write_s3_error(conn, 500, "InternalError", "path resolve failed", path);
        return;
      }
      uint64_t ino = 0;
      err = ensure_file(fs_, parent, name, &ino, set_owner, own_uid, own_gid);
      if (err) {
        write_s3_error(conn, 500, "InternalError", "create failed", path);
        return;
      }

      std::string put_data;
      std::string rdma_reply;
      if (rdma_put) {
        if (rdma_object_size == 0 || rdma_object_size > kCuObjectMaxTransferBytes) {
          write_s3_error(conn, 400, "InvalidArgument",
                         "RDMA PUT size out of range; retry without " +
                             std::string(kAmzRdmaToken),
                         path);
          return;
        }
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
          write_s3_error(conn, code, ename,
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
        if (!write_posix_err(conn, err, path))
          write_s3_error(conn, 500, "InternalError", "write failed", path);
        return;
      }
      if (!fsync_or_fail(ino)) return;
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
      write_http(conn, 200, "OK", rh, {});
      return;
    }

    // ----- GetObject / HeadObject -----
    if (method == "GET" || method == "HEAD") {
      uint64_t parent = 0;
      std::string name;
      int err = resolve_parent(fs_, bucket, key, false, &parent, &name);
      if (err) {
        write_s3_error(conn, 404, "NoSuchKey", "The specified key does not exist", path);
        return;
      }
      aios_posix_stat st{};
      err = aios_posix_lookup(fs_, parent, name.c_str(), &st);
      if (err || !S_ISREG(st.mode)) {
        write_s3_error(conn, 404, "NoSuchKey", "The specified key does not exist", path);
        return;
      }
      S3RangeParseResult ranges;
      if (method == "GET") {
        ranges = parse_s3_byte_ranges(header_get(headers, "range"), st.size);
        if (ranges.unsatisfiable) {
          write_s3_error(conn, 416, "InvalidRange", "Requested range not satisfiable", path);
          return;
        }
      }
      const bool ranged = ranges.present && !ranges.ranges.empty();

      std::unordered_map<std::string, std::string> rh = {
          {"Content-Type", "application/octet-stream"},
          {"Accept-Ranges", "bytes"},
          {"ETag", "\"size-" + std::to_string(st.size) + "\""},
          {"Last-Modified", iso8601_from_ns(st.mtime_ns)},
      };
      char ctbuf[256];
      int ctl = aios_posix_getxattr(fs_, st.ino, kXattrContentType, ctbuf, sizeof(ctbuf));
      if (ctl > 0) rh["Content-Type"] = std::string(ctbuf, ctl);
      const std::string object_ctype = rh["Content-Type"];

      if (method == "HEAD") {
        rh["Content-Length"] = std::to_string(st.size);
        write_http(conn, 200, "OK", rh, {});
        return;
      }

      auto read_span = [&](std::uint64_t start, std::uint64_t end, std::string& out) -> bool {
        const std::uint64_t len = end >= start ? (end - start + 1) : 0;
        out.resize(static_cast<std::size_t>(len));
        if (!len) return true;
        size_t got = 0;
        int rerr =
            aios_posix_read(fs_, st.ino, start, out.data(), static_cast<size_t>(len), &got);
        if (rerr) return false;
        out.resize(got);
        return true;
      };

      // Multi-range → multipart/byteranges (RFC 7233).
      if (ranged && ranges.ranges.size() > 1) {
        std::vector<std::string> parts;
        parts.reserve(ranges.ranges.size());
        for (const auto& br : ranges.ranges) {
          std::string part;
          if (!read_span(br.start, br.end, part)) {
            write_s3_error(conn, 500, "InternalError", "read failed", path);
            return;
          }
          parts.push_back(std::move(part));
        }
        const std::string boundary = "aiosboundary" + std::to_string(st.ino) + "x" +
                                     std::to_string(st.size) + "x" +
                                     std::to_string(ranges.ranges.size());
        auto body = build_s3_multipart_byteranges(boundary, object_ctype, st.size, ranges.ranges,
                                                  parts);
        rh["Content-Type"] = "multipart/byteranges; boundary=" + boundary;
        write_http(conn, 206, "Partial Content", rh, body);
        return;
      }

      std::uint64_t start = 0;
      std::uint64_t end = st.size ? st.size - 1 : 0;
      if (ranged) {
        start = ranges.ranges[0].start;
        end = ranges.ranges[0].end;
      } else if (st.size == 0) {
        start = 0;
        end = 0;
      }

      std::string out;
      if (st.size == 0 && !ranged) {
        out.clear();
      } else if (!read_span(start, end, out)) {
        write_s3_error(conn, 500, "InternalError", "read failed", path);
        return;
      }

      // Whole-object RDMA GET (no Range): push host buffer to client GPU/system memory.
      if (s3_want_rdma_get(rdma_token, ranged, method == "GET")) {
        std::string rdma_reply;
        std::string rdma_err;
        if (s3_try_rdma_get(cuobject_.get(), bucket + "/" + key, rdma_token, out.data(), out.size(),
                            rdma_reply, rdma_err)) {
          rh["Content-Length"] = std::to_string(out.size());
          rh[kAmzRdmaReply] = rdma_reply;
          write_http(conn, 200, "OK", rh, {});
          return;
        }
        AIOS_LOG_WARN("S3 RDMA GET failed (", rdma_err, "); falling back to TCP body");
      }

      if (ranged) {
        rh["Content-Range"] = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                              std::to_string(st.size);
        write_http(conn, 206, "Partial Content", rh, out);
      } else {
        write_http(conn, 200, "OK", rh, out);
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
        write_http(conn, 204, "No Content", {}, {});
        return;
      }
      aios_posix_unlink(fs_, parent, name.c_str());
      (void)aios_posix_fsyncdir(fs_, parent);
      write_http(conn, 204, "No Content", {}, {});
      return;
    }

    write_s3_error(conn, 405, "MethodNotAllowed", "method not supported", path);
  } catch (const std::exception& e) {
    AIOS_LOG_WARN("S3 session error: ", e.what());
    try {
      write_s3_error(conn, 500, "InternalError", e.what());
    } catch (...) {
    }
  }
}

}  // namespace aios
