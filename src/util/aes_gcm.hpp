#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aios {

inline constexpr std::size_t kAes256KeyBytes = 32;
inline constexpr std::size_t kAesGcmNonceBytes = 12;
inline constexpr std::size_t kAesGcmTagBytes = 16;

// Parse 64 hex chars into 32-byte key. Returns false on bad input.
bool parse_aes256_key_hex(const std::string& hex, std::vector<std::uint8_t>& key_out,
                          std::string& err);

bool random_aes_gcm_nonce(std::uint8_t nonce[kAesGcmNonceBytes], std::string& err);

// Encrypt: out = ciphertext || tag (tag last 16 bytes).
bool aes_256_gcm_encrypt(const std::uint8_t* key, std::size_t key_len, const std::uint8_t* nonce,
                         std::size_t nonce_len, const std::uint8_t* plain, std::size_t plain_len,
                         std::vector<std::uint8_t>& out, std::string& err);

// Decrypt: ct_and_tag is ciphertext || tag.
bool aes_256_gcm_decrypt(const std::uint8_t* key, std::size_t key_len, const std::uint8_t* nonce,
                         std::size_t nonce_len, const std::uint8_t* ct_and_tag,
                         std::size_t ct_and_tag_len, std::vector<std::uint8_t>& out,
                         std::string& err);

}  // namespace aios
