#include "util/compression.hpp"

#include <cstring>

#if defined(AIOS_HAVE_ZSTD) && AIOS_HAVE_ZSTD
#include <zstd.h>
#endif

namespace aios {

void set_compression_attrs(std::unordered_map<std::string, std::string>& attrs,
                           const std::string& algo, std::uint64_t full_size,
                           std::uint32_t full_crc) {
  attrs[kCompAttrAlgo] = algo;
  attrs[kCompAttrFullSize] = std::to_string(full_size);
  attrs[kCompAttrFullCrc] = std::to_string(full_crc);
}

bool zstd_available() {
#if defined(AIOS_HAVE_ZSTD) && AIOS_HAVE_ZSTD
  return true;
#else
  return false;
#endif
}

bool zstd_compress(const std::uint8_t* data, std::size_t len, int level,
                   std::vector<std::uint8_t>& out, std::string& err) {
#if defined(AIOS_HAVE_ZSTD) && AIOS_HAVE_ZSTD
  if (level < 1) level = 1;
  if (level > 22) level = 22;
  const std::size_t bound = ZSTD_compressBound(len);
  out.resize(bound);
  const std::size_t n =
      ZSTD_compress(out.data(), bound, data, len, level);
  if (ZSTD_isError(n)) {
    err = ZSTD_getErrorName(n);
    out.clear();
    return false;
  }
  out.resize(n);
  return true;
#else
  (void)data;
  (void)len;
  (void)level;
  (void)out;
  err = "zstd not available (build without libzstd)";
  return false;
#endif
}

bool zstd_decompress(const std::uint8_t* data, std::size_t len, std::uint64_t logical_size,
                     std::vector<std::uint8_t>& out, std::string& err) {
#if defined(AIOS_HAVE_ZSTD) && AIOS_HAVE_ZSTD
  out.resize(static_cast<std::size_t>(logical_size));
  const std::size_t n =
      ZSTD_decompress(out.data(), out.size(), data, len);
  if (ZSTD_isError(n)) {
    err = ZSTD_getErrorName(n);
    out.clear();
    return false;
  }
  if (n != logical_size) {
    err = "zstd decompressed size mismatch";
    out.clear();
    return false;
  }
  return true;
#else
  (void)data;
  (void)len;
  (void)logical_size;
  (void)out;
  err = "zstd not available (build without libzstd)";
  return false;
#endif
}

}  // namespace aios
