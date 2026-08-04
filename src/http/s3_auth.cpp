#include "http/s3_auth.hpp"

#include "util/auth.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>
#include <vector>

namespace aios {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string header_get(const std::unordered_map<std::string, std::string>& headers,
                       const std::string& name) {
  auto it = headers.find(lower(name));
  if (it == headers.end()) return {};
  return it->second;
}

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
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

bool parse_amz_date(const std::string& amz, std::time_t& out_ts) {
  // YYYYMMDD'T'HHMMSS'Z'
  if (amz.size() != 16 || amz[8] != 'T' || amz[15] != 'Z') return false;
  std::tm tm{};
  try {
    tm.tm_year = std::stoi(amz.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(amz.substr(4, 2)) - 1;
    tm.tm_mday = std::stoi(amz.substr(6, 2));
    tm.tm_hour = std::stoi(amz.substr(9, 2));
    tm.tm_min = std::stoi(amz.substr(11, 2));
    tm.tm_sec = std::stoi(amz.substr(13, 2));
  } catch (...) {
    return false;
  }
#if defined(_WIN32)
  out_ts = _mkgmtime(&tm);
#else
  out_ts = timegm(&tm);
#endif
  return out_ts != static_cast<std::time_t>(-1);
}

std::string signing_key(const std::string& secret, const std::string& date, const std::string& region,
                        const std::string& service) {
  const auto k_date = hmac_sha256_raw("AWS4" + secret, date);
  const auto k_region = hmac_sha256_raw(k_date, region);
  const auto k_service = hmac_sha256_raw(k_region, service);
  return hmac_sha256_raw(k_service, "aws4_request");
}

}  // namespace

std::string s3_sigv4_access_key(const std::unordered_map<std::string, std::string>& headers) {
  const std::string auth = header_get(headers, "authorization");
  if (auth.rfind("AWS4-HMAC-SHA256 ", 0) != 0) return {};
  const std::string rest = auth.substr(16);
  std::string credential;
  {
    std::istringstream iss(rest);
    std::string part;
    while (std::getline(iss, part, ',')) {
      part = trim(part);
      if (part.rfind("Credential=", 0) == 0) credential = part.substr(11);
    }
  }
  if (credential.empty()) return {};
  auto slash = credential.find('/');
  if (slash == std::string::npos || slash == 0) return {};
  return credential.substr(0, slash);
}

std::string s3_uri_encode(const std::string& in, bool encode_slash) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (unsigned char c : in) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~' || (!encode_slash && c == '/')) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xf]);
    }
  }
  return out;
}

S3AuthResult s3_sigv4_verify(const std::string& method, const std::string& canonical_uri,
                             const std::string& canonical_query,
                             const std::unordered_map<std::string, std::string>& headers,
                             const std::string& payload_hash_hex,
                             const std::string& expected_access_key,
                             const std::string& secret_key, int skew_ms) {
  S3AuthResult r;
  const std::string auth = header_get(headers, "authorization");
  if (auth.rfind("AWS4-HMAC-SHA256 ", 0) != 0) {
    r.error = "missing or unsupported Authorization";
    return r;
  }
  const std::string rest = auth.substr(16);
  std::string credential, signed_headers, signature;
  {
    std::istringstream iss(rest);
    std::string part;
    while (std::getline(iss, part, ',')) {
      part = trim(part);
      if (part.rfind("Credential=", 0) == 0) credential = part.substr(11);
      else if (part.rfind("SignedHeaders=", 0) == 0) signed_headers = part.substr(14);
      else if (part.rfind("Signature=", 0) == 0) signature = part.substr(10);
    }
  }
  if (credential.empty() || signed_headers.empty() || signature.empty()) {
    r.error = "malformed Authorization";
    return r;
  }

  // AKID/YYYYMMDD/region/service/aws4_request
  std::vector<std::string> cred_parts;
  {
    std::istringstream iss(credential);
    std::string p;
    while (std::getline(iss, p, '/')) cred_parts.push_back(p);
  }
  if (cred_parts.size() != 5 || cred_parts[4] != "aws4_request") {
    r.error = "bad Credential scope";
    return r;
  }
  r.access_key = cred_parts[0];
  const std::string& date_stamp = cred_parts[1];
  r.region = cred_parts[2];
  const std::string& service = cred_parts[3];
  if (r.access_key != expected_access_key) {
    r.error = "Unknown access key";
    return r;
  }
  if (service != "s3") {
    r.error = "Credential service must be s3";
    return r;
  }

  std::string amz_date = header_get(headers, "x-amz-date");
  if (amz_date.empty()) {
    // Fallback Date header not fully supported; require x-amz-date.
    r.error = "missing x-amz-date";
    return r;
  }
  if (amz_date.size() < 8 || amz_date.substr(0, 8) != date_stamp) {
    r.error = "x-amz-date does not match Credential date";
    return r;
  }
  std::time_t req_ts = 0;
  if (!parse_amz_date(amz_date, req_ts)) {
    r.error = "bad x-amz-date";
    return r;
  }
  const auto now = std::time(nullptr);
  if (skew_ms > 0) {
    const auto skew_s = skew_ms / 1000;
    if (req_ts + skew_s < now || req_ts > now + skew_s) {
      r.error = "Request has expired";
      return r;
    }
  }

  std::string content_hash = payload_hash_hex;
  if (content_hash.empty()) content_hash = header_get(headers, "x-amz-content-sha256");
  if (content_hash.empty()) {
    r.error = "missing x-amz-content-sha256";
    return r;
  }

  // Canonical headers from SignedHeaders list.
  std::vector<std::string> sh_names;
  {
    std::istringstream iss(signed_headers);
    std::string n;
    while (std::getline(iss, n, ';')) {
      if (!n.empty()) sh_names.push_back(lower(n));
    }
  }
  std::sort(sh_names.begin(), sh_names.end());
  std::ostringstream canon_headers;
  std::string signed_joined;
  for (std::size_t i = 0; i < sh_names.size(); ++i) {
    if (i) signed_joined.push_back(';');
    signed_joined += sh_names[i];
    std::string val = header_get(headers, sh_names[i]);
    // Collapse whitespace in header values.
    std::string collapsed;
    bool sp = false;
    for (char c : val) {
      if (std::isspace(static_cast<unsigned char>(c))) {
        if (!collapsed.empty()) sp = true;
      } else {
        if (sp) {
          collapsed.push_back(' ');
          sp = false;
        }
        collapsed.push_back(c);
      }
    }
    canon_headers << sh_names[i] << ':' << collapsed << '\n';
  }

  std::ostringstream canon_req;
  canon_req << method << '\n'
            << canonical_uri << '\n'
            << canonical_query << '\n'
            << canon_headers.str() << signed_joined << '\n'
            << content_hash;

  const auto canon_hash = sha256_hex(canon_req.str());
  std::ostringstream sts;
  sts << "AWS4-HMAC-SHA256\n"
      << amz_date << '\n'
      << date_stamp << '/' << r.region << '/' << service << "/aws4_request\n"
      << canon_hash;

  const auto key = signing_key(secret_key, date_stamp, r.region, service);
  const auto sig_raw = hmac_sha256_raw(key, sts.str());
  const auto expect = lower(hex_lower(sig_raw));
  const auto got = lower(signature);
  if (expect.size() != got.size()) {
    r.error = "SignatureDoesNotMatch";
    return r;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < expect.size(); ++i) {
    diff |= static_cast<unsigned char>(expect[i] ^ got[i]);
  }
  if (diff != 0) {
    r.error = "SignatureDoesNotMatch";
    return r;
  }
  r.ok = true;
  return r;
}

}  // namespace aios
