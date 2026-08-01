#pragma once

#include <cstddef>
#include <cstdint>

namespace aios {

// Castagnoli CRC-32C (poly 0x1EDC6F41). Init/final XOR 0xFFFFFFFF.
// Suitable for object end-to-end checksums; combinable across ranges for updates.

std::uint32_t crc32c_update(std::uint32_t crc, const std::uint8_t* data, std::size_t len);

inline std::uint32_t crc32c(const std::uint8_t* data, std::size_t len) {
  return crc32c_update(0, data, len);
}

// Extend CRC as if `len` zero bytes were appended (for sparse holes).
std::uint32_t crc32c_update_zeros(std::uint32_t crc, std::size_t len);

// CRC(A || B) given CRC(A), CRC(B), and len(B).
std::uint32_t crc32c_combine(std::uint32_t crc_a, std::uint32_t crc_b, std::size_t len_b);

}  // namespace aios
