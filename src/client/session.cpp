#include "client/session.hpp"

#include "http/http_auth.hpp"
#include "util/auth.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <cctype>
#include <sstream>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/time.h>
#endif

namespace aios {
namespace {

using tcp = boost::asio::ip::tcp;

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

void throw_http(const HttpResponse& resp, const std::string& what) {
  std::string code = "http";
  std::string msg = what + " status=" + std::to_string(resp.status);
  try {
    if (!resp.body.empty()) {
      auto j = nlohmann::json::parse(resp.body);
      if (j.contains("code")) code = j["code"].get<std::string>();
      if (j.contains("error")) msg = j["error"].get<std::string>();
    }
  } catch (...) {
  }
  if (resp.status == 412 || code == "precondition_failed") code = "conflict";
  if (resp.status == 409 && code == "lock_held") code = "lock_held";
  if (resp.status == 404) code = "not_found";
  throw client_error(code, msg);
}

// Fills CAS headers for aios.posix.cas. Returns new cas (expected+1), or 0 if unused.
std::uint64_t apply_posix_cas_headers(Session& session, const std::string& oid,
                                      std::optional<std::uint64_t> expected_cas,
                                      std::unordered_map<std::string, std::string>& headers) {
  if (!expected_cas.has_value()) return 0;
  const std::uint64_t new_cas = *expected_cas + 1;
  headers["x-aios-attr-aios.posix.cas"] = std::to_string(new_cas);
  if (*expected_cas == 0) {
    auto head = session.head_object(oid);
    if (!head.exists) {
      headers["if-none-match"] = "*";
    } else {
      auto it = head.attrs.find("aios.posix.cas");
      if (it == head.attrs.end()) {
        headers["if-match"] = "*";
        headers["x-aios-if-attr-absent"] = "aios.posix.cas";
      } else {
        throw client_error("conflict", "posix cas mismatch (expected 0)");
      }
    }
  } else {
    headers["if-match"] = "*";
    headers["x-aios-if-attr-eq"] = "aios.posix.cas=" + std::to_string(*expected_cas);
  }
  return new_cas;
}

}  // namespace

Session::Session(SessionConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.cluster_key.empty()) throw client_error("bad_request", "cluster_key required");
  parse_endpoint();
}

void Session::parse_endpoint() {
  auto colon = cfg_.endpoint.rfind(':');
  if (colon == std::string::npos) {
    throw client_error("bad_request", "endpoint must be HOST:PORT");
  }
  host_ = cfg_.endpoint.substr(0, colon);
  port_ = cfg_.endpoint.substr(colon + 1);
}

std::string Session::url_encode_oid(const std::string& oid) {
  // Encode '/' as %2F so /o/{oid}/lock|watch stays unambiguous.
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : oid) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xf]);
    }
  }
  return out;
}

std::string Session::stl_oid(const std::string& type, const std::string& name) {
  if (name.empty()) throw client_error("bad_request", "empty stl name");
  return "stl/" + type + "/" + name;
}

void Session::validate_header_value(const std::string& value, const char* what) {
  for (char c : value) {
    if (c == '\r' || c == '\n' || c == '\0') {
      throw client_error("bad_request", std::string("invalid characters in ") + what);
    }
  }
}

void Session::add_auth(std::unordered_map<std::string, std::string>& headers,
                       const std::string& method, const std::string& target,
                       const std::string& body) const {
  const std::string date = std::to_string(now_ms());
  headers["x-aios-date"] = date;
  const std::string payload_hash = sha256_hex(body);
  headers["x-aios-content-sha256"] = payload_hash;
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  const auto canon =
      http_canonical(method, target, date, signed_headers, headers, payload_hash);
  const auto sig = http_sign(cfg_.cluster_key, canon);
  headers["authorization"] = "AIOS-HMAC-SHA256 Credential=stl, SignedHeaders=" +
                             signed_headers + ", Signature=" + sig;
}

HttpResponse Session::request(const std::string& method, const std::string& target,
                              std::unordered_map<std::string, std::string> headers,
                              const std::string& body, int max_redirects) {
  if (body.size() > kMaxBodyBytes) {
    throw client_error("payload_too_large", "request body exceeds 16 MiB");
  }
  for (const auto& [k, v] : headers) {
    validate_header_value(k, "header name");
    validate_header_value(v, "header value");
  }
  if (!cfg_.app_label.empty()) validate_header_value(cfg_.app_label, "app label");

  std::string host = host_;
  std::string port = port_;
  std::string path = target;
  HttpResponse resp;

  for (int hop = 0; hop <= max_redirects; ++hop) {
    headers.erase("authorization");
    headers.erase("x-aios-date");
    headers["content-length"] = std::to_string(body.size());
    if (!cfg_.app_label.empty()) headers["x-aios-app-label"] = cfg_.app_label;
    add_auth(headers, method, path, body);

    boost::asio::io_context ioc;
    boost::system::error_code ec;
    tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(host, port, ec);
    if (ec) throw client_error("http", "resolve: " + ec.message());

    tcp::socket sock(ioc);
    boost::asio::connect(sock, endpoints, ec);
    if (ec) throw client_error("http", "connect: " + ec.message());

#ifndef _WIN32
    if (cfg_.socket_timeout_ms > 0) {
      struct timeval tv {};
      tv.tv_sec = cfg_.socket_timeout_ms / 1000;
      tv.tv_usec = (cfg_.socket_timeout_ms % 1000) * 1000;
      const int fd = sock.native_handle();
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
#endif

    std::ostringstream req;
    req << method << ' ' << path << " HTTP/1.1\r\n";
    req << "Host: " << host << ':' << port << "\r\n";
    req << "Connection: close\r\n";
    for (const auto& [k, v] : headers) req << k << ": " << v << "\r\n";
    req << "\r\n";
    auto head = req.str();
    boost::asio::write(sock, boost::asio::buffer(head), ec);
    if (!ec && !body.empty()) boost::asio::write(sock, boost::asio::buffer(body), ec);
    if (ec) throw client_error("http", "write: " + ec.message());

    boost::asio::streambuf buf;
    boost::asio::read_until(sock, buf, "\r\n\r\n", ec);
    if (ec) throw client_error("http", "read headers: " + ec.message());

    std::istream is(&buf);
    std::string status_line;
    std::getline(is, status_line);
    resp = HttpResponse{};
    {
      std::istringstream ss(status_line);
      std::string ver, reason;
      ss >> ver >> resp.status;
    }
    std::string line;
    std::size_t content_length = 0;
    bool have_content_length = false;
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
        have_content_length = true;
        if (value.empty() || value.front() == '-') {
          throw client_error("http", "invalid Content-Length");
        }
        try {
          const auto n = std::stoull(value);
          if (n > kMaxBodyBytes) {
            throw client_error("payload_too_large", "response body exceeds 16 MiB");
          }
          content_length = static_cast<std::size_t>(n);
        } catch (const client_error&) {
          throw;
        } catch (...) {
          throw client_error("http", "invalid Content-Length");
        }
      }
    }
    std::string already(std::istreambuf_iterator<char>(is), {});
    resp.body = already;
    while (resp.body.size() < content_length) {
      char tmp[4096];
      const auto n = sock.read_some(boost::asio::buffer(tmp), ec);
      if (n > 0) resp.body.append(tmp, tmp + n);
      if (ec) break;
    }
    if (content_length > 0 && resp.body.size() > content_length) resp.body.resize(content_length);
    if (have_content_length && method != "HEAD" && resp.status != 204 &&
        resp.body.size() != content_length) {
      throw client_error("http", "short response body");
    }

    if (resp.status == 307 || resp.status == 301 || resp.status == 302) {
      const auto loc = header_get(resp.headers, "location");
      std::string new_path;
      if (!parse_location(loc, host, port, new_path)) {
        throw client_error("http", "bad redirect Location");
      }
      path = new_path;
      continue;
    }
    return resp;
  }
  throw client_error("http", "too many redirects");
}

ObjectSnapshot Session::parse_object_meta(const HttpResponse& resp, bool with_body) {
  ObjectSnapshot snap;
  if (resp.status == 404) {
    snap.exists = false;
    return snap;
  }
  if (resp.status != 200 && resp.status != 204 && resp.status != 206) {
    throw_http(resp, "object get/head");
  }
  snap.exists = true;
  const auto ver = header_get(resp.headers, "x-aios-version");
  if (!ver.empty()) {
    try {
      snap.seq = static_cast<std::uint64_t>(std::stoull(ver));
    } catch (...) {
    }
  }
  const auto sz = header_get(resp.headers, "x-aios-size");
  if (!sz.empty()) {
    try {
      snap.size = static_cast<std::uint64_t>(std::stoull(sz));
    } catch (...) {
    }
  }
  for (const auto& [k, v] : resp.headers) {
    if (k.rfind("x-aios-attr-", 0) == 0) {
      snap.attrs[k.substr(12)] = v;
    }
  }
  auto it = snap.attrs.find("aios.stl.cas");
  if (it != snap.attrs.end()) {
    try {
      snap.cas = static_cast<std::uint64_t>(std::stoull(it->second));
    } catch (...) {
    }
  }
  if (with_body) snap.body = resp.body;
  if (snap.size == 0 && with_body) snap.size = snap.body.size();
  return snap;
}

ObjectSnapshot Session::get_object(const std::string& oid) {
  const auto path = "/o/" + url_encode_oid(oid);
  auto resp = request("GET", path);
  if (resp.status == 404) return ObjectSnapshot{};
  return parse_object_meta(resp, true);
}

ObjectSnapshot Session::head_object(const std::string& oid) {
  const auto path = "/o/" + url_encode_oid(oid);
  auto resp = request("HEAD", path);
  if (resp.status == 404) return ObjectSnapshot{};
  return parse_object_meta(resp, false);
}

ObjectSnapshot Session::get_range(const std::string& oid, std::uint64_t start,
                                  std::uint64_t end_inclusive) {
  if (end_inclusive < start) {
    throw client_error("bad_request", "invalid get_range bounds");
  }
  const auto path = "/o/" + url_encode_oid(oid);
  std::unordered_map<std::string, std::string> headers;
  headers["range"] = "bytes=" + std::to_string(start) + "-" + std::to_string(end_inclusive);
  auto resp = request("GET", path, headers);
  if (resp.status == 404) return ObjectSnapshot{};
  if (resp.status == 416) throw client_error("range_unsatisfiable", "get_range unsatisfiable");
  return parse_object_meta(resp, true);
}

std::uint64_t Session::put_bytes(const std::string& oid, const std::string& body,
                                 const std::unordered_map<std::string, std::string>& attrs,
                                 std::optional<std::uint64_t> expected_cas,
                                 const std::optional<std::string>& lock_token,
                                 const PutLayout& layout) {
  if (body.size() > kMaxBodyBytes) {
    throw client_error("payload_too_large", "put_bytes exceeds 16 MiB");
  }
  std::unordered_map<std::string, std::string> headers;
  headers["content-type"] = "application/octet-stream";
  for (const auto& [k, v] : attrs) {
    validate_header_value(k, "attribute name");
    validate_header_value(v, "attribute value");
    headers["x-aios-attr-" + k] = v;
  }
  apply_put_layout_headers(headers, layout);
  const std::uint64_t new_cas = apply_posix_cas_headers(*this, oid, expected_cas, headers);
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }
  const auto path = "/o/" + url_encode_oid(oid);
  auto resp = request("PUT", path, headers, body);
  if (resp.status != 204 && resp.status != 200 && resp.status != 201) {
    throw_http(resp, "put_bytes");
  }
  return new_cas;
}

void Session::put_range(const std::string& oid, std::uint64_t offset, const std::string& data,
                        const std::optional<std::string>& lock_token) {
  if (data.empty()) return;
  if (data.size() > kMaxBodyBytes) {
    throw client_error("payload_too_large", "put_range exceeds 16 MiB");
  }
  const std::uint64_t end = offset + static_cast<std::uint64_t>(data.size()) - 1;
  std::unordered_map<std::string, std::string> headers;
  headers["content-type"] = "application/octet-stream";
  headers["content-range"] =
      "bytes " + std::to_string(offset) + "-" + std::to_string(end) + "/*";
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }
  const auto path = "/o/" + url_encode_oid(oid);
  auto resp = request("PUT", path, headers, data);
  if (resp.status != 204 && resp.status != 200 && resp.status != 201) {
    throw_http(resp, "put_range");
  }
}

void Session::delete_object(const std::string& oid,
                            const std::optional<std::string>& lock_token) {
  std::unordered_map<std::string, std::string> headers;
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }
  const auto path = "/o/" + url_encode_oid(oid);
  auto resp = request("DELETE", path, headers);
  if (resp.status == 404) return;
  if (resp.status != 204 && resp.status != 200) throw_http(resp, "delete_object");
}

ListResult Session::list_prefix(const std::string& prefix, std::size_t limit,
                                const std::string& cursor) {
  std::string target = "/o?limit=" + std::to_string(limit > 0 ? limit : 256);
  if (!prefix.empty()) target += "&prefix=" + url_encode_oid(prefix);
  if (!cursor.empty()) target += "&cursor=" + url_encode_oid(cursor);
  auto resp = request("GET", target);
  if (resp.status != 200) throw_http(resp, "list_prefix");
  ListResult out;
  try {
    auto j = nlohmann::json::parse(resp.body);
    out.next_cursor = j.value("next_cursor", "");
    if (j.contains("objects") && j["objects"].is_array()) {
      for (const auto& o : j["objects"]) {
        ListObject e;
        e.oid = o.value("oid", "");
        e.size = o.value("size", static_cast<std::uint64_t>(0));
        e.mtime_ms = o.value("mtime_ms", static_cast<std::int64_t>(0));
        if (!e.oid.empty()) out.objects.push_back(std::move(e));
      }
    }
  } catch (const client_error&) {
    throw;
  } catch (...) {
    throw client_error("http", "bad list response");
  }
  return out;
}

std::uint64_t Session::put_object(const std::string& oid, const std::string& body,
                                  const std::string& stl_type, std::uint64_t expected_cas,
                                  const std::optional<std::string>& lock_token, int stl_v) {
  if (body.size() > kMaxBodyBytes) {
    throw client_error("payload_too_large", "stl object exceeds 16 MiB");
  }
  const std::uint64_t new_cas = expected_cas + 1;
  std::unordered_map<std::string, std::string> headers;
  headers["content-type"] = "application/json";
  headers["x-aios-attr-aios.stl.type"] = stl_type;
  headers["x-aios-attr-aios.stl.v"] = std::to_string(stl_v);
  headers["x-aios-attr-aios.stl.cas"] = std::to_string(new_cas);
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }

  if (expected_cas == 0) {
    // Create if absent, or first write when cas attr missing.
    auto head = head_object(oid);
    if (!head.exists) {
      headers["if-none-match"] = "*";
    } else if (head.cas == 0) {
      headers["if-match"] = "*";
      headers["x-aios-if-attr-absent"] = "aios.stl.cas";
    } else {
      throw client_error("conflict", "stl cas mismatch (expected 0)");
    }
  } else {
    headers["if-match"] = "*";
    headers["x-aios-if-attr-eq"] = "aios.stl.cas=" + std::to_string(expected_cas);
  }

  const auto path = "/o/" + url_encode_oid(oid);
  auto resp = request("PUT", path, headers, body);
  if (resp.status != 204 && resp.status != 200 && resp.status != 201) {
    throw_http(resp, "put_object");
  }
  return new_cas;
}

AppendResult Session::append(const std::string& oid, const std::string& data,
                             const std::optional<std::string>& lock_token) {
  if (data.size() > kMaxBodyBytes) {
    throw client_error("payload_too_large", "append exceeds 16 MiB");
  }
  std::unordered_map<std::string, std::string> headers;
  headers["content-type"] = "application/octet-stream";
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }
  const auto path = "/o/" + url_encode_oid(oid) + "/append";
  auto resp = request("POST", path, headers, data);
  if (resp.status != 200) throw_http(resp, "append");
  try {
    auto j = nlohmann::json::parse(resp.body);
    AppendResult ar;
    ar.offset = j.at("offset").get<std::uint64_t>();
    ar.size = j.at("size").get<std::uint64_t>();
    ar.seq = j.value("seq", static_cast<std::uint64_t>(0));
    ar.epoch = j.value("epoch", static_cast<std::uint64_t>(0));
    return ar;
  } catch (const client_error&) {
    throw;
  } catch (...) {
    throw client_error("http", "bad append response");
  }
}

LockResult Session::lock_acquire(const std::string& oid, int ttl_ms) {
  std::unordered_map<std::string, std::string> headers;
  headers["x-aios-lock-ttl-ms"] = std::to_string(ttl_ms);
  const auto path = "/o/" + url_encode_oid(oid) + "/lock";
  auto resp = request("POST", path, headers);
  if (resp.status != 201) throw_http(resp, "lock_acquire");
  try {
    auto j = nlohmann::json::parse(resp.body);
    LockResult out;
    out.token = j.at("token").get<std::string>();
    out.expires_ms = j.value("expires_ms", static_cast<std::int64_t>(0));
    return out;
  } catch (...) {
    throw client_error("http", "bad lock response");
  }
}

bool Session::lock_try_acquire(const std::string& oid, std::string& token_out, int ttl_ms,
                               std::int64_t* expires_ms_out) {
  try {
    const auto lr = lock_acquire(oid, ttl_ms);
    token_out = lr.token;
    if (expires_ms_out) *expires_ms_out = lr.expires_ms;
    return true;
  } catch (const client_error& e) {
    if (e.code() == "lock_held") return false;
    throw;
  }
}

void Session::lock_renew(const std::string& oid, const std::string& token, int ttl_ms,
                         std::int64_t* expires_ms_out) {
  std::unordered_map<std::string, std::string> headers;
  headers["x-aios-lock-ttl-ms"] = std::to_string(ttl_ms);
  validate_header_value(token, "lock token");
  headers["x-aios-lock-token"] = token;
  const auto path = "/o/" + url_encode_oid(oid) + "/lock/renew";
  auto resp = request("POST", path, headers);
  if (resp.status != 200) throw_http(resp, "lock_renew");
  if (expires_ms_out) {
    try {
      auto j = nlohmann::json::parse(resp.body);
      *expires_ms_out = j.value("expires_ms", static_cast<std::int64_t>(0));
    } catch (...) {
      *expires_ms_out = 0;
    }
  }
}

void Session::lock_release(const std::string& oid, const std::string& token) {
  std::unordered_map<std::string, std::string> headers;
  headers["x-aios-lock-token"] = token;
  const auto path = "/o/" + url_encode_oid(oid) + "/lock";
  auto resp = request("DELETE", path, headers);
  if (resp.status != 204 && resp.status != 200) throw_http(resp, "lock_release");
}

std::string Session::txn_begin() {
  auto resp = request("POST", "/txn");
  if (resp.status != 201 && resp.status != 200) throw_http(resp, "txn_begin");
  try {
    auto j = nlohmann::json::parse(resp.body);
    auto id = j.value("txn_id", "");
    if (id.empty()) throw client_error("http", "txn_begin missing txn_id");
    return id;
  } catch (const client_error&) {
    throw;
  } catch (...) {
    throw client_error("http", "bad txn_begin response");
  }
}

void Session::txn_prepare_put(const std::string& txn_id, const std::string& oid,
                              const std::string& body,
                              std::optional<std::uint64_t> expected_cas,
                              const std::optional<std::string>& lock_token,
                              const std::unordered_map<std::string, std::string>& attrs) {
  if (txn_id.empty() || oid.empty()) throw client_error("bad_request", "txn_prepare_put args");
  if (body.size() > kMaxBodyBytes) {
    throw client_error("payload_too_large", "txn_prepare_put exceeds 16 MiB");
  }
  std::unordered_map<std::string, std::string> headers;
  headers["content-type"] = "application/octet-stream";
  for (const auto& [k, v] : attrs) {
    validate_header_value(k, "attribute name");
    validate_header_value(v, "attribute value");
    headers["x-aios-attr-" + k] = v;
  }
  apply_posix_cas_headers(*this, oid, expected_cas, headers);
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }
  const auto path = "/txn/" + url_encode_oid(txn_id) + "/o/" + url_encode_oid(oid);
  auto resp = request("PUT", path, headers, body);
  if (resp.status != 200 && resp.status != 201 && resp.status != 204) {
    throw_http(resp, "txn_prepare_put");
  }
}

void Session::txn_prepare_delete(const std::string& txn_id, const std::string& oid,
                                 const std::optional<std::string>& lock_token) {
  if (txn_id.empty() || oid.empty()) {
    throw client_error("bad_request", "txn_prepare_delete args");
  }
  std::unordered_map<std::string, std::string> headers;
  if (lock_token) {
    validate_header_value(*lock_token, "lock token");
    headers["x-aios-lock-token"] = *lock_token;
  }
  const auto path = "/txn/" + url_encode_oid(txn_id) + "/o/" + url_encode_oid(oid);
  auto resp = request("DELETE", path, headers);
  if (resp.status != 200 && resp.status != 201 && resp.status != 204) {
    throw_http(resp, "txn_prepare_delete");
  }
}

void Session::txn_commit(const std::string& txn_id) {
  if (txn_id.empty()) throw client_error("bad_request", "empty txn_id");
  auto resp = request("POST", "/txn/" + url_encode_oid(txn_id) + "/commit");
  if (resp.status != 200) throw_http(resp, "txn_commit");
}

void Session::txn_abort(const std::string& txn_id) {
  if (txn_id.empty()) throw client_error("bad_request", "empty txn_id");
  auto resp = request("POST", "/txn/" + url_encode_oid(txn_id) + "/abort");
  if (resp.status != 200 && resp.status != 204) throw_http(resp, "txn_abort");
}

}  // namespace aios
