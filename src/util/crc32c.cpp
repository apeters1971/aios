#include "util/crc32c.hpp"

#include <algorithm>
#include <cstring>

namespace aios {
namespace {

struct CrcTables {
  std::uint32_t t[8][256]{};
  CrcTables() {
    constexpr std::uint32_t poly = 0x82f63b78u;
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) ? (poly ^ (c >> 1)) : (c >> 1);
      }
      t[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = t[0][i];
      for (int j = 1; j < 8; ++j) {
        c = t[0][c & 0xff] ^ (c >> 8);
        t[j][i] = c;
      }
    }
  }
};

const CrcTables& tables() {
  static const CrcTables t;
  return t;
}

constexpr int kDim = 32;
using Mat = std::uint32_t[kDim];

std::uint32_t mat_times(const Mat mat, std::uint32_t vec) {
  std::uint32_t sum = 0;
  const std::uint32_t* m = mat;
  while (vec) {
    if (vec & 1u) sum ^= *m;
    vec >>= 1;
    ++m;
  }
  return sum;
}

void mat_square(Mat square, const Mat mat) {
  for (int n = 0; n < kDim; ++n) {
    square[n] = mat_times(mat, mat[n]);
  }
}

// Apply `len` zero bytes to a finalized CRC (zlib crc32_combine zeros part).
std::uint32_t crc_apply_zeros(std::uint32_t crc, std::size_t len) {
  if (len == 0) return crc;

  Mat odd{};
  Mat even{};
  odd[0] = 0x82f63b78u;
  std::uint32_t row = 1;
  for (int n = 1; n < kDim; ++n) {
    odd[n] = row;
    row <<= 1;
  }
  mat_square(even, odd);
  mat_square(odd, even);

  do {
    Mat tmp{};
    mat_square(tmp, odd);
    std::memcpy(even, tmp, sizeof(even));
    if (len & 1u) crc = mat_times(even, crc);
    len >>= 1;
    if (len == 0) break;
    mat_square(tmp, even);
    std::memcpy(odd, tmp, sizeof(odd));
    if (len & 1u) crc = mat_times(odd, crc);
    len >>= 1;
  } while (len != 0);
  return crc;
}

}  // namespace

std::uint32_t crc32c_update(std::uint32_t crc, const std::uint8_t* data, std::size_t len) {
  const auto& t = tables();
  crc = ~crc;
  while (len && (reinterpret_cast<std::uintptr_t>(data) & 7u)) {
    crc = t.t[0][(crc ^ *data++) & 0xff] ^ (crc >> 8);
    --len;
  }
  while (len >= 8) {
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    std::memcpy(&lo, data, 4);
    std::memcpy(&hi, data + 4, 4);
    data += 8;
    len -= 8;
    crc ^= lo;
    crc = t.t[7][crc & 0xff] ^ t.t[6][(crc >> 8) & 0xff] ^ t.t[5][(crc >> 16) & 0xff] ^
          t.t[4][crc >> 24] ^ t.t[3][hi & 0xff] ^ t.t[2][(hi >> 8) & 0xff] ^
          t.t[1][(hi >> 16) & 0xff] ^ t.t[0][hi >> 24];
  }
  while (len--) {
    crc = t.t[0][(crc ^ *data++) & 0xff] ^ (crc >> 8);
  }
  return ~crc;
}

std::uint32_t crc32c_update_zeros(std::uint32_t crc, std::size_t len) {
  // Append `len` zero bytes to the CRC stream (for sparse holes while scanning).
  std::uint8_t z[4096]{};
  while (len > 0) {
    const std::size_t n = std::min(len, sizeof(z));
    crc = crc32c_update(crc, z, n);
    len -= n;
  }
  return crc;
}

std::uint32_t crc32c_combine(std::uint32_t crc_a, std::uint32_t crc_b, std::size_t len_b) {
  if (len_b == 0) return crc_a;
  return crc_apply_zeros(crc_a, len_b) ^ crc_b;
}

}  // namespace aios
