#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aios {

struct S3ByteRange {
  std::uint64_t start{0};  // inclusive
  std::uint64_t end{0};    // inclusive
};

struct S3RangeParseResult {
  // True when a bytes= Range header was recognized (even if no satisfiable ranges).
  bool present{false};
  // Satisfiable ranges only (RFC 7233: omit unsatisfiable specs when others remain).
  std::vector<S3ByteRange> ranges;
  // True when present && ranges.empty() && at least one range spec was parsed.
  bool unsatisfiable{false};
};

// Parse HTTP Range: bytes=... (single or multi). Supports START-END, START-, -SUFFIX.
// Non-bytes units or unparseable values → present=false (caller ignores Range).
S3RangeParseResult parse_s3_byte_ranges(const std::string& range_header, std::uint64_t object_size);

// Build multipart/byteranges body. `parts[i]` is the payload for `ranges[i]`.
// ranges and parts must have the same size and be non-empty.
std::string build_s3_multipart_byteranges(const std::string& boundary,
                                          const std::string& part_content_type,
                                          std::uint64_t object_size,
                                          const std::vector<S3ByteRange>& ranges,
                                          const std::vector<std::string>& parts);

}  // namespace aios
