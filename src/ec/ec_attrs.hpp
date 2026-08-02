#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace aios {

inline constexpr const char* kEcAttrK = "aios.ec.k";
inline constexpr const char* kEcAttrM = "aios.ec.m";
inline constexpr const char* kEcAttrI = "aios.ec.i";
inline constexpr const char* kEcAttrCodec = "aios.ec.codec";
inline constexpr const char* kEcAttrFullSize = "aios.ec.full_size";
inline constexpr const char* kEcAttrFullCrc = "aios.ec.full_crc";

struct EcMeta {
  int k{0};
  int m{0};
  int shard_i{-1};
  std::string codec;
  std::uint64_t full_size{0};
  std::uint32_t full_crc{0};
  bool full_crc_known{false};
};

inline bool attrs_are_ec(const std::unordered_map<std::string, std::string>& attrs) {
  return attrs.count(kEcAttrK) > 0 && attrs.count(kEcAttrM) > 0;
}

inline std::optional<EcMeta> parse_ec_attrs(
    const std::unordered_map<std::string, std::string>& attrs) {
  if (!attrs_are_ec(attrs)) return std::nullopt;
  EcMeta m;
  try {
    m.k = std::stoi(attrs.at(kEcAttrK));
    m.m = std::stoi(attrs.at(kEcAttrM));
    if (attrs.count(kEcAttrI)) m.shard_i = std::stoi(attrs.at(kEcAttrI));
    if (attrs.count(kEcAttrCodec)) m.codec = attrs.at(kEcAttrCodec);
    if (attrs.count(kEcAttrFullSize)) m.full_size = std::stoull(attrs.at(kEcAttrFullSize));
    if (attrs.count(kEcAttrFullCrc)) {
      m.full_crc = static_cast<std::uint32_t>(std::stoul(attrs.at(kEcAttrFullCrc)));
      m.full_crc_known = true;
    }
  } catch (...) {
    return std::nullopt;
  }
  if (m.k < 1 || m.m < 1) return std::nullopt;
  return m;
}

inline void set_ec_attrs(std::unordered_map<std::string, std::string>& attrs, int k, int m,
                         int shard_i, const std::string& codec, std::uint64_t full_size,
                         std::uint32_t full_crc) {
  attrs[kEcAttrK] = std::to_string(k);
  attrs[kEcAttrM] = std::to_string(m);
  attrs[kEcAttrI] = std::to_string(shard_i);
  attrs[kEcAttrCodec] = codec;
  attrs[kEcAttrFullSize] = std::to_string(full_size);
  attrs[kEcAttrFullCrc] = std::to_string(full_crc);
}

}  // namespace aios
