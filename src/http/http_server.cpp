#include "http/http_server.hpp"

#include "http/http_auth.hpp"
#include "net/framing.hpp"
#include "util/auth.hpp"
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
#include <unordered_map>
#include <vector>

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
  if (r.code == "no_targets") return 503;
  if (r.code == "quorum_failed") return 503;
  if (r.code == "bad_request") return 400;
  if (r.code == "conflict") return 409;
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

}  // namespace

HttpServer::HttpServer(boost::asio::io_context& ioc, Config cfg, ObjectService& objects)
    : ioc_(ioc), cfg_(std::move(cfg)), objects_(objects), acceptor_(ioc) {
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

    requests_.fetch_add(1);

    std::string path = target;
    std::string query;
    auto qpos = target.find('?');
    if (qpos != std::string::npos) {
      path = target.substr(0, qpos);
      query = target.substr(qpos + 1);
    }
    auto qmap = parse_query(query);

    // Auth
    const bool unsigned_payload =
        header_get(headers, "x-aios-content-sha256") == "UNSIGNED-PAYLOAD";
    const std::string payload_hash =
        unsigned_payload ? "UNSIGNED-PAYLOAD" : sha256_hex(body.data(), body.size());
    auto auth = http_auth_verify(method, target, headers, payload_hash, cfg_.cluster_key,
                                 cfg_.auth_skew_ms);
    if (!auth.ok) {
      write_json(*sock, 401, "Unauthorized", {{"error", auth.error}}, keep_alive);
      continue;
    }

    auto preds = parse_preconditions(headers);
    auto attrs = parse_attrs(headers);

    if (method == "GET" && path == "/map") {
      write_json(*sock, 200, "OK", objects_.map().to_json(), keep_alive);
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
          ApiResult r;
          if (!upload_path.empty()) {
            r = objects_.api_txn_prepare_put_file(txn_id, oid, upload_path, content_length,
                                                  upload_crc, attrs, preds, std::nullopt);
            std::error_code rec;
            fs::remove(upload_path, rec);
            upload_path.clear();
          } else {
            r = objects_.api_txn_prepare_put(txn_id, oid, body.data(), body.size(), attrs,
                                             preds, std::nullopt);
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
          auto r = objects_.api_txn_prepare_delete(txn_id, oid, preds);
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
          r = objects_.api_put_redirect(oid, redirect_to, attrs, true, preds);
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
                                     preds);
        } else if (!upload_path.empty()) {
          r = objects_.api_put_file(oid, upload_path, content_length, upload_crc, attrs, true,
                                    preds, expected_crc);
          std::error_code rec;
          fs::remove(upload_path, rec);
          upload_path.clear();
        } else {
          r = objects_.api_put(oid, body.data(), body.size(), attrs, true, preds,
                               expected_crc);
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
        auto r = objects_.api_del(oid, preds);
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
