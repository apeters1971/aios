#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

inline constexpr const char* kFrozenAttr = "aios.frozen";
inline constexpr const char* kBagIdAttr = "aios.bag_id";
inline constexpr const char* kBagOffsetAttr = "aios.bag_offset";
inline constexpr const char* kBagLengthAttr = "aios.bag_length";
inline constexpr const char* kContentSha256Attr = "aios.content_sha256";
inline constexpr const char* kArchiveStateAttr = "aios.archive_state";
inline constexpr const char* kTapeSinkAttr = "aios.tape_sink";
inline constexpr const char* kTapeRootAttr = "aios.tape_root";
inline constexpr const char* kTapeUriAttr = "aios.tape_uri";

inline constexpr const char* kArchiveStateBagged = "bagged";
inline constexpr const char* kArchiveStateOnTape = "on_tape";
inline constexpr const char* kArchiveStateRestoring = "restoring";

inline constexpr const char kBagMagic[4] = {'A', 'I', 'A', 'B'};
inline constexpr std::uint32_t kBagVersion = 1;

struct ArchiveMember {
  std::string oid;
  std::uint64_t offset{0};
  std::uint64_t length{0};
  std::string sha256_hex;
  std::unordered_map<std::string, std::string> attrs;
  std::vector<std::uint8_t> data;
};

struct ArchiveBag {
  std::vector<ArchiveMember> members;
};

// Encode members into a bag body (header + payloads + trailing index).
bool encode_archive_bag(const std::vector<ArchiveMember>& members, std::vector<std::uint8_t>& out,
                        std::string& err);

// Decode bag body; populates member metadata (and optionally payloads if fill_data).
bool decode_archive_bag(const std::uint8_t* data, std::size_t len, ArchiveBag& out,
                        bool fill_data, std::string& err);

bool is_archive_bag_oid(const std::string& oid);
bool attrs_are_frozen(const std::unordered_map<std::string, std::string>& attrs);
std::string archive_state_for_attrs(const std::unordered_map<std::string, std::string>& attrs);

void apply_frozen_stub_attrs(std::unordered_map<std::string, std::string>& attrs,
                             const std::string& bag_id, std::uint64_t offset, std::uint64_t length,
                             const std::string& sha256_hex);

void clear_frozen_stub_attrs(std::unordered_map<std::string, std::string>& attrs);

std::string sha256_hex_bytes(const std::uint8_t* data, std::size_t len);

}  // namespace aios
