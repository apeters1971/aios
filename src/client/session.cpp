#include "client/session.hpp"

#include "http/http_auth.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <cctype>
#include <sstream>

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

void Session::add_auth(std::unordered_map<std::string, std::string>& headers,
                       const std::string& method, const std::string& target) const {
  const std::string date = std::to_string(now_ms());
  headers["x-aios-date"] = date;
  headers["x-aios-content-sha256"] = "UNSIGNED-PAYLOAD";
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  const auto canon =
      http_canonical(method, target, date, signed_headers, headers, "UNSIGNED-PAYLOAD");
  const auto sig = http_sign(cfg_.cluster_key, canon);
  headers["authorization"] = "AIOS-HMAC-SHA256 Credential=stl, SignedHeaders=" +
                             signed_headers + ", Signature=" + sig;
}

HttpResponse Session::request(const std::string& method, const std::string& target,
                              std::unordered_map<std::string, std::string> headers,
                              const std::string& body, int max_redirects) {
  std::string host = host_;
  std::string port = port_;
  std::string path = target;
  HttpResponse resp;

  for (int hop = 0; hop <= max_redirects; ++hop) {
    headers.erase("authorization");
    headers.erase("x-aios-date");
    headers["content-length"] = std::to_string(body.size());
    add_auth(headers, method, path);

    boost::asio::io_context ioc;
    boost::system::error_code ec;
    tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(host, port, ec);
    if (ec) throw client_error("http", "resolve: " + ec.message());

    tcp::socket sock(ioc);
    boost::asio::connect(sock, endpoints, ec);
    if (ec) throw client_error("http", "connect: " + ec.message());

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
        try {
          content_length = static_cast<std::size_t>(std::stoull(value));
        } catch (...) {
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
  if (lock_token) headers["x-aios-lock-token"] = *lock_token;

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
  if (lock_token) headers["x-aios-lock-token"] = *lock_token;
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

std::string Session::lock_acquire(const std::string& oid, int ttl_ms) {
  std::unordered_map<std::string, std::string> headers;
  headers["x-aios-lock-ttl-ms"] = std::to_string(ttl_ms);
  const auto path = "/o/" + url_encode_oid(oid) + "/lock";
  auto resp = request("POST", path, headers);
  if (resp.status != 201) throw_http(resp, "lock_acquire");
  try {
    auto j = nlohmann::json::parse(resp.body);
    return j.at("token").get<std::string>();
  } catch (...) {
    throw client_error("http", "bad lock response");
  }
}

bool Session::lock_try_acquire(const std::string& oid, std::string& token_out, int ttl_ms) {
  try {
    token_out = lock_acquire(oid, ttl_ms);
    return true;
  } catch (const client_error& e) {
    if (e.code() == "lock_held") return false;
    throw;
  }
}

void Session::lock_renew(const std::string& oid, const std::string& token, int ttl_ms) {
  std::unordered_map<std::string, std::string> headers;
  headers["x-aios-lock-ttl-ms"] = std::to_string(ttl_ms);
  headers["x-aios-lock-token"] = token;
  const auto path = "/o/" + url_encode_oid(oid) + "/lock/renew";
  auto resp = request("POST", path, headers);
  if (resp.status != 200) throw_http(resp, "lock_renew");
}

void Session::lock_release(const std::string& oid, const std::string& token) {
  std::unordered_map<std::string, std::string> headers;
  headers["x-aios-lock-token"] = token;
  const auto path = "/o/" + url_encode_oid(oid) + "/lock";
  auto resp = request("DELETE", path, headers);
  if (resp.status != 204 && resp.status != 200) throw_http(resp, "lock_release");
}

}  // namespace aios
