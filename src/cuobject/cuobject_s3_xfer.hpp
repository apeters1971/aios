#pragma once

#include "cuobject/cuobject_endpoint.hpp"

#include <string>

namespace aios {

// Shared control-plane helpers used by S3Server (also unit-tested).

inline bool s3_want_rdma_get(const std::string& rdma_token, bool ranged, bool is_get) {
  return is_get && !rdma_token.empty() && !ranged;
}

inline bool s3_try_rdma_get(CuObjectEndpoint* ep, const std::string& object_key,
                            const std::string& token, const void* data, std::size_t len,
                            std::string& reply, std::string& err) {
  if (!ep || !ep->available()) {
    err = "cuobject unavailable";
    return false;
  }
  if (len > kCuObjectMaxTransferBytes) {
    err = "transfer exceeds 1 GiB limit";
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
  if (len == 0) {
    err = "RDMA PUT requires Content-Length > 0";
    return false;
  }
  if (len > kCuObjectMaxTransferBytes) {
    err = "transfer exceeds 1 GiB limit";
    return false;
  }
  return ep->rdma_put(object_key, data, len, token, reply, err);
}

}  // namespace aios
