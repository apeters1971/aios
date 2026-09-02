#include "http/http_auth.hpp"

#include "util/auth.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

#include <time.h>

namespace aios {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool const_time_eq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

// Parse an auth date into unix milliseconds. Accepts unix-ms integers plus the
// calendar formats curl and the AWS SDKs emit, so that every accepted date can be
// skew-checked rather than exempted from it.
bool parse_auth_date_ms(const std::string& date, std::int64_t& out_ms) {
  if (date.empty()) return false;
  if (date.find_first_not_of("0123456789") == std::string::npos) {
    try {
      out_ms = std::stoll(date);
    } catch (...) {
      return false;
    }
    return true;
  }
  static constexpr const char* kFormats[] = {
      "%a, %d %b %Y %H:%M:%S",  // RFC 7231 IMF-fixdate
      "%Y%m%dT%H%M%SZ",         // ISO 8601 basic, as in x-amz-date
      "%Y-%m-%dT%H:%M:%SZ",     // ISO 8601 extended
  };
  for (const char* fmt : kFormats) {
    std::tm tm{};
    if (::strptime(date.c_str(), fmt, &tm) == nullptr) continue;
    const std::time_t secs = ::timegm(&tm);
    if (secs == static_cast<std::time_t>(-1)) continue;
    out_ms = static_cast<std::int64_t>(secs) * 1000;
    return true;
  }
  return false;
}

std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (!std::isspace(static_cast<unsigned char>(c))) {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

std::string header_get(const std::unordered_map<std::string, std::string>& headers,
                       const std::string& name) {
  const auto key = lower(name);
  auto it = headers.find(key);
  if (it == headers.end()) return {};
  return it->second;
}

std::string http_canonical(const std::string& method, const std::string& path_with_query,
                           const std::string& date, const std::string& signed_headers,
                           const std::unordered_map<std::string, std::string>& headers,
                           const std::string& payload_hash_hex) {
  std::ostringstream oss;
  oss << method << '\n' << path_with_query << '\n' << date << '\n';
  /* Comma-split only. "a;b" is one name — kernel/aios_http/auth.c matches this. */
  auto names = split_csv(signed_headers);
  for (auto& n : names) n = lower(n);
  std::sort(names.begin(), names.end());
  for (const auto& n : names) {
    oss << n << ':' << header_get(headers, n) << '\n';
  }
  oss << signed_headers << '\n' << payload_hash_hex;
  const std::string nonce = header_get(headers, kHttpNonceHeader);
  if (!nonce.empty()) oss << '\n' << kHttpNonceHeader << ':' << nonce;
  return oss.str();
}

std::string http_sign(const std::string& cluster_key, const std::string& canonical) {
  return hmac_sha256_hex(cluster_key, canonical);
}

bool HttpReplayCache::check_and_insert(const std::string& key, std::int64_t expires_at_ms,
                                       std::int64_t now) {
  std::lock_guard lock(mu_);
  if (++inserts_since_sweep_ >= 1024 || entries_.size() >= kMaxEntries) {
    evict_expired(now);
    inserts_since_sweep_ = 0;
  }
  auto it = entries_.find(key);
  if (it != entries_.end()) {
    if (it->second > now) return false;
    it->second = expires_at_ms;
    return true;
  }
  while (entries_.size() >= kMaxEntries) entries_.erase(entries_.begin());
  entries_.emplace(key, expires_at_ms);
  return true;
}

void HttpReplayCache::clear() {
  std::lock_guard lock(mu_);
  entries_.clear();
}

std::size_t HttpReplayCache::size() const {
  std::lock_guard lock(mu_);
  return entries_.size();
}

void HttpReplayCache::evict_expired(std::int64_t now) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second <= now) it = entries_.erase(it);
    else ++it;
  }
}

HttpReplayCache& http_replay_cache() {
  static HttpReplayCache cache;
  return cache;
}

HttpAuthResult http_auth_verify(const std::string& method, const std::string& path_with_query,
                                const std::unordered_map<std::string, std::string>& headers,
                                const std::string& payload_hash_hex,
                                const std::string& cluster_key, int skew_ms) {
  return http_auth_verify(method, path_with_query, headers, payload_hash_hex, cluster_key,
                          skew_ms, &http_replay_cache());
}

HttpAuthResult http_auth_verify(const std::string& method, const std::string& path_with_query,
                                const std::unordered_map<std::string, std::string>& headers,
                                const std::string& payload_hash_hex,
                                const std::string& cluster_key, int skew_ms,
                                HttpReplayCache* replay) {
  HttpAuthPolicy policy;
  policy.cluster_key = cluster_key;
  policy.skew_ms = skew_ms;
  return http_auth_verify(method, path_with_query, headers, payload_hash_hex, policy, replay);
}

HttpAuthResult http_auth_verify(const std::string& method, const std::string& path_with_query,
                                const std::unordered_map<std::string, std::string>& headers,
                                const std::string& payload_hash_hex,
                                const HttpAuthPolicy& policy, HttpReplayCache* replay) {
  HttpAuthResult r;
  const int skew_ms = policy.skew_ms;
  const std::string auth = header_get(headers, "authorization");
  if (auth.rfind("AIOS-HMAC-SHA256 ", 0) != 0) {
    r.error = "missing or unsupported Authorization";
    r.code = "missing_auth";
    return r;
  }
  const std::string rest = auth.substr(16);
  std::string credential, signed_headers, signature;
  {
    std::istringstream iss(rest);
    std::string part;
    while (std::getline(iss, part, ',')) {
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) {
        part.erase(part.begin());
      }
      auto eq = part.find('=');
      if (eq == std::string::npos) continue;
      auto k = part.substr(0, eq);
      auto v = part.substr(eq + 1);
      if (k == "Credential") credential = v;
      else if (k == "SignedHeaders") signed_headers = v;
      else if (k == "Signature") signature = v;
    }
  }
  if (credential.empty() || signed_headers.empty() || signature.size() != 64) {
    r.error = "bad Authorization fields";
    r.code = "bad_auth_fields";
    return r;
  }

  std::string date = header_get(headers, "x-aios-date");
  if (date.empty()) date = header_get(headers, "date");
  if (date.empty()) {
    r.error = "missing date";
    r.code = "missing_date";
    return r;
  }

  std::int64_t ts = 0;
  if (!parse_auth_date_ms(date, ts)) {
    r.error = "unparsable date";
    r.code = "bad_date";
    return r;
  }
  const std::int64_t now = now_ms();
  if (std::llabs(now - ts) > skew_ms) {
    r.error = "date skew too large";
    r.code = "date_skew";
    return r;
  }

  // Pick the HMAC key: the session key inside a ticket credential, or the
  // shared cluster key for the legacy scheme.
  std::string hmac_key;
  if (credential.compare(0, std::strlen(kTicketPrefix), kTicketPrefix) == 0) {
    if (!policy.sealer) {
      r.error = "ticket credentials not accepted";
      r.code = "bad_ticket";
      return r;
    }
    std::string terr;
    auto t = policy.sealer->open(credential, now, terr);
    if (!t) {
      r.error = terr;
      r.code = terr == "ticket expired" ? "ticket_expired" : "bad_ticket";
      return r;
    }
    hmac_key = t->session_key;
    r.principal = t->principal;
    r.role = t->role;
    r.ticket = std::move(t);
  } else {
    if (!policy.allow_shared_key) {
      r.error = "shared cluster key not accepted from this peer; use a principal ticket";
      r.code = "shared_key_refused";
      return r;
    }
    hmac_key = policy.cluster_key;
    r.role = PrincipalRole::Node;
  }

  const auto canon =
      http_canonical(method, path_with_query, date, signed_headers, headers, payload_hash_hex);
  const auto expect = http_sign(hmac_key, canon);
  if (!const_time_eq(expect, signature)) {
    r.error = "bad signature";
    r.code = "bad_signature";
    r.principal.clear();
    r.ticket.reset();
    return r;
  }

  if (replay) {
    const std::string nonce = header_get(headers, kHttpNonceHeader);
    const bool mutating = method == "PUT" || method == "POST" || method == "DELETE";
    const bool signed_body = payload_hash_hex != "UNSIGNED-PAYLOAD";
    std::string key;
    if (!nonce.empty()) {
      key = "n\n" + date + '\n' + nonce + '\n' + signature;
    } else if (mutating && signed_body) {
      key = "r\n" + method + '\n' + path_with_query + '\n' + date + '\n' + signature;
    }
    // A replay stays inside the skew window for at most skew_ms past its date.
    if (!key.empty() && !replay->check_and_insert(key, ts + skew_ms + 1000, now)) {
      r.error = "replayed request";
      r.code = "replayed";
      r.principal.clear();
      r.ticket.reset();
      return r;
    }
  }
  r.ok = true;
  r.credential = credential;
  return r;
}

std::string http_url_encode_oid(const std::string& oid) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(oid.size() * 3);
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

}  // namespace aios
