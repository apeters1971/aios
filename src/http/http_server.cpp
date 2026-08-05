#include "http/http_server.hpp"

#include "http/http_auth.hpp"
#include "metrics/app_label.hpp"
#include "metrics/frontend_io.hpp"
#include "net/framing.hpp"
#include "object/object_layout.hpp"
#include "object/pubsub.hpp"
#include "object/archive_pack.hpp"
#include "object/archive_tape.hpp"
#include "object/repair.hpp"
#include "object/transition.hpp"
#include "util/auth.hpp"
#include "util/base64.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef AIOS_ADMIN_WEB_DEFAULT
#define AIOS_ADMIN_WEB_DEFAULT ""
#endif

namespace fs = std::filesystem;

namespace aios {
namespace {

using tcp = boost::asio::ip::tcp;

std::string sha256_hex(const std::uint8_t* data, std::size_t len) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  EVP_Digest(data, len, md, &md_len, EVP_sha256(), nullptr);
  static const char* hexd = "0123456789abcdef";
  std::string out(md_len * 2, '\0');
  for (unsigned int i = 0; i < md_len; ++i) {
    out[i * 2] = hexd[md[i] >> 4];
    out[i * 2 + 1] = hexd[md[i] & 0xf];
  }
  return out;
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

std::string url_encode_path(const std::string& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (unsigned char c : in) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xf]);
    }
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

bool parse_content_range(const std::string& v, std::uint64_t& start, std::uint64_t& end,
                         std::string& err) {
  // bytes START-END/* or bytes START-END/TOTAL
  if (v.rfind("bytes ", 0) != 0) {
    err = "Content-Range must start with bytes ";
    return false;
  }
  auto rest = v.substr(6);
  auto dash = rest.find('-');
  auto slash = rest.find('/');
  if (dash == std::string::npos || slash == std::string::npos || slash < dash) {
    err = "bad Content-Range";
    return false;
  }
  try {
    start = std::stoull(rest.substr(0, dash));
    end = std::stoull(rest.substr(dash + 1, slash - dash - 1));
  } catch (...) {
    err = "bad Content-Range numbers";
    return false;
  }
  if (end < start) {
    err = "Content-Range end < start";
    return false;
  }
  return true;
}

bool parse_range(const std::string& v, std::uint64_t& start, std::optional<std::uint64_t>& end,
                 std::string& err) {
  // bytes=START-END or bytes=START-
  if (v.rfind("bytes=", 0) != 0) {
    err = "Range must start with bytes=";
    return false;
  }
  auto rest = v.substr(6);
  auto dash = rest.find('-');
  if (dash == std::string::npos) {
    err = "bad Range";
    return false;
  }
  try {
    start = std::stoull(rest.substr(0, dash));
    auto end_s = rest.substr(dash + 1);
    if (!end_s.empty()) end = std::stoull(end_s);
  } catch (...) {
    err = "bad Range numbers";
    return false;
  }
  return true;
}

std::vector<AttrPrecondition> parse_preconditions(
    const std::unordered_map<std::string, std::string>& headers) {
  std::vector<AttrPrecondition> preds;
  auto inm = header_get(headers, "if-none-match");
  if (inm == "*") {
    preds.push_back({AttrPrecondition::Kind::MustNotExist, {}, {}});
  }
  auto im = header_get(headers, "if-match");
  if (im == "*") {
    preds.push_back({AttrPrecondition::Kind::MustExist, {}, {}});
  }

  // Multi-value headers may be joined; also accept repeated logical via comma.
  auto add_kv = [&](AttrPrecondition::Kind kind, const std::string& raw) {
    auto eq = raw.find('=');
    AttrPrecondition p;
    p.kind = kind;
    if (eq == std::string::npos) {
      p.key = raw;
    } else {
      p.key = raw.substr(0, eq);
      p.value = raw.substr(eq + 1);
    }
    if (!p.key.empty()) preds.push_back(std::move(p));
  };

  // Scan all headers for x-aios-if-attr-*
  for (const auto& [k, v] : headers) {
    if (k == "x-aios-if-attr-eq") add_kv(AttrPrecondition::Kind::Eq, v);
    else if (k == "x-aios-if-attr-ne") add_kv(AttrPrecondition::Kind::Ne, v);
    else if (k == "x-aios-if-attr-absent")
      preds.push_back({AttrPrecondition::Kind::Absent, v, {}});
    else if (k == "x-aios-if-attr-present")
      preds.push_back({AttrPrecondition::Kind::Present, v, {}});
  }
  return preds;
}

std::unordered_map<std::string, std::string> parse_attrs(
    const std::unordered_map<std::string, std::string>& headers) {
  std::unordered_map<std::string, std::string> attrs;
  const std::string prefix = "x-aios-attr-";
  for (const auto& [k, v] : headers) {
    if (k.rfind(prefix, 0) == 0) {
      attrs[k.substr(prefix.size())] = v;
    }
  }
  return attrs;
}

int status_for(const ApiResult& r) {
  if (r.ok) return 200;
  if (r.code == "not_found") return 404;
  if (r.code == "precondition_failed") return 412;
  if (r.code == "crc_mismatch") return 400;
  if (r.code == "range_unsatisfiable") return 416;
  if (r.code == "not_primary") return 307;
  if (r.code == "restoring") return 503;
  if (r.code == "frozen") return 409;
  if (r.code == "no_targets") return 503;
  if (r.code == "quorum_failed") return 503;
  if (r.code == "bad_request") return 400;
  if (r.code == "conflict") return 409;
  if (r.code == "lock_held") return 409;
  if (r.code == "mode_mismatch") return 409;
  if (r.code == "payload_too_large") return 413;
  return 500;
}

void write_response(tcp::socket& sock, int status, const std::string& reason,
                    const std::unordered_map<std::string, std::string>& headers,
                    const std::uint8_t* body, std::size_t body_len, bool keep_alive) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
  oss << "Content-Length: " << body_len << "\r\n";
  oss << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
  for (const auto& [k, v] : headers) {
    oss << k << ": " << v << "\r\n";
  }
  oss << "\r\n";
  const auto head = oss.str();
  boost::system::error_code ec;
  boost::asio::write(sock, boost::asio::buffer(head), ec);
  if (!ec && body_len > 0 && body) {
    boost::asio::write(sock, boost::asio::buffer(body, body_len), ec);
  }
}

void write_json(tcp::socket& sock, int status, const std::string& reason,
                const nlohmann::json& j, bool keep_alive) {
  const auto body = j.dump();
  write_response(sock, status, reason, {{"Content-Type", "application/json"}},
                 reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), keep_alive);
}

void write_not_primary(tcp::socket& sock, const std::string& path_with_query,
                       const ApiResult& r, bool keep_alive) {
  nlohmann::json acting = nlohmann::json::array();
  std::string location;
  if (!r.placement.acting_set.empty()) {
    const auto& p = r.placement.acting_set[0];
    if (!p.http_addr.empty()) {
      location = "http://" + p.http_addr + path_with_query;
    }
    for (const auto& t : r.placement.acting_set) {
      acting.push_back({{"node_id", t.node_id},
                        {"addr", t.addr},
                        {"http_addr", t.http_addr},
                        {"aios_path", t.aios_path}});
    }
  }
  nlohmann::json body = {{"error", r.error},
                         {"code", "not_primary"},
                         {"epoch", r.epoch},
                         {"acting_set", acting}};
  const auto body_s = body.dump();
  std::unordered_map<std::string, std::string> headers = {
      {"Content-Type", "application/json"},
      {"x-aios-acting-set", acting.dump()},
  };
  if (!location.empty()) {
    headers["Location"] = location;
    if (!r.placement.acting_set.empty()) {
      headers["x-aios-primary"] = r.placement.acting_set[0].node_id;
    }
  }
  write_response(sock, 307, "Temporary Redirect", headers,
                 reinterpret_cast<const std::uint8_t*>(body_s.data()), body_s.size(),
                 keep_alive);
}

bool write_file_body(tcp::socket& sock, int status, const std::string& reason,
                     std::unordered_map<std::string, std::string> headers,
                     const std::string& path, std::uint64_t size, bool head_only,
                     bool keep_alive) {
  headers["Content-Length"] = std::to_string(size);
  headers["Connection"] = keep_alive ? "keep-alive" : "close";
  std::ostringstream oss;
  oss << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
  for (const auto& [k, v] : headers) {
    if (k == "Content-Length" || k == "Connection") continue;
    oss << k << ": " << v << "\r\n";
  }
  oss << "Content-Length: " << size << "\r\n";
  oss << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
  oss << "\r\n";
  const auto head = oss.str();
  boost::system::error_code ec;
  boost::asio::write(sock, boost::asio::buffer(head), ec);
  if (ec || head_only || size == 0) return !ec;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::vector<char> buf(256 * 1024);
  std::uint64_t left = size;
  while (left > 0 && in) {
    const auto chunk = static_cast<std::streamsize>(
        std::min<std::uint64_t>(left, static_cast<std::uint64_t>(buf.size())));
    in.read(buf.data(), chunk);
    const auto n = in.gcount();
    if (n <= 0) break;
    boost::asio::write(sock, boost::asio::buffer(buf.data(), static_cast<std::size_t>(n)),
                       ec);
    if (ec) return false;
    left -= static_cast<std::uint64_t>(n);
  }
  return left == 0;
}

void write_api_error(tcp::socket& sock, const ApiResult& r, const std::string& path_q,
                     bool keep_alive) {
  if (r.code == "not_primary") {
    write_not_primary(sock, path_q, r, keep_alive);
    return;
  }
  if (r.code == "restoring") {
    const auto j =
        nlohmann::json({{"error", r.error}, {"code", r.code}, {"epoch", r.epoch}}).dump();
    write_response(sock, 503, "Service Unavailable",
                   {{"Content-Type", "application/json"}, {"Retry-After", "30"}},
                   reinterpret_cast<const std::uint8_t*>(j.data()), j.size(), keep_alive);
    return;
  }
  write_json(sock, status_for(r), "Error",
             {{"error", r.error}, {"code", r.code}, {"epoch", r.epoch}}, keep_alive);
}

bool read_line(tcp::socket& sock, std::string& line, boost::system::error_code& ec) {
  line.clear();
  char c;
  while (true) {
    boost::asio::read(sock, boost::asio::buffer(&c, 1), ec);
    if (ec) return false;
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return true;
    }
    line.push_back(c);
    if (line.size() > 64 * 1024) {
      ec = boost::asio::error::message_size;
      return false;
    }
  }
}

constexpr std::int64_t kAdminSessionTtlMs = 12LL * 60 * 60 * 1000;
constexpr const char* kAdminCookie = "aios_admin";

bool const_time_eq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

std::string cookie_get(const std::unordered_map<std::string, std::string>& headers,
                       const std::string& name) {
  const auto raw = header_get(headers, "cookie");
  std::size_t i = 0;
  while (i < raw.size()) {
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == ';')) ++i;
    auto eq = raw.find('=', i);
    auto semi = raw.find(';', i);
    if (eq == std::string::npos) break;
    if (semi == std::string::npos) semi = raw.size();
    if (eq < semi) {
      auto k = raw.substr(i, eq - i);
      auto v = raw.substr(eq + 1, semi - eq - 1);
      if (k == name) return v;
    }
    i = semi;
  }
  return {};
}

std::string make_admin_session(const std::string& cluster_key) {
  const auto exp = std::to_string(now_ms() + kAdminSessionTtlMs);
  const auto sig = hmac_sha256_hex(cluster_key, std::string("aios-admin-v1\n") + exp);
  return exp + "." + sig;
}

bool verify_admin_session(const std::string& token, const std::string& cluster_key) {
  auto dot = token.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= token.size()) return false;
  const auto exp_s = token.substr(0, dot);
  const auto sig = token.substr(dot + 1);
  std::int64_t exp = 0;
  try {
    exp = std::stoll(exp_s);
  } catch (...) {
    return false;
  }
  if (exp < now_ms()) return false;
  const auto expect = hmac_sha256_hex(cluster_key, std::string("aios-admin-v1\n") + exp_s);
  return const_time_eq(expect, sig);
}

std::filesystem::path find_admin_web_root() {
  if (const char* env = std::getenv("AIOS_ADMIN_WEB"); env && *env) {
    std::filesystem::path p(env);
    if (std::filesystem::is_directory(p)) return p;
  }
  const char* defaults = AIOS_ADMIN_WEB_DEFAULT;
  if (defaults && *defaults) {
    std::filesystem::path p(defaults);
    if (std::filesystem::is_directory(p)) return p;
  }
  for (const char* cand : {"web/admin", "../web/admin", "share/aios/admin",
                           "../share/aios/admin"}) {
    std::filesystem::path p(cand);
    if (std::filesystem::is_directory(p)) return std::filesystem::absolute(p);
  }
  return {};
}

bool is_admin_static_path(const std::string& path) {
  return path == "/admin" || path == "/admin/" || path == "/admin/index.html" ||
         path == "/admin/app.js" || path == "/admin/style.css" ||
         path == "/admin/aios-icon.png";
}

std::string admin_static_content_type(const std::string& path) {
  if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") return "text/html; charset=utf-8";
  if (path.size() >= 3 && path.substr(path.size() - 3) == ".js")
    return "application/javascript; charset=utf-8";
  if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") return "text/css; charset=utf-8";
  if (path.size() >= 4 && path.substr(path.size() - 4) == ".png") return "image/png";
  return "application/octet-stream";
}

bool serve_admin_static(tcp::socket& sock, const std::string& path, bool keep_alive) {
  auto root = find_admin_web_root();
  if (root.empty()) {
    write_json(sock, 503, "Service Unavailable",
               {{"error", "admin web assets not found (set AIOS_ADMIN_WEB)"}}, keep_alive);
    return true;
  }
  std::string rel = "index.html";
  if (path == "/admin/app.js") rel = "app.js";
  else if (path == "/admin/style.css") rel = "style.css";
  else if (path == "/admin/aios-icon.png") rel = "aios-icon.png";
  else if (path == "/admin/index.html" || path == "/admin" || path == "/admin/") rel = "index.html";
  else {
    write_json(sock, 404, "Not Found", {{"error", "unknown admin asset"}}, keep_alive);
    return true;
  }
  auto file = root / rel;
  std::error_code ec;
  file = std::filesystem::weakly_canonical(file, ec);
  auto root_c = std::filesystem::weakly_canonical(root, ec);
  if (ec || file.string().rfind(root_c.string(), 0) != 0 ||
      !std::filesystem::is_regular_file(file)) {
    write_json(sock, 404, "Not Found", {{"error", "asset missing"}}, keep_alive);
    return true;
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    write_json(sock, 404, "Not Found", {{"error", "asset unreadable"}}, keep_alive);
    return true;
  }
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  write_response(sock, 200, "OK", {{"Content-Type", admin_static_content_type(path)}},
                 reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), keep_alive);
  return true;
}

}  // namespace

nlohmann::json HttpServer::admin_config_json() const {
  const auto& c = objects_.config();
  nlohmann::json rules = nlohmann::json::array();
  for (const auto& r : c.layout_rules) {
    nlohmann::json jr = {{"prefix", r.prefix}, {"layout", r.layout}};
    if (r.storage_class) jr["storage_class"] = *r.storage_class;
    if (r.ec_k) jr["ec_k"] = *r.ec_k;
    if (r.ec_m) jr["ec_m"] = *r.ec_m;
    if (r.ec_codec) jr["ec_codec"] = *r.ec_codec;
    rules.push_back(std::move(jr));
  }
  nlohmann::json transitions = nlohmann::json::array();
  for (const auto& r : c.transition_rules) {
    nlohmann::json jr = {{"prefix", r.prefix}, {"from", r.from}, {"to", r.to}};
    if (r.layout) jr["layout"] = *r.layout;
    if (r.ec_k) jr["ec_k"] = *r.ec_k;
    if (r.ec_m) jr["ec_m"] = *r.ec_m;
    if (r.ec_codec) jr["ec_codec"] = *r.ec_codec;
    transitions.push_back(std::move(jr));
  }
  nlohmann::json archives = nlohmann::json::array();
  for (const auto& r : c.archive_rules) {
    archives.push_back({{"prefix", r.prefix},
                        {"from", r.from},
                        {"staging_class", r.staging_class},
                        {"min_age_days", r.min_age_days},
                        {"min_bag_bytes", r.min_bag_bytes},
                        {"max_bag_bytes", r.max_bag_bytes},
                        {"max_members", r.max_members},
                        {"max_open_ms", r.max_open_ms},
                        {"tape_sink", r.tape_sink},
                        {"tape_root", r.tape_root},
                        {"tape_put_cmd", r.tape_put_cmd},
                        {"tape_get_cmd", r.tape_get_cmd}});
  }
  return nlohmann::json{
      {"node_id", c.node_id},
      {"listen", c.listen},
      {"http_listen", c.http_listen},
      {"peers", c.peers},
      {"cluster_key", "***"},
      {"auth_skew_ms", c.auth_skew_ms},
      {"gossip_interval_ms", c.gossip_interval_ms},
      {"suspect_after_ms", c.suspect_after_ms},
      {"dead_after_ms", c.dead_after_ms},
      {"scan_interval_ms", c.scan_interval_ms},
      {"status_file", c.status_file},
      {"replica_count", c.replica_count},
      {"write_quorum", c.write_quorum > 0 ? c.write_quorum : c.replica_count},
      {"durability", c.durability},
      {"default_storage_class", c.default_storage_class},
      {"placement",
       {{"vnodes_per_target", c.vnodes_per_target},
        {"min_vnodes", c.min_vnodes},
        {"max_vnodes", c.max_vnodes}}},
      {"ec_k", c.ec_k},
      {"ec_m", c.ec_m},
      {"ec_codec", c.ec_codec},
      {"max_ec_k", c.max_ec_k},
      {"max_ec_m", c.max_ec_m},
      {"max_replica_count", c.max_replica_count},
      {"layout_rules", std::move(rules)},
      {"transition_rules", std::move(transitions)},
      {"archive_rules", std::move(archives)},
      {"repair_interval_ms", c.repair_interval_ms},
      {"repair_batch_oids", c.repair_batch_oids},
      {"transition_interval_ms", c.transition_interval_ms},
      {"transition_batch_oids", c.transition_batch_oids},
      {"archive_interval_ms", c.archive_interval_ms},
      {"archive_batch_oids", c.archive_batch_oids},
      {"http_body_sync", c.http_body_sync},
      {"max_versions", c.max_versions},
      {"clone_required", c.clone_required},
      {"max_object_bytes", c.max_object_bytes},
      {"admin", cfg_.admin},
      {"admin_metrics_public", cfg_.admin_metrics_public},
      {"s3_listen", c.s3_listen},
      {"s3_volume", c.s3_volume},
      {"s3_access_key", c.s3_access_key},
      {"cuobject_listen", c.cuobject_listen},
      {"compression", c.compression},
      {"compression_level", c.compression_level},
      {"compression_min_bytes", c.compression_min_bytes},
  };
}

nlohmann::json HttpServer::admin_status_json() const {
  const auto members = membership_.snapshot();
  std::size_t alive = 0;
  for (const auto& m : members) {
    if (m.state == MemberState::Alive) ++alive;
  }
  return nlohmann::json{
      {"node_id", cfg_.node_id},
      {"listen", cfg_.listen},
      {"http_listen", cfg_.http_listen},
      {"admin", cfg_.admin},
      {"admin_metrics_public", cfg_.admin_metrics_public},
      {"map_epoch", objects_.map().epoch},
      {"map_targets", objects_.map().targets.size()},
      {"members", members.size()},
      {"members_alive", alive},
      {"membership", membership_.to_json()},
      {"cluster_map", objects_.map().to_json()},
      {"ops", objects_.ops().to_json()},
      {"ops_by_label", objects_.ops().by_label_json()},
  };
}

HttpServer::HttpServer(boost::asio::io_context& ioc, Config cfg, ObjectService& objects,
                       MembershipTable& membership, std::shared_ptr<S3IamStore> s3_iam,
                       std::shared_ptr<QuotaAdminStore> quota, std::shared_ptr<QosAdminStore> qos)
    : ioc_(ioc),
      cfg_(std::move(cfg)),
      objects_(objects),
      membership_(membership),
      s3_iam_(std::move(s3_iam)),
      quota_(std::move(quota)),
      qos_(std::move(qos)),
      acceptor_(ioc) {
  std::string host, port;
  if (!split_host_port(cfg_.http_listen, host, port)) {
    throw std::runtime_error("invalid http_listen: " + cfg_.http_listen);
  }
  tcp::resolver resolver(ioc_);
  const auto endpoints = resolver.resolve(host, port);
  const tcp::endpoint ep = *endpoints.begin();
  acceptor_.open(ep.protocol());
  acceptor_.set_option(tcp::acceptor::reuse_address(true));
  acceptor_.bind(ep);
  acceptor_.listen();
  AIOS_LOG_INFO("http listening on ", ep.address().to_string(), ":", ep.port());
}

void HttpServer::start() { do_accept(); }

void HttpServer::do_accept() {
  auto sock = std::make_shared<tcp::socket>(ioc_);
  acceptor_.async_accept(*sock, [this, sock](boost::system::error_code ec) {
    if (!ec) {
      boost::asio::post(ioc_, [this, sock] { handle_session(sock); });
    } else {
      AIOS_LOG_WARN("http accept: ", ec.message());
    }
    do_accept();
  });
}

void HttpServer::handle_session(std::shared_ptr<tcp::socket> sock) {
  boost::system::error_code ec;
  bool keep_alive = true;

  while (keep_alive) {
    std::string req_line;
    if (!read_line(*sock, req_line, ec) || req_line.empty()) return;

    std::istringstream rls(req_line);
    std::string method, target, version;
    rls >> method >> target >> version;
    if (method.empty() || target.empty()) return;

    std::unordered_map<std::string, std::string> headers;
    while (true) {
      std::string line;
      if (!read_line(*sock, line, ec)) return;
      if (line.empty()) break;
      auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      auto name = line.substr(0, colon);
      auto value = line.substr(colon + 1);
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
      }
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      headers[name] = value;
    }

    std::size_t content_length = 0;
    {
      auto cl = header_get(headers, "content-length");
      if (!cl.empty()) {
        try {
          content_length = static_cast<std::size_t>(std::stoull(cl));
        } catch (...) {
          write_json(*sock, 400, "Bad Request", {{"error", "bad Content-Length"}}, false);
          return;
        }
      }
    }
    if (content_length > cfg_.max_object_bytes) {
      write_json(*sock, 413, "Payload Too Large", {{"error", "body too large"}}, false);
      return;
    }

    constexpr std::size_t kMemThreshold = 256u * 1024u;
    std::vector<std::uint8_t> body;
    std::string upload_path;
    std::uint32_t upload_crc = 0;
    if (content_length > 0 && content_length > kMemThreshold) {
      upload_path =
          (fs::temp_directory_path() / ("aios-upload-" + std::to_string(aios::now_ms())))
              .string();
      std::ofstream out(upload_path, std::ios::binary | std::ios::trunc);
      if (!out) {
        write_json(*sock, 500, "Error", {{"error", "cannot create upload temp"}}, false);
        return;
      }
      std::vector<std::uint8_t> buf(256 * 1024);
      std::size_t left = content_length;
      upload_crc = 0;
      std::uint32_t crc = 0;
      while (left > 0) {
        const auto n = std::min(left, buf.size());
        boost::asio::read(*sock, boost::asio::buffer(buf.data(), n), ec);
        if (ec) {
          out.close();
          fs::remove(upload_path);
          return;
        }
        out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(n));
        crc = crc32c_update(crc, buf.data(), n);
        left -= n;
      }
      out.close();
      upload_crc = crc;
    } else if (content_length > 0) {
      body.resize(content_length);
      boost::asio::read(*sock, boost::asio::buffer(body), ec);
      if (ec) return;
    }

    const auto conn = header_get(headers, "connection");
    if (version == "HTTP/1.0") keep_alive = (conn == "keep-alive");
    else keep_alive = (conn != "close");

    std::string path = target;
    std::string query;
    auto qpos = target.find('?');
    if (qpos != std::string::npos) {
      path = target.substr(0, qpos);
      query = target.substr(qpos + 1);
    }
    auto qmap = parse_query(query);

    // Optional client workload label (for OPS / future QoS).
    std::string app_label;
    {
      const auto raw = header_get(headers, kAppLabelHeader);
      std::string lerr;
      if (!normalize_app_label(raw, app_label, lerr)) {
        write_json(*sock, 400, "Bad Request", {{"error", lerr}, {"code", "bad_request"}},
                   keep_alive);
        continue;
      }
    }
    AppLabelScope label_scope(app_label);
    objects_.ops().note_http_request();

    const bool metrics_public =
        cfg_.admin && cfg_.admin_metrics_public && method == "GET" && path == "/metrics";
    const bool admin_static = cfg_.admin && method == "GET" && is_admin_static_path(path);
    const bool admin_login = cfg_.admin && method == "POST" && path == "/admin/login";
    const bool admin_logout = cfg_.admin && method == "POST" && path == "/admin/logout";
    const bool admin_api = path.rfind("/admin/api/", 0) == 0;
    const bool skip_hmac = metrics_public || admin_static || admin_login || admin_logout;

    bool authed = skip_hmac;
    if (!skip_hmac) {
      const bool unsigned_payload =
          header_get(headers, "x-aios-content-sha256") == "UNSIGNED-PAYLOAD";
      const std::string payload_hash =
          unsigned_payload ? "UNSIGNED-PAYLOAD" : sha256_hex(body.data(), body.size());
      auto auth = http_auth_verify(method, target, headers, payload_hash, cfg_.cluster_key,
                                   cfg_.auth_skew_ms);
      if (auth.ok) {
        authed = true;
      } else if (admin_api && cfg_.admin) {
        const auto tok = cookie_get(headers, kAdminCookie);
        if (!tok.empty() && verify_admin_session(tok, cfg_.cluster_key)) authed = true;
        else {
          write_json(*sock, 401, "Unauthorized",
                     {{"error", auth.error.empty() ? "login required" : auth.error}}, keep_alive);
          continue;
        }
      } else {
        write_json(*sock, 401, "Unauthorized", {{"error", auth.error}}, keep_alive);
        continue;
      }
    }
    (void)authed;

    auto preds = parse_preconditions(headers);
    auto attrs = parse_attrs(headers);

    // Admin console API / web UI (only when node started with admin: true / --admin).
    if (path.rfind("/admin", 0) == 0 || path == "/metrics") {
      if (!cfg_.admin) {
        write_json(*sock, 404, "Not Found",
                   {{"error", "admin API disabled (set admin: true)"}}, keep_alive);
        continue;
      }

      if (admin_static) {
        serve_admin_static(*sock, path, keep_alive);
        continue;
      }

      if (admin_login) {
        std::string key;
        try {
          const std::string raw = body.empty()
                                      ? "{}"
                                      : std::string(reinterpret_cast<const char*>(body.data()),
                                                    body.size());
          auto j = nlohmann::json::parse(raw);
          if (j.contains("cluster_key") && j["cluster_key"].is_string())
            key = j["cluster_key"].get<std::string>();
        } catch (...) {
          write_json(*sock, 400, "Bad Request", {{"error", "invalid JSON"}}, keep_alive);
          continue;
        }
        if (!const_time_eq(key, cfg_.cluster_key)) {
          write_json(*sock, 401, "Unauthorized", {{"error", "invalid cluster key"}}, keep_alive);
          continue;
        }
        const auto token = make_admin_session(cfg_.cluster_key);
        write_response(*sock, 200, "OK",
                       {{"Content-Type", "application/json"},
                        {"Set-Cookie", std::string(kAdminCookie) + "=" + token +
                                           "; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200"}},
                       reinterpret_cast<const std::uint8_t*>(R"({"ok":true})"), 11, keep_alive);
        continue;
      }

      if (admin_logout) {
        write_response(*sock, 200, "OK",
                       {{"Content-Type", "application/json"},
                        {"Set-Cookie", std::string(kAdminCookie) +
                                           "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0"}},
                       reinterpret_cast<const std::uint8_t*>(R"({"ok":true})"), 11, keep_alive);
        continue;
      }

      if (method == "GET" && path == "/metrics") {
        const auto body_txt = objects_.ops().to_prometheus(cfg_.node_id);
        write_response(*sock, 200, "OK",
                       {{"Content-Type", "text/plain; version=0.0.4; charset=utf-8"}},
                       reinterpret_cast<const std::uint8_t*>(body_txt.data()), body_txt.size(),
                       keep_alive);
        continue;
      }

      auto write_cluster = [&]() {
        nlohmann::json peers = nlohmann::json::array();
        std::unordered_set<std::string> seen;
        for (const auto& m : membership_.snapshot()) {
          if (m.state != MemberState::Alive) continue;
          if (m.http_addr.empty()) continue;
          if (!seen.insert(m.http_addr).second) continue;
          peers.push_back({{"node_id", m.node_id},
                           {"addr", m.addr},
                           {"http_addr", m.http_addr},
                           {"self", m.node_id == cfg_.node_id}});
        }
        write_json(*sock, 200, "OK",
                   {{"node_id", cfg_.node_id},
                    {"status", admin_status_json()},
                    {"admin_peers", peers}},
                   keep_alive);
      };

      auto write_transitions = [&]() {
        nlohmann::json rules = nlohmann::json::array();
        for (const auto& r : cfg_.transition_rules) {
          nlohmann::json jr = {{"prefix", r.prefix}, {"from", r.from}, {"to", r.to}};
          if (r.layout) jr["layout"] = *r.layout;
          rules.push_back(std::move(jr));
        }
        write_json(*sock, 200, "OK",
                   {{"transition_rules", rules},
                    {"transition_interval_ms", cfg_.transition_interval_ms},
                    {"transition_batch_oids", cfg_.transition_batch_oids}},
                   keep_alive);
      };

      auto run_transitions_handler = [&]() {
        const auto stats = run_transitions(
            cfg_, objects_.advertise(), objects_.map(), objects_.stores(),
            static_cast<std::size_t>(std::max(1, cfg_.transition_batch_oids)));
        write_json(*sock, 200, "OK",
                   {{"oids_scanned", stats.oids_scanned},
                    {"matched", stats.matched},
                    {"migrated", stats.migrated},
                    {"drained", stats.drained},
                    {"failed", stats.failed}},
                   keep_alive);
      };

      if (method == "GET" && (path == "/admin/status" || path == "/admin/api/status")) {
        write_json(*sock, 200, "OK", admin_status_json(), keep_alive);
        continue;
      }
      if (method == "GET" && (path == "/admin/ops" || path == "/admin/api/ops")) {
        auto payload = objects_.ops().to_admin_json();
        payload["io_frontends"] = io_frontends_admin_json(payload.value("ops_by_label", nlohmann::json::object()));
        payload["node_id"] = cfg_.node_id;
        write_json(*sock, 200, "OK", payload, keep_alive);
        continue;
      }
      if (method == "GET" && (path == "/admin/config" || path == "/admin/api/config")) {
        write_json(*sock, 200, "OK", admin_config_json(), keep_alive);
        continue;
      }
      if (method == "GET" && (path == "/admin/cluster" || path == "/admin/api/cluster")) {
        write_cluster();
        continue;
      }
      if (method == "GET" &&
          (path == "/admin/transitions" || path == "/admin/api/transitions")) {
        write_transitions();
        continue;
      }
      if (method == "POST" &&
          (path == "/admin/transitions/run" || path == "/admin/api/transitions/run")) {
        run_transitions_handler();
        continue;
      }
      if (method == "GET" && (path == "/admin/archive" || path == "/admin/api/archive")) {
        nlohmann::json rules = nlohmann::json::array();
        for (const auto& r : cfg_.archive_rules) {
          rules.push_back({{"prefix", r.prefix},
                           {"from", r.from},
                           {"staging_class", r.staging_class},
                           {"min_age_days", r.min_age_days},
                           {"min_bag_bytes", r.min_bag_bytes},
                           {"max_bag_bytes", r.max_bag_bytes},
                           {"max_members", r.max_members},
                           {"max_open_ms", r.max_open_ms},
                           {"tape_sink", r.tape_sink},
                           {"tape_root", r.tape_root},
                           {"tape_put_cmd", r.tape_put_cmd},
                           {"tape_get_cmd", r.tape_get_cmd}});
        }
        write_json(*sock, 200, "OK",
                   {{"archive_rules", rules},
                    {"archive_interval_ms", cfg_.archive_interval_ms},
                    {"archive_batch_oids", cfg_.archive_batch_oids}},
                   keep_alive);
        continue;
      }
      if (method == "POST" &&
          (path == "/admin/archive/run" || path == "/admin/api/archive/run")) {
        const auto stats = run_archive(
            cfg_, objects_.advertise(), objects_.map(), objects_.stores(),
            static_cast<std::size_t>(std::max(1, cfg_.archive_batch_oids)));
        write_json(*sock, 200, "OK",
                   {{"oids_scanned", stats.oids_scanned},
                    {"matched", stats.matched},
                    {"packed", stats.packed},
                    {"bags_sealed", stats.bags_sealed},
                    {"failed", stats.failed}},
                   keep_alive);
        continue;
      }
      if (method == "POST" &&
          (path == "/admin/archive/drain" || path == "/admin/api/archive/drain")) {
        const auto stats = run_archive_drain(
            cfg_, objects_.advertise(), objects_.map(), objects_.stores(),
            static_cast<std::size_t>(std::max(1, cfg_.archive_batch_oids)));
        write_json(*sock, 200, "OK",
                   {{"bags_scanned", stats.bags_scanned},
                    {"drained", stats.drained},
                    {"skipped", stats.skipped},
                    {"failed", stats.failed}},
                   keep_alive);
        continue;
      }
      if (method == "POST" &&
          (path == "/admin/archive/recall" || path == "/admin/api/archive/recall")) {
        try {
          const std::string raw =
              body.empty() ? "{}"
                           : std::string(reinterpret_cast<const char*>(body.data()), body.size());
          auto j = nlohmann::json::parse(raw);
          const std::string oid = j.value("oid", "");
          if (oid.empty()) {
            write_json(*sock, 400, "Bad Request", {{"error", "oid required"}}, keep_alive);
            continue;
          }
          std::string err;
          if (!recall_archived_oid(cfg_, objects_.advertise(), objects_.map(), objects_.stores(),
                                   oid, err)) {
            const int st = (err == "restoring") ? 503 : 400;
            write_json(*sock, st, st == 503 ? "Service Unavailable" : "Bad Request",
                       {{"error", err}, {"code", err == "restoring" ? "restoring" : "bad_request"}},
                       keep_alive);
            continue;
          }
          write_json(*sock, 200, "OK", {{"ok", true}, {"oid", oid}}, keep_alive);
        } catch (...) {
          write_json(*sock, 400, "Bad Request", {{"error", "invalid JSON"}}, keep_alive);
        }
        continue;
      }
      if (method == "POST" && path == "/admin/api/repair/run") {
        const auto stats = run_repair(
            cfg_, objects_.advertise(), objects_.map(), objects_.stores(),
            static_cast<std::size_t>(std::max(1, cfg_.repair_batch_oids)));
        write_json(*sock, 200, "OK",
                   {{"oids_scanned", stats.oids_scanned},
                    {"under_replicated", stats.under_replicated},
                    {"repaired", stats.repaired},
                    {"failed", stats.failed}},
                   keep_alive);
        continue;
      }
      if (method == "POST" && path == "/admin/api/settings") {
        try {
          const std::string raw = body.empty()
                                      ? "{}"
                                      : std::string(reinterpret_cast<const char*>(body.data()),
                                                    body.size());
          auto j = nlohmann::json::parse(raw);
          if (j.contains("admin_metrics_public")) {
            if (!j["admin_metrics_public"].is_boolean()) {
              write_json(*sock, 400, "Bad Request",
                         {{"error", "admin_metrics_public must be boolean"}}, keep_alive);
              continue;
            }
            cfg_.admin_metrics_public = j["admin_metrics_public"].get<bool>();
          }
          write_json(*sock, 200, "OK",
                     {{"ok", true}, {"admin_metrics_public", cfg_.admin_metrics_public}},
                     keep_alive);
        } catch (...) {
          write_json(*sock, 400, "Bad Request", {{"error", "invalid JSON"}}, keep_alive);
        }
        continue;
      }
      if (method == "GET" && path == "/admin/api/s3/credentials") {
        if (!s3_iam_) {
          write_json(*sock, 404, "Not Found",
                     {{"error", "S3 IAM unavailable (enable s3_listen)"}}, keep_alive);
          continue;
        }
        write_json(*sock, 200, "OK", s3_iam_->list_redacted(), keep_alive);
        continue;
      }
      if (method == "POST" && path == "/admin/api/s3/credentials") {
        if (!s3_iam_) {
          write_json(*sock, 404, "Not Found",
                     {{"error", "S3 IAM unavailable (enable s3_listen)"}}, keep_alive);
          continue;
        }
        try {
          const std::string raw = body.empty()
                                      ? "{}"
                                      : std::string(reinterpret_cast<const char*>(body.data()),
                                                    body.size());
          auto j = nlohmann::json::parse(raw);
          S3Credential cred;
          cred.access_key_id = j.value("access_key_id", "");
          cred.secret = j.value("secret", "");
          cred.uid = j.value("uid", 0u);
          cred.gid = j.value("gid", 0u);
          if (j.contains("buckets") && j["buckets"].is_array()) {
            for (const auto& b : j["buckets"]) {
              if (b.is_string()) cred.buckets.push_back(b.get<std::string>());
            }
          } else if (j.contains("buckets") && j["buckets"].is_string()) {
            // Comma-separated convenience.
            std::string s = j["buckets"].get<std::string>();
            std::size_t i = 0;
            while (i < s.size()) {
              auto comma = s.find(',', i);
              if (comma == std::string::npos) comma = s.size();
              auto part = s.substr(i, comma - i);
              while (!part.empty() && part.front() == ' ') part.erase(part.begin());
              while (!part.empty() && part.back() == ' ') part.pop_back();
              if (!part.empty()) cred.buckets.push_back(part);
              i = comma + 1;
            }
          }
          std::string ierr;
          auto created = s3_iam_->create(std::move(cred), ierr);
          if (!created) {
            write_json(*sock, 400, "Bad Request", {{"error", ierr}}, keep_alive);
            continue;
          }
          write_json(*sock, 201, "Created",
                     {{"access_key_id", created->access_key_id},
                      {"secret", created->secret},
                      {"uid", created->uid},
                      {"gid", created->gid},
                      {"buckets", created->buckets}},
                     keep_alive);
        } catch (...) {
          write_json(*sock, 400, "Bad Request", {{"error", "invalid JSON"}}, keep_alive);
        }
        continue;
      }
      if (method == "DELETE" && path.rfind("/admin/api/s3/credentials/", 0) == 0) {
        if (!s3_iam_) {
          write_json(*sock, 404, "Not Found",
                     {{"error", "S3 IAM unavailable (enable s3_listen)"}}, keep_alive);
          continue;
        }
        const auto id = path.substr(std::string("/admin/api/s3/credentials/").size());
        std::string ierr;
        if (!s3_iam_->remove(id, ierr)) {
          const int status = (ierr == "not found") ? 404 : 400;
          write_json(*sock, status, status == 404 ? "Not Found" : "Bad Request",
                     {{"error", ierr}}, keep_alive);
          continue;
        }
        write_json(*sock, 200, "OK", {{"ok", true}, {"access_key_id", id}}, keep_alive);
        continue;
      }
      if (method == "GET" && path == "/admin/api/quota") {
        if (!quota_) {
          write_json(*sock, 404, "Not Found", {{"error", "quota admin unavailable"}}, keep_alive);
          continue;
        }
        write_json(*sock, 200, "OK", quota_->show(), keep_alive);
        continue;
      }
      if (method == "PUT" && path == "/admin/api/quota/limits") {
        if (!quota_) {
          write_json(*sock, 404, "Not Found", {{"error", "quota admin unavailable"}}, keep_alive);
          continue;
        }
        try {
          const std::string raw = body.empty()
                                      ? "{}"
                                      : std::string(reinterpret_cast<const char*>(body.data()),
                                                    body.size());
          auto j = nlohmann::json::parse(raw);
          std::string ierr;
          bool ok = true;
          if (j.contains("uid")) {
            std::optional<std::uint64_t> bytes;
            if (j.contains("bytes") && !j["bytes"].is_null())
              bytes = j["bytes"].get<std::uint64_t>();
            ok = quota_->set_volume_uid_limit(j["uid"].get<std::uint32_t>(), bytes, ierr);
          } else if (j.contains("gid")) {
            std::optional<std::uint64_t> bytes;
            if (j.contains("bytes") && !j["bytes"].is_null())
              bytes = j["bytes"].get<std::uint64_t>();
            ok = quota_->set_volume_gid_limit(j["gid"].get<std::uint32_t>(), bytes, ierr);
          } else {
            write_json(*sock, 400, "Bad Request", {{"error", "uid or gid required"}}, keep_alive);
            continue;
          }
          if (!ok) {
            write_json(*sock, 400, "Bad Request", {{"error", ierr}}, keep_alive);
            continue;
          }
          write_json(*sock, 200, "OK", {{"ok", true}}, keep_alive);
        } catch (...) {
          write_json(*sock, 400, "Bad Request", {{"error", "invalid JSON"}}, keep_alive);
        }
        continue;
      }
      if (method == "POST" && path == "/admin/api/quota/projects") {
        if (!quota_) {
          write_json(*sock, 404, "Not Found", {{"error", "quota admin unavailable"}}, keep_alive);
          continue;
        }
        try {
          const std::string raw = body.empty()
                                      ? "{}"
                                      : std::string(reinterpret_cast<const char*>(body.data()),
                                                    body.size());
          auto j = nlohmann::json::parse(raw);
          std::optional<std::uint64_t> bytes;
          if (j.contains("bytes") && !j["bytes"].is_null())
            bytes = j["bytes"].get<std::uint64_t>();
          std::uint32_t id = 0;
          std::string ierr;
          if (!quota_->create_project(j.value("name", ""), j.value("root_ino", 0ull), bytes, id,
                                      ierr)) {
            write_json(*sock, 400, "Bad Request", {{"error", ierr}}, keep_alive);
            continue;
          }
          write_json(*sock, 201, "Created", {{"id", id}, {"ok", true}}, keep_alive);
        } catch (...) {
          write_json(*sock, 400, "Bad Request", {{"error", "invalid JSON"}}, keep_alive);
        }
        continue;
      }
      if ((method == "DELETE" || method == "PUT") &&
          path.rfind("/admin/api/quota/projects/", 0) == 0) {
        if (!quota_) {
          write_json(*sock, 404, "Not Found", {{"error", "quota admin unavailable"}}, keep_alive);
          continue;
        }
        try {
          const auto id = static_cast<std::uint32_t>(
              std::stoul(path.substr(std::string("/admin/api/quota/projects/").size())));
          std::string ierr;
          if (method == "DELETE") {
            if (!quota_->delete_project(id, ierr)) {
              write_json(*sock, ierr == "not found" ? 404 : 400,
                         ierr == "not found" ? "Not Found" : "Bad Request", {{"error", ierr}},
                         keep_alive);
              continue;
            }
            write_json(*sock, 200, "OK", {{"ok", true}}, keep_alive);
            continue;
          }
          const std::string raw = body.empty()
                                      ? "{}"
                                      : std::string(reinterpret_cast<const char*>(body.data()),
                                                    body.size());
          auto j = nlohmann::json::parse(raw);
          if (!j.contains("uid")) {
            write_json(*sock, 400, "Bad Request", {{"error", "uid required"}}, keep_alive);
            continue;
          }
          std::optional<std::uint64_t> bytes;
          if (j.contains("bytes") && !j["bytes"].is_null())
            bytes = j["bytes"].get<std::uint64_t>();
          if (!quota_->set_project_uid_limit(id, j["uid"].get<std::uint32_t>(), bytes, ierr)) {
            write_json(*sock, ierr == "project not found" ? 404 : 400,
                       ierr == "project not found" ? "Not Found" : "Bad Request",
                       {{"error", ierr}}, keep_alive);
            continue;
          }
          write_json(*sock, 200, "OK", {{"ok", true}}, keep_alive);
        } catch (...) {
          write_json(*sock, 400, "Bad Request", {{"error", "bad project request"}}, keep_alive);
        }
        continue;
      }
      if (method == "POST" && path == "/admin/api/quota/reconcile") {
        if (!quota_) {
          write_json(*sock, 404, "Not Found", {{"error", "quota admin unavailable"}}, keep_alive);
          continue;
        }
        std::string ierr;
        if (!quota_->reconcile(ierr)) {
          write_json(*sock, 500, "Internal Server Error", {{"error", ierr}}, keep_alive);
          continue;
        }
        write_json(*sock, 200, "OK", {{"ok", true}, {"quota", quota_->show()}}, keep_alive);
        continue;
      }
      if (method == "GET" && path == "/admin/api/qos") {
        if (!qos_) {
          write_json(*sock, 404, "Not Found", {{"error", "qos admin unavailable"}}, keep_alive);
          continue;
        }
        write_json(*sock, 200, "OK", qos_->show(), keep_alive);
        continue;
      }
      if (method == "PUT" && path == "/admin/api/qos/limits") {
        if (!qos_) {
          write_json(*sock, 404, "Not Found", {{"error", "qos admin unavailable"}}, keep_alive);
          continue;
        }
        try {
          const std::string raw = body.empty()
                                      ? "{}"
                                      : std::string(reinterpret_cast<const char*>(body.data()),
                                                    body.size());
          auto j = nlohmann::json::parse(raw);
          std::string ierr;
          std::optional<std::uint64_t> iops, bps;
          bool clear_flag = j.value("clear", false);
          if (j.contains("iops") && !j["iops"].is_null()) iops = j["iops"].get<std::uint64_t>();
          if (j.contains("bps") && !j["bps"].is_null()) bps = j["bps"].get<std::uint64_t>();
          if (!clear_flag && !iops && !bps) {
            write_json(*sock, 400, "Bad Request",
                       {{"error", "iops and/or bps required (or clear=true)"}}, keep_alive);
            continue;
          }
          bool ok = false;
          if (j.contains("project_id")) {
            std::optional<std::uint32_t> uid;
            if (j.contains("uid")) uid = j["uid"].get<std::uint32_t>();
            ok = qos_->set_project(j["project_id"].get<std::uint32_t>(), uid, iops, bps, clear_flag,
                                   ierr);
          } else if (j.contains("uid")) {
            ok = qos_->set_volume_uid(j["uid"].get<std::uint32_t>(), iops, bps, clear_flag, ierr);
          } else if (j.contains("gid")) {
            ok = qos_->set_volume_gid(j["gid"].get<std::uint32_t>(), iops, bps, clear_flag, ierr);
          } else {
            write_json(*sock, 400, "Bad Request",
                       {{"error", "uid, gid, or project_id required"}}, keep_alive);
            continue;
          }
          if (!ok) {
            write_json(*sock, 400, "Bad Request", {{"error", ierr}}, keep_alive);
            continue;
          }
          write_json(*sock, 200, "OK", {{"ok", true}}, keep_alive);
        } catch (...) {
          write_json(*sock, 400, "Bad Request", {{"error", "invalid JSON"}}, keep_alive);
        }
        continue;
      }
      write_json(*sock, 404, "Not Found", {{"error", "unknown admin path"}}, keep_alive);
      continue;
    }

    if (method == "GET" && path == "/map") {
      write_json(*sock, 200, "OK", objects_.map().to_json(), keep_alive);
      continue;
    }

    // Prefix watch (primary-local): GET /watch?prefix=&timeout_ms=
    if (method == "GET" && path == "/watch") {
      const auto prefix = qmap.count("prefix") ? qmap["prefix"] : "";
      if (prefix.empty()) {
        write_json(*sock, 400, "Bad Request", {{"error", "prefix required"}}, keep_alive);
        continue;
      }
      int timeout_ms = 30000;
      if (qmap.count("timeout_ms")) {
        try {
          timeout_ms = std::stoi(qmap["timeout_ms"]);
        } catch (...) {
          write_json(*sock, 400, "Bad Request", {{"error", "bad timeout_ms"}}, keep_alive);
          continue;
        }
      }
      timeout_ms = std::max(1, std::min(timeout_ms, 120000));
      auto sock_ptr = sock;
      const std::string target_copy = target;
      const std::string label_copy = app_label;
      ObjectService* svc = &objects_;
      std::thread([sock_ptr, svc, prefix, timeout_ms, target_copy, label_copy]() {
        AppLabelScope scope(label_copy);
        auto r = svc->api_watch_prefix(prefix, timeout_ms);
        if (!r.ok) {
          write_api_error(*sock_ptr, r, target_copy, false);
          return;
        }
        if (r.code == "timeout") {
          write_response(*sock_ptr, 204, "No Content",
                         {{"x-aios-epoch", std::to_string(r.epoch)}}, nullptr, 0, false);
          return;
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : r.watch_events) {
          arr.push_back(
              {{"oid", e.oid}, {"seq", e.seq}, {"op", e.op}, {"ts_ms", e.ts_ms}});
        }
        write_json(*sock_ptr, 200, "OK", {{"events", arr}}, false);
      }).detach();
      return;
    }

    // Pub/sub: /pubsub/{topic}, /pubsub/{topic}/publish, /pubsub/{topic}/subscribe
    if (path.rfind("/pubsub/", 0) == 0) {
      const std::string rest = path.substr(8);
      std::string topic_raw = rest;
      std::string sub;
      if (rest.size() >= 8 && rest.compare(rest.size() - 8, 8, "/publish") == 0) {
        topic_raw = rest.substr(0, rest.size() - 8);
        sub = "publish";
      } else if (rest.size() >= 10 && rest.compare(rest.size() - 10, 10, "/subscribe") == 0) {
        topic_raw = rest.substr(0, rest.size() - 10);
        sub = "subscribe";
      }
      const std::string topic = url_decode(topic_raw);
      if (topic.empty()) {
        write_json(*sock, 400, "Bad Request", {{"error", "empty topic"}}, keep_alive);
        continue;
      }

      auto parse_delivery = [&]() -> std::optional<DeliveryMode> {
        if (!qmap.count("delivery")) return std::nullopt;
        return parse_delivery_mode(qmap.at("delivery"));
      };
      auto parse_capacity = [&]() -> std::optional<std::size_t> {
        if (!qmap.count("capacity")) return std::nullopt;
        try {
          const auto v = static_cast<std::size_t>(std::stoull(qmap.at("capacity")));
          return v;
        } catch (...) {
          return std::nullopt;
        }
      };

      if (sub.empty() && method == "PUT") {
        auto mode = parse_delivery();
        if (!mode) {
          if (qmap.count("delivery")) {
            write_json(*sock, 400, "Bad Request", {{"error", "bad delivery"}}, keep_alive);
            continue;
          }
          mode = DeliveryMode::Buffered;
        }
        std::size_t capacity = TopicHub::kDefaultCapacity;
        if (auto c = parse_capacity()) {
          capacity = *c;
        } else if (qmap.count("capacity")) {
          write_json(*sock, 400, "Bad Request", {{"error", "bad capacity"}}, keep_alive);
          continue;
        }
        auto r = objects_.api_pubsub_create(topic, *mode, capacity);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        write_json(*sock, 201, "Created", r.json_body.value_or(nlohmann::json::object()),
                   keep_alive);
        continue;
      }

      if (sub.empty() && method == "GET") {
        auto r = objects_.api_pubsub_stat(topic);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        write_json(*sock, 200, "OK", r.json_body.value_or(nlohmann::json::object()),
                   keep_alive);
        continue;
      }

      if (sub == "publish" && method == "POST") {
        if (!upload_path.empty() || body.size() > TopicHub::kMaxMessageBytes ||
            content_length > TopicHub::kMaxMessageBytes) {
          if (!upload_path.empty()) {
            std::error_code rec;
            fs::remove(upload_path, rec);
            upload_path.clear();
          }
          write_json(*sock, 413, "Payload Too Large",
                     {{"error", "message exceeds 1 MiB"}, {"code", "payload_too_large"}},
                     keep_alive);
          continue;
        }
        std::optional<DeliveryMode> mode = parse_delivery();
        if (qmap.count("delivery") && !mode) {
          write_json(*sock, 400, "Bad Request", {{"error", "bad delivery"}}, keep_alive);
          continue;
        }
        std::size_t capacity = TopicHub::kDefaultCapacity;
        if (auto c = parse_capacity()) {
          capacity = *c;
        } else if (qmap.count("capacity")) {
          write_json(*sock, 400, "Bad Request", {{"error", "bad capacity"}}, keep_alive);
          continue;
        }
        const std::string ct = header_get(headers, "content-type");
        auto r = objects_.api_pubsub_publish(topic, body.data(), body.size(), ct, mode, capacity);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        write_json(*sock, 201, "Created", r.json_body.value_or(nlohmann::json::object()),
                   keep_alive);
        continue;
      }

      if (sub == "subscribe" && method == "GET") {
        int timeout_ms = 30000;
        if (qmap.count("timeout_ms")) {
          try {
            timeout_ms = std::stoi(qmap["timeout_ms"]);
          } catch (...) {
            write_json(*sock, 400, "Bad Request", {{"error", "bad timeout_ms"}}, keep_alive);
            continue;
          }
        }
        timeout_ms = std::max(1, std::min(timeout_ms, 120000));
        std::uint64_t after_id = 0;
        bool after_set = false;
        if (qmap.count("after_id")) {
          try {
            after_id = static_cast<std::uint64_t>(std::stoull(qmap["after_id"]));
            after_set = true;
          } catch (...) {
            write_json(*sock, 400, "Bad Request", {{"error", "bad after_id"}}, keep_alive);
            continue;
          }
        }
        auto sock_ptr = sock;
        const std::string target_copy = target;
        const std::string label_copy = app_label;
        ObjectService* svc = &objects_;
        std::thread([sock_ptr, svc, topic, after_id, after_set, timeout_ms, target_copy,
                     label_copy]() {
          AppLabelScope scope(label_copy);
          auto r = svc->api_pubsub_subscribe(topic, after_id, after_set, timeout_ms);
          if (!r.ok) {
            write_api_error(*sock_ptr, r, target_copy, false);
            return;
          }
          if (r.code == "timeout") {
            write_response(*sock_ptr, 204, "No Content",
                           {{"x-aios-epoch", std::to_string(r.epoch)}}, nullptr, 0, false);
            return;
          }
          nlohmann::json arr = nlohmann::json::array();
          for (const auto& m : r.pub_messages) {
            arr.push_back({{"id", m.id},
                           {"ts_ms", m.ts_ms},
                           {"content_type", m.content_type},
                           {"data_b64", base64_encode(m.data)}});
          }
          write_json(*sock_ptr, 200, "OK", {{"topic", topic}, {"messages", arr}}, false);
        }).detach();
        return;
      }

      write_json(*sock, 404, "Not Found", {{"error", "not found"}}, keep_alive);
      continue;
    }

    // Cross-object transactions: /txn, /txn/{id}, /txn/{id}/commit|abort, /txn/{id}/o/{oid}
    if (path == "/txn" && method == "POST") {
      auto r = objects_.api_txn_begin();
      if (!r.ok) {
        write_api_error(*sock, r, target, keep_alive);
        continue;
      }
      nlohmann::json body = nlohmann::json::parse(
          std::string(r.data->begin(), r.data->end()));
      write_json(*sock, 201, "Created", body, keep_alive);
      continue;
    }
    if (path.rfind("/txn/", 0) == 0) {
      const std::string rest = path.substr(5);
      std::string txn_id = rest;
      std::string txn_sub;
      const auto slash = rest.find('/');
      if (slash != std::string::npos) {
        txn_id = rest.substr(0, slash);
        txn_sub = rest.substr(slash + 1);
      }
      if (txn_id.empty()) {
        write_json(*sock, 404, "Not Found", {{"error", "not found"}}, keep_alive);
        continue;
      }

      if (txn_sub.empty() && method == "GET") {
        auto r = objects_.api_txn_get(txn_id);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        write_json(*sock, 200, "OK",
                   nlohmann::json::parse(std::string(r.data->begin(), r.data->end())),
                   keep_alive);
        continue;
      }
      if (txn_sub == "commit" && method == "POST") {
        auto r = objects_.api_txn_commit(txn_id);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        write_json(*sock, 200, "OK",
                   nlohmann::json::parse(std::string(r.data->begin(), r.data->end())),
                   keep_alive);
        continue;
      }
      if (txn_sub == "abort" && method == "POST") {
        auto r = objects_.api_txn_abort(txn_id);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        write_json(*sock, 200, "OK",
                   nlohmann::json::parse(std::string(r.data->begin(), r.data->end())),
                   keep_alive);
        continue;
      }
      if (txn_sub.rfind("o/", 0) == 0) {
        const std::string oid = url_decode(txn_sub.substr(2));
        if (oid.empty()) {
          write_json(*sock, 400, "Bad Request", {{"error", "empty oid"}}, keep_alive);
          continue;
        }
        if (method == "PUT") {
          std::optional<std::string> lock_token;
          {
            const auto t = header_get(headers, "x-aios-lock-token");
            if (!t.empty()) lock_token = t;
          }
          ApiResult r;
          if (!upload_path.empty()) {
            r = objects_.api_txn_prepare_put_file(txn_id, oid, upload_path, content_length,
                                                  upload_crc, attrs, preds, std::nullopt,
                                                  lock_token);
            std::error_code rec;
            fs::remove(upload_path, rec);
            upload_path.clear();
          } else {
            r = objects_.api_txn_prepare_put(txn_id, oid, body.data(), body.size(), attrs,
                                             preds, std::nullopt, lock_token);
          }
          if (!r.ok) {
            write_api_error(*sock, r, target, keep_alive);
            continue;
          }
          nlohmann::json resp = {{"txn_id", txn_id},
                                 {"oid", oid},
                                 {"seq", r.info ? r.info->seq : 0},
                                 {"epoch", r.epoch}};
          write_json(*sock, 200, "OK", resp, keep_alive);
          continue;
        }
        if (method == "DELETE") {
          std::optional<std::string> lock_token;
          {
            const auto t = header_get(headers, "x-aios-lock-token");
            if (!t.empty()) lock_token = t;
          }
          auto r = objects_.api_txn_prepare_delete(txn_id, oid, preds, lock_token);
          if (!r.ok) {
            write_api_error(*sock, r, target, keep_alive);
            continue;
          }
          nlohmann::json resp = {{"txn_id", txn_id},
                                 {"oid", oid},
                                 {"seq", r.info ? r.info->seq : 0},
                                 {"epoch", r.epoch}};
          write_json(*sock, 200, "OK", resp, keep_alive);
          continue;
        }
      }
      write_json(*sock, 404, "Not Found", {{"error", "not found"}}, keep_alive);
      continue;
    }

    if (method == "GET" && path == "/o") {
      const auto prefix = qmap.count("prefix") ? qmap["prefix"] : "";
      std::string attr_key, attr_val;
      if (qmap.count("attr_eq")) {
        auto v = qmap["attr_eq"];
        auto c = v.find(':');
        if (c != std::string::npos) {
          attr_key = v.substr(0, c);
          attr_val = v.substr(c + 1);
        }
      }
      std::size_t limit = 1000;
      if (qmap.count("limit")) {
        try {
          limit = static_cast<std::size_t>(std::stoull(qmap["limit"]));
        } catch (...) {
        }
      }
      const auto cursor = qmap.count("cursor") ? qmap["cursor"] : "";
      const bool include_attrs = qmap.count("attrs") && qmap["attrs"] == "1";
      const bool cluster = !(qmap.count("scope") && qmap["scope"] == "local");
      auto r =
          objects_.api_list(prefix, attr_key, attr_val, limit, cursor, include_attrs, cluster);
      if (!r.ok) {
        write_api_error(*sock, r, target, keep_alive);
        continue;
      }
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& o : r.list.objects) {
        nlohmann::json jo = {{"oid", o.oid}, {"size", o.size}, {"mtime_ms", o.mtime_ms}};
        if (include_attrs) jo["attrs"] = o.attrs;
        arr.push_back(std::move(jo));
      }
      nlohmann::json resp = {{"objects", arr}, {"next_cursor", r.list.next_cursor}};
      write_json(*sock, 200, "OK", resp, keep_alive);
      continue;
    }

    if (path.rfind("/o/", 0) == 0) {
      const std::string rest = path.substr(3);
      std::string oid_raw = rest;
      std::string sub;
      const auto slash = rest.find('/');
      if (slash != std::string::npos) {
        oid_raw = rest.substr(0, slash);
        sub = rest.substr(slash + 1);
      }
      const std::string oid = url_decode(oid_raw);
      if (oid.empty()) {
        write_json(*sock, 400, "Bad Request", {{"error", "empty oid"}}, keep_alive);
        continue;
      }

      auto parse_version = [&]() -> std::optional<std::uint64_t> {
        std::string vs;
        if (qmap.count("version")) vs = qmap["version"];
        if (vs.empty()) vs = header_get(headers, "x-aios-version");
        if (vs.empty()) return std::nullopt;
        try {
          return static_cast<std::uint64_t>(std::stoull(vs));
        } catch (...) {
          return std::nullopt;
        }
      };

      if (sub == "versions" && method == "GET") {
        auto r = objects_.api_list_versions(oid);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& v : r.versions) {
          nlohmann::json j = {{"seq", v.seq},
                              {"size", v.size},
                              {"ctime_ms", v.ctime_ms},
                              {"is_delete", v.is_delete}};
          if (v.crc32c_known) j["crc32c"] = v.crc32c;
          if (!v.redirect_oid.empty()) j["redirect"] = v.redirect_oid;
          arr.push_back(std::move(j));
        }
        write_json(*sock, 200, "OK", {{"oid", oid}, {"versions", arr}}, keep_alive);
        continue;
      }

      if (sub == "purge" && method == "POST") {
        int keep = 0;
        if (qmap.count("keep")) {
          try {
            keep = std::stoi(qmap["keep"]);
          } catch (...) {
            write_json(*sock, 400, "Bad Request", {{"error", "bad keep"}}, keep_alive);
            continue;
          }
        }
        auto r = objects_.api_trim_versions(oid, keep);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        write_response(*sock, 204, "No Content",
                       {{"x-aios-epoch", std::to_string(r.epoch)}}, nullptr, 0, keep_alive);
        continue;
      }

      auto parse_ttl_ms = [&]() -> int {
        int ttl = LockTable::kDefaultTtlMs;
        const auto h = header_get(headers, "x-aios-lock-ttl-ms");
        if (!h.empty()) {
          try {
            ttl = std::stoi(h);
          } catch (...) {
            ttl = -1;
          }
        }
        return ttl;
      };
      auto lock_token_hdr = [&]() -> std::optional<std::string> {
        const auto t = header_get(headers, "x-aios-lock-token");
        if (t.empty()) return std::nullopt;
        return t;
      };

      if (sub == "append" && method == "POST") {
        if (!upload_path.empty()) {
          if (content_length > kMaxBodySize) {
            std::error_code rec;
            fs::remove(upload_path, rec);
            upload_path.clear();
            write_json(*sock, 413, "Payload Too Large", {{"error", "append body too large"}},
                       keep_alive);
            continue;
          }
          std::ifstream in(upload_path, std::ios::binary);
          body.resize(content_length);
          in.read(reinterpret_cast<char*>(body.data()),
                  static_cast<std::streamsize>(content_length));
          std::error_code rec;
          fs::remove(upload_path, rec);
          upload_path.clear();
        }
        const LayoutRequest layout_req = layout_request_from_headers(headers);
        const auto lock_token = lock_token_hdr();
        auto r = objects_.api_append(oid, body.data(), body.size(), attrs, false, preds,
                                     layout_req, lock_token);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        write_json(*sock, 200, "OK", r.json_body.value_or(nlohmann::json::object()),
                   keep_alive);
        continue;
      }

      if (sub == "lock" && method == "POST") {
        const int ttl = parse_ttl_ms();
        if (ttl < 0) {
          write_json(*sock, 400, "Bad Request", {{"error", "bad x-aios-lock-ttl-ms"}},
                     keep_alive);
          continue;
        }
        auto r = objects_.api_lock_acquire(oid, ttl);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        write_json(*sock, 201, "Created", r.json_body.value_or(nlohmann::json::object()),
                   keep_alive);
        continue;
      }
      if (sub == "lock/renew" && method == "POST") {
        const auto tok = lock_token_hdr();
        if (!tok) {
          write_json(*sock, 400, "Bad Request", {{"error", "x-aios-lock-token required"}},
                     keep_alive);
          continue;
        }
        const int ttl = parse_ttl_ms();
        if (ttl < 0) {
          write_json(*sock, 400, "Bad Request", {{"error", "bad x-aios-lock-ttl-ms"}},
                     keep_alive);
          continue;
        }
        auto r = objects_.api_lock_renew(oid, *tok, ttl);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        write_json(*sock, 200, "OK", r.json_body.value_or(nlohmann::json::object()),
                   keep_alive);
        continue;
      }
      if (sub == "lock" && method == "DELETE") {
        const auto tok = lock_token_hdr();
        if (!tok) {
          write_json(*sock, 400, "Bad Request", {{"error", "x-aios-lock-token required"}},
                     keep_alive);
          continue;
        }
        auto r = objects_.api_lock_release(oid, *tok);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        write_response(*sock, 204, "No Content",
                       {{"x-aios-epoch", std::to_string(r.epoch)}}, nullptr, 0, keep_alive);
        continue;
      }
      if (sub == "lock" && method == "GET") {
        auto r = objects_.api_lock_stat(oid);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        write_json(*sock, 200, "OK", r.json_body.value_or(nlohmann::json::object()),
                   keep_alive);
        continue;
      }

      if (sub == "watch" && method == "GET") {
        int timeout_ms = 30000;
        if (qmap.count("timeout_ms")) {
          try {
            timeout_ms = std::stoi(qmap["timeout_ms"]);
          } catch (...) {
            write_json(*sock, 400, "Bad Request", {{"error", "bad timeout_ms"}}, keep_alive);
            continue;
          }
        }
        timeout_ms = std::max(1, std::min(timeout_ms, 120000));
        std::uint64_t after_seq = 0;
        bool after_set = false;
        if (qmap.count("after_seq")) {
          try {
            after_seq = static_cast<std::uint64_t>(std::stoull(qmap["after_seq"]));
            after_set = true;
          } catch (...) {
            write_json(*sock, 400, "Bad Request", {{"error", "bad after_seq"}}, keep_alive);
            continue;
          }
        }
        // Default: wait for the next change after current tip (not the current tip).
        if (!after_set) {
          auto tip = objects_.api_head(oid, {});
          if (tip.ok && tip.info) after_seq = tip.info->seq;
        }
        auto sock_ptr = sock;
        const std::string target_copy = target;
        const std::string label_copy = app_label;
        ObjectService* svc = &objects_;
        std::thread([sock_ptr, svc, oid, after_seq, timeout_ms, target_copy, label_copy]() {
          AppLabelScope scope(label_copy);
          auto r = svc->api_watch_oid(oid, after_seq, timeout_ms);
          if (!r.ok) {
            write_api_error(*sock_ptr, r, target_copy, false);
            return;
          }
          if (r.code == "timeout") {
            write_response(*sock_ptr, 204, "No Content",
                           {{"x-aios-epoch", std::to_string(r.epoch)}}, nullptr, 0, false);
            return;
          }
          const auto& e = *r.watch_event;
          write_json(*sock_ptr, 200, "OK",
                     {{"oid", e.oid}, {"seq", e.seq}, {"op", e.op}, {"ts_ms", e.ts_ms}},
                     false);
        }).detach();
        return;
      }

      if (!sub.empty()) {
        write_json(*sock, 404, "Not Found", {{"error", "not found"}}, keep_alive);
        continue;
      }

      if (method == "PUT") {
        const auto redirect_to = header_get(headers, "x-aios-redirect");
        std::string cr = header_get(headers, "content-range");
        std::optional<std::uint32_t> expected_crc;
        const auto crc_hdr = header_get(headers, "x-aios-crc32c");
        if (!crc_hdr.empty()) {
          try {
            expected_crc = static_cast<std::uint32_t>(std::stoul(crc_hdr));
          } catch (...) {
            write_json(*sock, 400, "Bad Request", {{"error", "bad x-aios-crc32c"}},
                       keep_alive);
            continue;
          }
        }
        const LayoutRequest layout_req = layout_request_from_headers(headers);
        const auto lock_token = lock_token_hdr();
        ApiResult r;
        if (!redirect_to.empty()) {
          if (!cr.empty()) {
            write_json(*sock, 400, "Bad Request",
                       {{"error", "Content-Range not allowed with redirect"}}, keep_alive);
            continue;
          }
          if (!body.empty()) {
            write_json(*sock, 400, "Bad Request",
                       {{"error", "redirect PUT must have empty body"}}, keep_alive);
            continue;
          }
          r = objects_.api_put_redirect(oid, redirect_to, attrs, true, preds, lock_token);
        } else if (!cr.empty()) {
          if (!upload_path.empty()) {
            // Load staged range patch (capped at frame/object chunk size).
            if (content_length > kMaxBodySize) {
              std::error_code rec;
              fs::remove(upload_path, rec);
              upload_path.clear();
              write_json(*sock, 413, "Payload Too Large",
                         {{"error", "ranged PUT patch too large"}}, keep_alive);
              continue;
            }
            std::ifstream in(upload_path, std::ios::binary);
            body.resize(content_length);
            in.read(reinterpret_cast<char*>(body.data()),
                    static_cast<std::streamsize>(content_length));
            std::error_code rec;
            fs::remove(upload_path, rec);
            upload_path.clear();
            if (!in) {
              write_json(*sock, 500, "Error", {{"error", "read upload temp"}}, keep_alive);
              continue;
            }
          }
          std::uint64_t start = 0, end = 0;
          std::string perr;
          if (!parse_content_range(cr, start, end, perr)) {
            write_json(*sock, 400, "Bad Request", {{"error", perr}}, keep_alive);
            continue;
          }
          if (end - start + 1 != body.size()) {
            write_json(*sock, 400, "Bad Request",
                       {{"error", "Content-Range length mismatch"}}, keep_alive);
            continue;
          }
          if (expected_crc && crc32c(body.data(), body.size()) != *expected_crc) {
            write_json(*sock, 400, "Bad Request",
                       {{"error", "range crc32c mismatch"}, {"code", "crc_mismatch"}},
                       keep_alive);
            continue;
          }
          r = objects_.api_put_range(oid, start, body.data(), body.size(), attrs, false,
                                     preds, layout_req, lock_token);
        } else if (!upload_path.empty()) {
          r = objects_.api_put_file(oid, upload_path, content_length, upload_crc, attrs, true,
                                    preds, expected_crc, layout_req, lock_token);
          std::error_code rec;
          fs::remove(upload_path, rec);
          upload_path.clear();
        } else {
          r = objects_.api_put(oid, body.data(), body.size(), attrs, true, preds,
                               expected_crc, layout_req, lock_token);
        }
        if (!r.ok) {
          if (!upload_path.empty()) {
            std::error_code rec;
            fs::remove(upload_path, rec);
          }
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        std::unordered_map<std::string, std::string> rh = {
            {"x-aios-epoch", std::to_string(r.epoch)},
            {"x-aios-replicas", std::to_string(r.replicas)},
        };
        if (!r.redirect_oid.empty()) rh["x-aios-redirect"] = r.redirect_oid;
        if (r.info) {
          rh["x-aios-version"] = std::to_string(r.info->seq);
          if (r.info->crc32c_known) {
            rh["x-aios-crc32c"] = std::to_string(r.info->crc32c);
          }
        }
        write_response(*sock, 204, "No Content", rh, nullptr, 0, keep_alive);
        continue;
      }

      if (method == "GET" || method == "HEAD") {
        auto ver = parse_version();
        if (qmap.count("version") || !header_get(headers, "x-aios-version").empty()) {
          if (!ver.has_value()) {
            write_json(*sock, 400, "Bad Request", {{"error", "bad version"}}, keep_alive);
            continue;
          }
        }
        std::optional<std::uint64_t> off, end;
        const auto range = header_get(headers, "range");
        if (!range.empty()) {
          std::uint64_t start = 0;
          std::optional<std::uint64_t> end_opt;
          std::string perr;
          if (!parse_range(range, start, end_opt, perr)) {
            write_json(*sock, 400, "Bad Request", {{"error", perr}}, keep_alive);
            continue;
          }
          off = start;
          end = end_opt;
        }
        auto r = objects_.api_get(oid, off, end, preds, ver);
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        if (!r.redirect_oid.empty()) {
          std::unordered_map<std::string, std::string> rh = {
              {"Location", "/o/" + url_encode_path(r.redirect_oid)},
              {"x-aios-redirect", r.redirect_oid},
              {"x-aios-epoch", std::to_string(r.epoch)},
          };
          if (r.info) rh["x-aios-version"] = std::to_string(r.info->seq);
          write_response(*sock, 307, "Temporary Redirect", rh, nullptr, 0, keep_alive);
          continue;
        }
        std::unordered_map<std::string, std::string> rh = {
            {"Content-Type", "application/octet-stream"},
            {"x-aios-epoch", std::to_string(r.epoch)},
            {"Accept-Ranges", "bytes"},
        };
        if (r.info) {
          rh["x-aios-size"] = std::to_string(r.info->size);
          rh["x-aios-mtime-ms"] = std::to_string(r.info->mtime_ms);
          rh["x-aios-version"] = std::to_string(r.info->seq);
          if (r.info->crc32c_known) {
            rh["x-aios-crc32c"] = std::to_string(r.info->crc32c);
          }
        }
        for (const auto& [k, v] : r.attrs) {
          rh["x-aios-attr-" + k] = v;
        }

        if (off.has_value() && r.info && r.data) {
          const auto start = *off;
          const auto end_v = start + r.data->size() - 1;
          rh["Content-Range"] = "bytes " + std::to_string(start) + "-" +
                                std::to_string(end_v) + "/" + std::to_string(r.info->size);
          if (method == "HEAD") {
            write_response(*sock, 206, "Partial Content", rh, nullptr, r.data->size(),
                           keep_alive);
          } else {
            write_response(*sock, 206, "Partial Content", rh, r.data->data(), r.data->size(),
                           keep_alive);
          }
        } else if (!r.body_path.empty() && r.info) {
          write_file_body(*sock, 200, "OK", rh, r.body_path, r.info->size, method == "HEAD",
                          keep_alive);
        } else {
          const auto* p = (method == "HEAD" || !r.data) ? nullptr : r.data->data();
          const auto n = r.data ? r.data->size() : 0;
          write_response(*sock, 200, "OK", rh, p, n, keep_alive);
        }
        continue;
      }

      if (method == "DELETE") {
        auto ver = parse_version();
        if (qmap.count("version") || !header_get(headers, "x-aios-version").empty()) {
          if (!ver.has_value()) {
            write_json(*sock, 400, "Bad Request", {{"error", "bad version"}}, keep_alive);
            continue;
          }
          auto r = objects_.api_purge_version(oid, *ver, /*allow_tip=*/false);
          if (!r.ok) {
            write_api_error(*sock, r, target, keep_alive);
            continue;
          }
          write_response(*sock, 204, "No Content",
                         {{"x-aios-epoch", std::to_string(r.epoch)}}, nullptr, 0,
                         keep_alive);
          continue;
        }
        auto r = objects_.api_del(oid, preds, lock_token_hdr());
        if (!r.ok) {
          write_api_error(*sock, r, target, keep_alive);
          continue;
        }
        std::unordered_map<std::string, std::string> rh = {
            {"x-aios-epoch", std::to_string(r.epoch)},
        };
        if (r.info) rh["x-aios-version"] = std::to_string(r.info->seq);
        write_response(*sock, 204, "No Content", rh, nullptr, 0, keep_alive);
        continue;
      }
    }

    write_json(*sock, 404, "Not Found", {{"error", "not found"}}, keep_alive);
  }
}

}  // namespace aios
