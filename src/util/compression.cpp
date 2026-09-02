#include "util/compression.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

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
  // full_size is attacker-controllable metadata; never let it throw out of here,
  // and never allocate on its say-so alone: the frame header must agree, the
  // expansion ratio must be physically possible, and the output is grown as the
  // decoder actually produces bytes rather than zero-filled up front.
  out.clear();
  constexpr std::uint64_t kMaxLogical = 64ull * 1024ull * 1024ull * 1024ull;
  // zstd's densest encoding is an RLE block: 3-byte header + 1 byte -> 128 KiB
  // (32768:1); allow 2x headroom so all-zero bodies still decode.
  constexpr std::uint64_t kMaxRatio = 65536ull;
  // Below this size the up-front allocation is cheap; above it we stream.
  constexpr std::uint64_t kOneShotMax = 16ull * 1024ull * 1024ull;
  constexpr std::size_t kStreamChunk = 4u * 1024u * 1024u;
  if (!data || len == 0) {
    err = "empty zstd frame";
    return false;
  }
  if (logical_size > kMaxLogical ||
      logical_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    err = "logical size too large";
    return false;
  }
  if (logical_size > static_cast<std::uint64_t>(len) * kMaxRatio) {
    err = "zstd expansion ratio implausible";
    return false;
  }
  const unsigned long long fcs = ZSTD_getFrameContentSize(data, len);
  if (fcs == ZSTD_CONTENTSIZE_ERROR) {
    err = "bad zstd frame header";
    return false;
  }
  if (fcs == ZSTD_CONTENTSIZE_UNKNOWN) {
    err = "zstd frame without content size";
    return false;
  }
  if (static_cast<std::uint64_t>(fcs) != logical_size) {
    err = "zstd frame content size mismatch";
    return false;
  }

  if (logical_size <= kOneShotMax) {
    try {
      out.resize(static_cast<std::size_t>(logical_size));
    } catch (const std::exception&) {
      err = "decompress buffer allocation failed";
      out.clear();
      return false;
    }
    const std::size_t n = ZSTD_decompress(out.data(), out.size(), data, len);
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
  }

  ZSTD_DStream* ds = ZSTD_createDStream();
  if (!ds) {
    err = "ZSTD_createDStream failed";
    return false;
  }
  struct DsGuard {
    ZSTD_DStream* d;
    ~DsGuard() { ZSTD_freeDStream(d); }
  } guard{ds};
  ZSTD_initDStream(ds);
  ZSTD_inBuffer in{data, len, 0};
  try {
    out.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(logical_size, static_cast<std::uint64_t>(len) * 64ull)));
    std::size_t produced = 0;
    for (;;) {
      if (produced >= logical_size) {
        // Decoder wants to emit more than the frame promised.
        err = "zstd decompressed size mismatch";
        out.clear();
        return false;
      }
      const std::size_t room = static_cast<std::size_t>(
          std::min<std::uint64_t>(kStreamChunk, logical_size - produced));
      out.resize(produced + room);
      ZSTD_outBuffer ob{out.data() + produced, room, 0};
      const std::size_t rc = ZSTD_decompressStream(ds, &ob, &in);
      if (ZSTD_isError(rc)) {
        err = ZSTD_getErrorName(rc);
        out.clear();
        return false;
      }
      produced += ob.pos;
      out.resize(produced);
      if (rc == 0) break;  // frame fully decoded
      if (ob.pos == 0 && in.pos >= in.size) {
        err = "truncated zstd frame";
        out.clear();
        return false;
      }
    }
    if (produced != logical_size || in.pos != in.size) {
      err = "zstd decompressed size mismatch";
      out.clear();
      return false;
    }
  } catch (const std::exception&) {
    err = "decompress buffer allocation failed";
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
