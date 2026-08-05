#include "object/archive_bag.hpp"

#include "util/aes_gcm.hpp"
#include "util/compression.hpp"

#include <openssl/evp.h>

#include <cstring>
#include <nlohmann/json.hpp>

namespace aios {
namespace {

void write_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xff));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
}
void write_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}
void write_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}

bool read_u16(const std::uint8_t*& p, const std::uint8_t* end, std::uint16_t& v) {
  if (end - p < 2) return false;
  v = static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
  p += 2;
  return true;
}
bool read_u32(const std::uint8_t*& p, const std::uint8_t* end, std::uint32_t& v) {
  if (end - p < 4) return false;
  v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
  p += 4;
  return true;
}
bool read_u64(const std::uint8_t*& p, const std::uint8_t* end, std::uint64_t& v) {
  if (end - p < 8) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  p += 8;
  return true;
}

}  // namespace

std::string sha256_hex_bytes(const std::uint8_t* data, std::size_t len) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  EVP_Digest(data, len, md, &md_len, EVP_sha256(), nullptr);
  static const char* hexd = "0123456789abcdef";
  std::string out(md_len * 2, '\0');
  for (unsigned int i = 0; i < md_len; ++i) {
    out[i * 2] = hexd[md[i] >> 4];
    out[i * 2 + 1] = hexd[md[i] & 0xf];
  }
  return out;
}

bool is_archive_bag_oid(const std::string& oid) { return oid.rfind("archive/bag/", 0) == 0; }

bool attrs_are_frozen(const std::unordered_map<std::string, std::string>& attrs) {
  auto it = attrs.find(kFrozenAttr);
  return it != attrs.end() && (it->second == "1" || it->second == "true");
}

std::string archive_state_for_attrs(const std::unordered_map<std::string, std::string>& attrs) {
  auto it = attrs.find(kArchiveStateAttr);
  return it == attrs.end() ? std::string{} : it->second;
}

void apply_frozen_stub_attrs(std::unordered_map<std::string, std::string>& attrs,
                             const std::string& bag_id, std::uint64_t offset, std::uint64_t length,
                             const std::string& sha256_hex) {
  attrs[kFrozenAttr] = "1";
  attrs[kBagIdAttr] = bag_id;
  attrs[kBagOffsetAttr] = std::to_string(offset);
  attrs[kBagLengthAttr] = std::to_string(length);
  attrs[kContentSha256Attr] = sha256_hex;
  attrs[kArchiveStateAttr] = kArchiveStateBagged;
}

void clear_frozen_stub_attrs(std::unordered_map<std::string, std::string>& attrs) {
  attrs.erase(kFrozenAttr);
  attrs.erase(kBagIdAttr);
  attrs.erase(kBagOffsetAttr);
  attrs.erase(kBagLengthAttr);
  attrs.erase(kContentSha256Attr);
  attrs.erase(kArchiveStateAttr);
}

bool encode_archive_bag(const std::vector<ArchiveMember>& members, std::vector<std::uint8_t>& out,
                        std::string& err) {
  if (members.empty()) {
    err = "empty bag";
    return false;
  }
  constexpr std::size_t kHeader = 24;
  out.clear();
  out.reserve(kHeader + 1024);
  out.insert(out.end(), kBagMagic, kBagMagic + 4);
  write_u32(out, kBagVersion);
  write_u32(out, static_cast<std::uint32_t>(members.size()));
  write_u64(out, 0);  // index_offset placeholder
  write_u32(out, 0);  // reserved

  std::vector<ArchiveMember> meta = members;
  for (auto& m : meta) {
    m.offset = out.size();
    m.length = m.data.size();
    if (m.sha256_hex.empty()) m.sha256_hex = sha256_hex_bytes(m.data.data(), m.data.size());
    out.insert(out.end(), m.data.begin(), m.data.end());
  }
  const std::uint64_t index_off = out.size();
  // Patch index_offset at byte 12.
  for (int i = 0; i < 8; ++i)
    out[12 + i] = static_cast<std::uint8_t>((index_off >> (8 * i)) & 0xff);

  for (const auto& m : meta) {
    if (m.oid.size() > 65535) {
      err = "oid too long";
      return false;
    }
    write_u16(out, static_cast<std::uint16_t>(m.oid.size()));
    out.insert(out.end(), m.oid.begin(), m.oid.end());
    write_u64(out, m.offset);
    write_u64(out, m.length);
    if (m.sha256_hex.size() != 64) {
      err = "bad sha256";
      return false;
    }
    out.insert(out.end(), m.sha256_hex.begin(), m.sha256_hex.end());
    nlohmann::json ja = nlohmann::json::object();
    for (const auto& [k, v] : m.attrs) ja[k] = v;
    const std::string js = ja.dump();
    write_u32(out, static_cast<std::uint32_t>(js.size()));
    out.insert(out.end(), js.begin(), js.end());
  }
  return true;
}

bool decode_archive_bag(const std::uint8_t* data, std::size_t len, ArchiveBag& out, bool fill_data,
                        std::string& err) {
  out = ArchiveBag{};
  if (!data || len < 24) {
    err = "bag too short";
    return false;
  }
  if (std::memcmp(data, kBagMagic, 4) != 0) {
    err = "bad bag magic";
    return false;
  }
  const std::uint8_t* p = data + 4;
  const std::uint8_t* end = data + len;
  std::uint32_t ver = 0, count = 0, reserved = 0;
  std::uint64_t index_off = 0;
  if (!read_u32(p, end, ver) || !read_u32(p, end, count) || !read_u64(p, end, index_off) ||
      !read_u32(p, end, reserved)) {
    err = "bad bag header";
    return false;
  }
  if (ver != kBagVersion) {
    err = "unsupported bag version";
    return false;
  }
  if (index_off >= len) {
    err = "bad index offset";
    return false;
  }
  p = data + index_off;
  out.members.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    ArchiveMember m;
    std::uint16_t oid_len = 0;
    if (!read_u16(p, end, oid_len) || end - p < oid_len) {
      err = "bad oid";
      return false;
    }
    m.oid.assign(reinterpret_cast<const char*>(p), oid_len);
    p += oid_len;
    if (!read_u64(p, end, m.offset) || !read_u64(p, end, m.length)) {
      err = "bad member extents";
      return false;
    }
    if (end - p < 64) {
      err = "bad sha";
      return false;
    }
    m.sha256_hex.assign(reinterpret_cast<const char*>(p), 64);
    p += 64;
    std::uint32_t alen = 0;
    if (!read_u32(p, end, alen) || end - p < static_cast<std::ptrdiff_t>(alen)) {
      err = "bad attrs";
      return false;
    }
    try {
      auto ja = nlohmann::json::parse(std::string(reinterpret_cast<const char*>(p), alen));
      if (ja.is_object()) {
        for (auto it = ja.begin(); it != ja.end(); ++it) {
          if (it.value().is_string()) m.attrs[it.key()] = it.value().get<std::string>();
        }
      }
    } catch (...) {
      err = "attrs json";
      return false;
    }
    p += alen;
    if (m.offset + m.length > len) {
      err = "member out of range";
      return false;
    }
    if (fill_data) {
      m.data.assign(data + m.offset, data + m.offset + m.length);
    }
    out.members.push_back(std::move(m));
  }
  return true;
}

bool bag_body_is_transformed(const std::uint8_t* data, std::size_t len) {
  return len >= 4 && std::memcmp(data, kBagXformMagic, 4) == 0;
}

bool transform_bag_for_storage(const std::vector<std::uint8_t>& plain, const BagTransformOpts& opts,
                               const std::string& key_hex, std::vector<std::uint8_t>& stored_out,
                               std::unordered_map<std::string, std::string>& attrs_out,
                               std::string& err) {
  const bool want_zstd = opts.compression == "zstd";
  const bool want_aes = opts.encryption == "aes-256-gcm";
  if (!want_zstd && opts.compression != "none" && !opts.compression.empty()) {
    err = "bag_compression must be none or zstd";
    return false;
  }
  if (!want_aes && opts.encryption != "none" && !opts.encryption.empty()) {
    err = "bag_encryption must be none or aes-256-gcm";
    return false;
  }
  attrs_out[kBagCompressionAttr] = want_zstd ? "zstd" : "none";
  attrs_out[kBagEncryptionAttr] = want_aes ? "aes-256-gcm" : "none";

  if (!want_zstd && !want_aes) {
    stored_out = plain;
    return true;
  }

  std::vector<std::uint8_t> mid = plain;
  if (want_zstd) {
    if (!zstd_available()) {
      err = "bag_compression=zstd requires libzstd at build time";
      return false;
    }
    std::vector<std::uint8_t> compressed;
    if (!zstd_compress(plain.data(), plain.size(), opts.compression_level, compressed, err)) {
      return false;
    }
    mid = std::move(compressed);
  }

  std::uint8_t nonce[kAesGcmNonceBytes]{};
  std::vector<std::uint8_t> payload = mid;
  if (want_aes) {
    std::vector<std::uint8_t> key;
    if (!parse_aes256_key_hex(key_hex, key, err)) return false;
    if (!random_aes_gcm_nonce(nonce, err)) return false;
    std::vector<std::uint8_t> ct;
    if (!aes_256_gcm_encrypt(key.data(), key.size(), nonce, kAesGcmNonceBytes, mid.data(),
                             mid.size(), ct, err)) {
      return false;
    }
    payload = std::move(ct);
  }

  stored_out.clear();
  stored_out.insert(stored_out.end(), kBagXformMagic, kBagXformMagic + 4);
  write_u32(stored_out, kBagXformVersion);
  std::uint32_t flags = 0;
  if (want_zstd) flags |= kBagXformFlagZstd;
  if (want_aes) flags |= kBagXformFlagAesGcm;
  write_u32(stored_out, flags);
  write_u64(stored_out, static_cast<std::uint64_t>(plain.size()));
  stored_out.insert(stored_out.end(), nonce, nonce + kAesGcmNonceBytes);
  stored_out.insert(stored_out.end(), payload.begin(), payload.end());
  return true;
}

bool untransform_bag_from_storage(const std::uint8_t* stored, std::size_t stored_len,
                                  const std::string& key_hex, std::vector<std::uint8_t>& plain_out,
                                  std::string& err) {
  if (!stored || stored_len < 4) {
    err = "empty bag body";
    return false;
  }
  if (!bag_body_is_transformed(stored, stored_len)) {
    // Plain AIAB.
    plain_out.assign(stored, stored + stored_len);
    return true;
  }
  const std::uint8_t* p = stored;
  const std::uint8_t* end = stored + stored_len;
  p += 4;  // magic
  std::uint32_t ver = 0, flags = 0;
  std::uint64_t plain_len = 0;
  if (!read_u32(p, end, ver) || ver != kBagXformVersion) {
    err = "unsupported AITF version";
    return false;
  }
  if (!read_u32(p, end, flags) || !read_u64(p, end, plain_len)) {
    err = "truncated AITF header";
    return false;
  }
  if (end - p < static_cast<std::ptrdiff_t>(kAesGcmNonceBytes)) {
    err = "truncated AITF nonce";
    return false;
  }
  const std::uint8_t* nonce = p;
  p += kAesGcmNonceBytes;
  const std::size_t payload_len = static_cast<std::size_t>(end - p);
  std::vector<std::uint8_t> mid;
  if (flags & kBagXformFlagAesGcm) {
    std::vector<std::uint8_t> key;
    if (!parse_aes256_key_hex(key_hex, key, err)) return false;
    if (!aes_256_gcm_decrypt(key.data(), key.size(), nonce, kAesGcmNonceBytes, p, payload_len, mid,
                             err)) {
      return false;
    }
  } else {
    mid.assign(p, end);
  }
  if (flags & kBagXformFlagZstd) {
    if (!zstd_decompress(mid.data(), mid.size(), plain_len, plain_out, err)) return false;
  } else {
    if (mid.size() != plain_len) {
      err = "AITF plain_len mismatch";
      return false;
    }
    plain_out = std::move(mid);
  }
  return true;
}

}  // namespace aios
