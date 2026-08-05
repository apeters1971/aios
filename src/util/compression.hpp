#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

inline constexpr const char* kCompAttrAlgo = "aios.compression";
inline constexpr const char* kCompAttrFullSize = "aios.compression.full_size";
inline constexpr const char* kCompAttrFullCrc = "aios.compression.full_crc";
inline constexpr const char* kCompAlgoZstd = "zstd";

inline bool attrs_are_compressed(const std::unordered_map<std::string, std::string>& attrs) {
  auto it = attrs.find(kCompAttrAlgo);
  return it != attrs.end() && !it->second.empty() && it->second != "none";
}

inline std::optional<std::uint64_t> compression_full_size(
    const std::unordered_map<std::string, std::string>& attrs) {
  auto it = attrs.find(kCompAttrFullSize);
  if (it == attrs.end()) return std::nullopt;
  try {
    return std::stoull(it->second);
  } catch (...) {
    return std::nullopt;
  }
}

void set_compression_attrs(std::unordered_map<std::string, std::string>& attrs,
                           const std::string& algo, std::uint64_t full_size,
                           std::uint32_t full_crc);

// Returns true if ZSTD support is compiled in.
bool zstd_available();

// Compress with zstd. On success, out is smaller than (or equal to) in when
// compression helps; returns false on error or if libzstd is unavailable.
bool zstd_compress(const std::uint8_t* data, std::size_t len, int level,
                   std::vector<std::uint8_t>& out, std::string& err);

bool zstd_decompress(const std::uint8_t* data, std::size_t len, std::uint64_t logical_size,
                     std::vector<std::uint8_t>& out, std::string& err);

}  // namespace aios
