#include "http/http_auth.hpp"

#include "util/auth.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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
  auto names = split_csv(signed_headers);
  for (auto& n : names) n = lower(n);
  std::sort(names.begin(), names.end());
  for (const auto& n : names) {
    oss << n << ':' << header_get(headers, n) << '\n';
  }
  oss << signed_headers << '\n' << payload_hash_hex;
  return oss.str();
}

std::string http_sign(const std::string& cluster_key, const std::string& canonical) {
  return hmac_sha256_hex(cluster_key, canonical);
}

HttpAuthResult http_auth_verify(const std::string& method, const std::string& path_with_query,
                                const std::unordered_map<std::string, std::string>& headers,
                                const std::string& payload_hash_hex,
                                const std::string& cluster_key, int skew_ms) {
  HttpAuthResult r;
  const std::string auth = header_get(headers, "authorization");
  if (auth.rfind("AIOS-HMAC-SHA256 ", 0) != 0) {
    r.error = "missing or unsupported Authorization";
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
    return r;
  }

  std::string date = header_get(headers, "x-aios-date");
  if (date.empty()) date = header_get(headers, "date");
  if (date.empty()) {
    r.error = "missing date";
    return r;
  }

  std::int64_t ts = 0;
  if (!parse_auth_date_ms(date, ts)) {
    r.error = "unparsable date";
    return r;
  }
  if (std::llabs(now_ms() - ts) > skew_ms) {
    r.error = "date skew too large";
    return r;
  }

  const auto canon =
      http_canonical(method, path_with_query, date, signed_headers, headers, payload_hash_hex);
  const auto expect = http_sign(cluster_key, canon);
  if (!const_time_eq(expect, signature)) {
    r.error = "bad signature";
    return r;
  }
  r.ok = true;
  r.credential = credential;
  return r;
}

}  // namespace aios
