#pragma once

#include "cuobject/cuobject_endpoint.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace aios {

// cuObject DC token, colon-delimited hex, as sent in x-amz-rdma-token.
// Field 1 is the remote address (uint64, at most 16 digits). Field 2 is the
// registered buffer size (uint32, at most 8 digits). The remaining fields
// (rkey, LID, DCT number, GID) stay inside the string passed to cuObjServer.
// A first field longer than 16 hex digits is a different scheme (for example
// an RC token) and is not a DC descriptor.
struct RdmaTokenView {
  bool cuobject{false};
  bool unsupported{false};
  bool malformed{false};
  bool has_addr{false};
  bool has_size{false};
  std::uint64_t addr{0};
  std::uint32_t size{0};
};

inline bool rdma_hex_ok(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    const bool digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!digit) return false;
  }
  return true;
}

inline bool rdma_parse_hex(std::string_view s, int max_digits, std::uint64_t& out) {
  if (s.empty() || static_cast<int>(s.size()) > max_digits || !rdma_hex_ok(s)) return false;
  std::uint64_t v = 0;
  for (char c : s) {
    unsigned d = 0;
    if (c >= '0' && c <= '9')
      d = static_cast<unsigned>(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = static_cast<unsigned>(c - 'a' + 10);
    else
      d = static_cast<unsigned>(c - 'A' + 10);
    v = (v << 4) | d;
  }
  out = v;
  return true;
}

inline RdmaTokenView parse_rdma_token(std::string_view token) {
  RdmaTokenView v;
  const auto colon = token.find(':');
  if (colon == std::string_view::npos) return v;
  const auto addr_field = token.substr(0, colon);
  if (addr_field.size() > 16 && rdma_hex_ok(addr_field)) {
    v.unsupported = true;
    return v;
  }
  std::uint64_t addr = 0;
  if (!rdma_parse_hex(addr_field, 16, addr) || addr == 0) {
    v.malformed = true;
    return v;
  }
  v.cuobject = true;
  v.has_addr = true;
  v.addr = addr;
  const auto rest = token.substr(colon + 1);
  const auto colon2 = rest.find(':');
  const auto size_field = colon2 == std::string_view::npos ? rest : rest.substr(0, colon2);
  if (size_field.empty()) return v;
  std::uint64_t size = 0;
  if (!rdma_parse_hex(size_field, 8, size)) {
    v.malformed = true;
    v.cuobject = false;
    v.has_addr = false;
    return v;
  }
  v.has_size = true;
  v.size = static_cast<std::uint32_t>(size);
  return v;
}

struct S3RdmaPutPlan {
  bool ok{false};
  int status{400};
  const char* message{""};
  std::size_t len{0};
};

// Content-Length > 0 is the object size (aios-cuobj-s3 and the AIOS NIXL plugin).
// Content-Length 0 is NIXL's accelerated PUT: the size is field 2 of the token.
// A mismatch, a missing size, or an RC token does not store an object.
inline S3RdmaPutPlan s3_plan_rdma_put(std::size_t content_len, std::string_view token) {
  S3RdmaPutPlan plan;
  const auto tok = parse_rdma_token(token);
  if (tok.unsupported) {
    plan.status = 501;
    plan.message = "RDMA token scheme is not supported";
    return plan;
  }
  if (tok.malformed) {
    plan.status = 400;
    plan.message = "malformed x-amz-rdma-token";
    return plan;
  }
  if (content_len > kCuObjectMaxTransferBytes ||
      (tok.has_size && tok.size > kCuObjectMaxTransferBytes)) {
    plan.status = 400;
    plan.message = "RDMA PUT exceeds 1 GiB";
    return plan;
  }
  if (content_len > 0 && tok.has_size && tok.size != content_len) {
    plan.status = 400;
    plan.message = "RDMA PUT Content-Length does not match the token size";
    return plan;
  }
  std::uint64_t len = content_len;
  if (len == 0 && tok.has_size) len = tok.size;
  if (len == 0) {
    plan.status = 501;
    plan.message = "RDMA PUT size is not in Content-Length or the token";
    return plan;
  }
  plan.ok = true;
  plan.len = static_cast<std::size_t>(len);
  return plan;
}

// Single-range and whole-object GETs both carry a token. Multi-range stays on TCP.
// `len` is the object span to push. A token buffer smaller than that span is not RDMA.
inline bool s3_want_rdma_get(const std::string& rdma_token, bool multi_range, bool is_get,
                             std::size_t len) {
  if (!is_get || rdma_token.empty() || multi_range || len == 0 || len > kCuObjectMaxTransferBytes) {
    return false;
  }
  const auto tok = parse_rdma_token(rdma_token);
  if (tok.unsupported || tok.malformed) return false;
  if (tok.has_size && len > tok.size) return false;
  return true;
}

// 200 when the range is absent or covers the whole object. 206 for a sub-range.
// NIXL accepts only the pair (HTTP 200, reply 200) or (HTTP 206, reply 206).
inline int s3_rdma_get_http_status(bool ranged, std::uint64_t start, std::uint64_t end,
                                   std::uint64_t object_size) {
  if (!ranged || object_size == 0) return 200;
  if (start == 0 && end + 1 == object_size) return 200;
  return 206;
}

inline bool s3_try_rdma_get(CuObjectEndpoint* ep, const std::string& object_key,
                            const std::string& token, const void* data, std::size_t len,
                            std::string& reply, std::string& err) {
  if (!ep || !ep->available()) {
    err = "cuobject unavailable";
    return false;
  }
  if (len == 0 || len > kCuObjectMaxTransferBytes) {
    err = "transfer size out of range";
    return false;
  }
  return ep->rdma_get(object_key, data, len, token, reply, err);
}

inline bool s3_try_rdma_put(CuObjectEndpoint* ep, const std::string& object_key,
                            const std::string& token, void* data, std::size_t len,
                            std::string& reply, std::string& err) {
  if (!ep || !ep->available()) {
    err = "cuobject unavailable";
    return false;
  }
  if (len == 0 || len > kCuObjectMaxTransferBytes) {
    err = "transfer size out of range";
    return false;
  }
  return ep->rdma_put(object_key, data, len, token, reply, err);
}

}  // namespace aios
