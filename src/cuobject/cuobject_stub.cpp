#include "cuobject/cuobject_endpoint.hpp"

#include <cstring>

namespace aios {

bool StubCuObjectEndpoint::rdma_get(const std::string& key, const void* local, std::size_t len,
                                    const std::string& token, std::string& reply,
                                    std::string& err) {
  (void)local;
  ++get_calls_;
  last_key_ = key;
  last_get_token_ = token;
  last_get_size_ = len;
  if (fail_) {
    err = "stub rdma_get failed";
    return false;
  }
  if (token.empty()) {
    err = "empty rdma token";
    return false;
  }
  reply = "stub-ok";
  return true;
}

bool StubCuObjectEndpoint::rdma_put(const std::string& key, void* local, std::size_t len,
                                    const std::string& token, std::string& reply,
                                    std::string& err) {
  ++put_calls_;
  last_key_ = key;
  last_put_token_ = token;
  last_put_size_ = len;
  if (fail_) {
    err = "stub rdma_put failed";
    return false;
  }
  if (token.empty()) {
    err = "empty rdma token";
    return false;
  }
  if (!local && len) {
    err = "null buffer";
    return false;
  }
  if (len) {
    if (put_payload_.size() >= len) {
      std::memcpy(local, put_payload_.data(), len);
    } else if (!put_payload_.empty()) {
      std::memcpy(local, put_payload_.data(), put_payload_.size());
      std::memset(static_cast<std::uint8_t*>(local) + put_payload_.size(), 0xAB,
                  len - put_payload_.size());
    } else {
      std::memset(local, 0xAB, len);
    }
  }
  reply = "stub-ok";
  return true;
}

}  // namespace aios
