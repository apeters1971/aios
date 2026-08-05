#include "http/s3_range.hpp"

#include <cctype>
#include <sstream>

namespace aios {
namespace {

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

std::vector<std::string> split_comma(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      out.push_back(trim(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(trim(cur));
  return out;
}

bool parse_u64(const std::string& s, std::uint64_t& v) {
  if (s.empty()) return false;
  try {
    std::size_t idx = 0;
    unsigned long long x = std::stoull(s, &idx, 10);
    if (idx != s.size()) return false;
    v = static_cast<std::uint64_t>(x);
    return true;
  } catch (...) {
    return false;
  }
}

// Resolve one range spec against object_size. Returns false if unsatisfiable/invalid.
bool resolve_spec(const std::string& spec, std::uint64_t object_size, S3ByteRange& out) {
  auto dash = spec.find('-');
  if (dash == std::string::npos) return false;
  const std::string left = trim(spec.substr(0, dash));
  const std::string right = trim(spec.substr(dash + 1));

  if (object_size == 0) {
    // Empty object: only bytes=0-0 or bytes=-0 style is odd; treat all as unsatisfiable.
    return false;
  }

  if (left.empty()) {
    // Suffix: bytes=-N → last N bytes
    std::uint64_t n = 0;
    if (!parse_u64(right, n) || n == 0) return false;
    if (n > object_size) n = object_size;
    out.start = object_size - n;
    out.end = object_size - 1;
    return true;
  }

  std::uint64_t start = 0;
  if (!parse_u64(left, start)) return false;
  if (start >= object_size) return false;

  if (right.empty()) {
    // bytes=START-
    out.start = start;
    out.end = object_size - 1;
    return true;
  }

  std::uint64_t end = 0;
  if (!parse_u64(right, end)) return false;
  if (end < start) return false;
  if (end >= object_size) end = object_size - 1;
  out.start = start;
  out.end = end;
  return true;
}

}  // namespace

S3RangeParseResult parse_s3_byte_ranges(const std::string& range_header,
                                        std::uint64_t object_size) {
  S3RangeParseResult r;
  auto hdr = trim(range_header);
  if (hdr.empty()) return r;
  // Case-insensitive "bytes="
  if (hdr.size() < 6) return r;
  std::string unit = hdr.substr(0, 6);
  for (char& c : unit) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (unit != "bytes=") return r;

  r.present = true;
  const auto specs = split_comma(hdr.substr(6));
  int parsed = 0;
  for (const auto& spec : specs) {
    if (spec.empty()) continue;
    ++parsed;
    S3ByteRange br;
    if (resolve_spec(spec, object_size, br)) r.ranges.push_back(br);
  }
  if (parsed > 0 && r.ranges.empty()) r.unsatisfiable = true;
  // No valid specs at all → treat as absent (ignore Range).
  if (parsed == 0) {
    r.present = false;
    r.unsatisfiable = false;
  }
  return r;
}

std::string build_s3_multipart_byteranges(const std::string& boundary,
                                          const std::string& part_content_type,
                                          std::uint64_t object_size,
                                          const std::vector<S3ByteRange>& ranges,
                                          const std::vector<std::string>& parts) {
  std::ostringstream oss;
  const std::string ctype =
      part_content_type.empty() ? "application/octet-stream" : part_content_type;
  for (std::size_t i = 0; i < ranges.size() && i < parts.size(); ++i) {
    oss << "--" << boundary << "\r\n";
    oss << "Content-Type: " << ctype << "\r\n";
    oss << "Content-Range: bytes " << ranges[i].start << '-' << ranges[i].end << '/'
        << object_size << "\r\n";
    oss << "\r\n";
    oss << parts[i] << "\r\n";
  }
  oss << "--" << boundary << "--\r\n";
  return oss.str();
}

}  // namespace aios
