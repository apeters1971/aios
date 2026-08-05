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
inline constexpr const char* kTapeUriPrefixAttr = "aios.tape_uri_prefix";
inline constexpr const char* kTapeBinAttr = "aios.tape_bin";
inline constexpr const char* kTapeS3EndpointAttr = "aios.tape_s3_endpoint";

inline constexpr const char* kArchiveStateBagged = "bagged";
inline constexpr const char* kArchiveStateOnTape = "on_tape";
inline constexpr const char* kArchiveStateRestoring = "restoring";

inline constexpr const char kBagMagic[4] = {'A', 'I', 'A', 'B'};
inline constexpr std::uint32_t kBagVersion = 1;

// Whole-bag transform wrapper (compress and/or encrypt).
inline constexpr const char kBagXformMagic[4] = {'A', 'I', 'T', 'F'};
inline constexpr std::uint32_t kBagXformVersion = 1;
inline constexpr std::uint32_t kBagXformFlagZstd = 1u;
inline constexpr std::uint32_t kBagXformFlagAesGcm = 2u;

inline constexpr const char* kBagCompressionAttr = "aios.bag.compression";
inline constexpr const char* kBagEncryptionAttr = "aios.bag.encryption";

struct BagTransformOpts {
  std::string compression{"none"};  // none | zstd
  int compression_level{3};
  std::string encryption{"none"};  // none | aes-256-gcm
};

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

// Compress then encrypt AIAB plaintext into stored bag body (AITF if any transform).
// key_hex is Config.bag_encryption_key (required when encryption != none).
// Sets aios.bag.compression / aios.bag.encryption on attrs_out.
bool transform_bag_for_storage(const std::vector<std::uint8_t>& plain, const BagTransformOpts& opts,
                               const std::string& key_hex, std::vector<std::uint8_t>& stored_out,
                               std::unordered_map<std::string, std::string>& attrs_out,
                               std::string& err);

// Inverse: stored body (AIAB or AITF) → AIAB plaintext.
bool untransform_bag_from_storage(const std::uint8_t* stored, std::size_t stored_len,
                                  const std::string& key_hex, std::vector<std::uint8_t>& plain_out,
                                  std::string& err);

bool bag_body_is_transformed(const std::uint8_t* data, std::size_t len);

}  // namespace aios
