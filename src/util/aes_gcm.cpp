#include "util/aes_gcm.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cctype>

namespace aios {
namespace {

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool parse_aes256_key_hex(const std::string& hex, std::vector<std::uint8_t>& key_out,
                          std::string& err) {
  if (hex.size() != 64) {
    err = "bag_encryption_key must be 64 hex characters (32 bytes)";
    return false;
  }
  key_out.resize(kAes256KeyBytes);
  for (std::size_t i = 0; i < kAes256KeyBytes; ++i) {
    const int hi = hex_nibble(hex[i * 2]);
    const int lo = hex_nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      err = "bag_encryption_key contains non-hex characters";
      key_out.clear();
      return false;
    }
    key_out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool random_aes_gcm_nonce(std::uint8_t nonce[kAesGcmNonceBytes], std::string& err) {
  if (RAND_bytes(nonce, static_cast<int>(kAesGcmNonceBytes)) != 1) {
    err = "RAND_bytes failed for GCM nonce";
    return false;
  }
  return true;
}

bool aes_256_gcm_encrypt(const std::uint8_t* key, std::size_t key_len, const std::uint8_t* nonce,
                         std::size_t nonce_len, const std::uint8_t* plain, std::size_t plain_len,
                         std::vector<std::uint8_t>& out, std::string& err) {
  if (!key || key_len != kAes256KeyBytes || !nonce || nonce_len != kAesGcmNonceBytes) {
    err = "invalid AES-GCM key/nonce size";
    return false;
  }
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    err = "EVP_CIPHER_CTX_new failed";
    return false;
  }
  bool ok = false;
  out.clear();
  do {
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
      err = "EncryptInit failed";
      break;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce_len), nullptr) !=
        1) {
      err = "SET_IVLEN failed";
      break;
    }
    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1) {
      err = "EncryptInit key/iv failed";
      break;
    }
    out.resize(plain_len + kAesGcmTagBytes);
    int outl = 0;
    if (plain_len > 0) {
      if (EVP_EncryptUpdate(ctx, out.data(), &outl, plain, static_cast<int>(plain_len)) != 1) {
        err = "EncryptUpdate failed";
        break;
      }
    }
    int fin = 0;
    if (EVP_EncryptFinal_ex(ctx, out.data() + outl, &fin) != 1) {
      err = "EncryptFinal failed";
      break;
    }
    outl += fin;
    if (static_cast<std::size_t>(outl) != plain_len) {
      err = "encrypt length mismatch";
      break;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kAesGcmTagBytes),
                            out.data() + plain_len) != 1) {
      err = "GET_TAG failed";
      break;
    }
    ok = true;
  } while (false);
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) out.clear();
  return ok;
}

bool aes_256_gcm_decrypt(const std::uint8_t* key, std::size_t key_len, const std::uint8_t* nonce,
                         std::size_t nonce_len, const std::uint8_t* ct_and_tag,
                         std::size_t ct_and_tag_len, std::vector<std::uint8_t>& out,
                         std::string& err) {
  if (!key || key_len != kAes256KeyBytes || !nonce || nonce_len != kAesGcmNonceBytes) {
    err = "invalid AES-GCM key/nonce size";
    return false;
  }
  if (ct_and_tag_len < kAesGcmTagBytes) {
    err = "ciphertext too short for GCM tag";
    return false;
  }
  const std::size_t ct_len = ct_and_tag_len - kAesGcmTagBytes;
  const std::uint8_t* tag = ct_and_tag + ct_len;
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    err = "EVP_CIPHER_CTX_new failed";
    return false;
  }
  bool ok = false;
  out.clear();
  do {
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
      err = "DecryptInit failed";
      break;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce_len), nullptr) !=
        1) {
      err = "SET_IVLEN failed";
      break;
    }
    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1) {
      err = "DecryptInit key/iv failed";
      break;
    }
    out.resize(ct_len);
    int outl = 0;
    if (ct_len > 0) {
      if (EVP_DecryptUpdate(ctx, out.data(), &outl, ct_and_tag, static_cast<int>(ct_len)) != 1) {
        err = "DecryptUpdate failed";
        break;
      }
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kAesGcmTagBytes),
                            const_cast<std::uint8_t*>(tag)) != 1) {
      err = "SET_TAG failed";
      break;
    }
    int fin = 0;
    if (EVP_DecryptFinal_ex(ctx, out.data() + outl, &fin) != 1) {
      err = "AES-GCM authentication failed";
      break;
    }
    outl += fin;
    if (static_cast<std::size_t>(outl) != ct_len) {
      err = "decrypt length mismatch";
      break;
    }
    ok = true;
  } while (false);
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) out.clear();
  return ok;
}

}  // namespace aios
