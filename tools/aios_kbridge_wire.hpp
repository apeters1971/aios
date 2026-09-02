/* Reply framing for aios-kbridge.
 *
 * aios_dev_write (kernel/aiosfs/upcall.c) consumes exactly one reply per
 * write(2) and requires count >= sizeof(aios_kabi_rep_hdr) + payload_len, so
 * header and payload must be delivered in a single contiguous buffer.
 */
#pragma once

#include "../kernel/aios_kabi.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace aios_kbridge {

inline std::vector<unsigned char> frame_reply(uint64_t unique, int32_t result,
                                              const void* payload, uint32_t payload_len) {
  aios_kabi_rep_hdr hdr{};
  hdr.magic = AIOS_KABI_MAGIC;
  hdr.version = AIOS_KABI_VERSION;
  hdr.unique = unique;
  hdr.result = result;
  hdr.payload_len = payload_len;
  std::vector<unsigned char> buf(sizeof(hdr) + payload_len);
  std::memcpy(buf.data(), &hdr, sizeof(hdr));
  if (payload_len) std::memcpy(buf.data() + sizeof(hdr), payload, payload_len);
  return buf;
}

/* One write(2) per reply; the device rejects short or split frames. */
inline bool write_reply(int fd, uint64_t unique, int32_t result, const void* payload,
                        uint32_t payload_len) {
  const std::vector<unsigned char> buf = frame_reply(unique, result, payload, payload_len);
  for (;;) {
    const ssize_t n = ::write(fd, buf.data(), buf.size());
    if (n < 0 && errno == EINTR) continue;
    return n == static_cast<ssize_t>(buf.size());
  }
}

}  // namespace aios_kbridge
