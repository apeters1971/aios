#include "cuobject/cuobject_endpoint.hpp"

#include "util/log.hpp"

#include <memory>
#include <mutex>
#include <string>

#if defined(AIOS_HAVE_CUOBJECT) && AIOS_HAVE_CUOBJECT
// NVIDIA cuObjServer (separate partner package). Header name may vary by install.
#if __has_include(<cuObjServer.h>)
#include <cuObjServer.h>
#elif __has_include(<cuobject/cuObjServer.h>)
#include <cuobject/cuObjServer.h>
#else
#error "AIOS_HAVE_CUOBJECT set but cuObjServer.h not found"
#endif
#endif

namespace aios {
namespace {

#if defined(AIOS_HAVE_CUOBJECT) && AIOS_HAVE_CUOBJECT

class NvidiaCuObjectEndpoint final : public CuObjectEndpoint {
 public:
  NvidiaCuObjectEndpoint(std::string ip, unsigned short port)
      : ip_(std::move(ip)), port_(port) {
    try {
      server_ = std::make_unique<cuObjServer>(ip_.c_str(), port_,
                                              static_cast<unsigned>(CUOBJ_PROTO_RDMA_DC_V1));
      connected_ = server_ && server_->isConnected();
      if (!connected_) {
        AIOS_LOG_WARN("cuObjServer constructed but isConnected()=false ip=", ip_, " port=", port_);
      } else {
        AIOS_LOG_INFO("cuObjServer listening on ", ip_, ":", port_);
      }
    } catch (const std::exception& e) {
      AIOS_LOG_ERROR("cuObjServer init failed: ", e.what());
      server_.reset();
      connected_ = false;
    }
  }

  bool available() const override { return connected_ && server_ != nullptr; }

  bool rdma_get(const std::string& key, const void* local, std::size_t len,
                const std::string& token, std::string& reply, std::string& err) override {
    if (!available()) {
      err = "cuobject not connected";
      return false;
    }
    std::lock_guard lock(mu_);
    void* buf = const_cast<void*>(local);
    // Prefer registering caller memory; fall back to alloc+copy if required by SDK.
    struct rdma_buffer* handle = server_->registerBuffer(buf, len);
    if (!handle) {
      err = "registerBuffer failed";
      return false;
    }
    const uint16_t ch = server_->allocateChannelId();
    ssize_t n = -1;
    if (ch != INVALID_CHANNEL_ID) {
      n = server_->handleGetObject(key, handle, 0, len, token, uint32_t{0}, ch);
      server_->freeChannelId(ch);
    } else {
      n = server_->handleGetObject(key, handle, 0, len, token, uint32_t{0});
    }
    server_->deRegisterBuffer(handle);
    if (n < 0 || static_cast<std::size_t>(n) != len) {
      err = "handleGetObject failed rc=" + std::to_string(n);
      return false;
    }
    reply = "ok";
    return true;
  }

  bool rdma_put(const std::string& key, void* local, std::size_t len, const std::string& token,
                std::string& reply, std::string& err) override {
    if (!available()) {
      err = "cuobject not connected";
      return false;
    }
    std::lock_guard lock(mu_);
    struct rdma_buffer* handle = server_->registerBuffer(local, len);
    if (!handle) {
      err = "registerBuffer failed";
      return false;
    }
    const uint16_t ch = server_->allocateChannelId();
    ssize_t n = -1;
    if (ch != INVALID_CHANNEL_ID) {
      n = server_->handlePutObject(key, handle, 0, len, token, uint32_t{0}, ch);
      server_->freeChannelId(ch);
    } else {
      n = server_->handlePutObject(key, handle, 0, len, token, uint32_t{0});
    }
    server_->deRegisterBuffer(handle);
    if (n < 0 || static_cast<std::size_t>(n) != len) {
      err = "handlePutObject failed rc=" + std::to_string(n);
      return false;
    }
    reply = "ok";
    return true;
  }

 private:
  std::string ip_;
  unsigned short port_;
  std::unique_ptr<cuObjServer> server_;
  bool connected_{false};
  std::mutex mu_;
};

bool split_listen(const std::string& listen, std::string& host, unsigned short& port) {
  auto colon = listen.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= listen.size()) return false;
  host = listen.substr(0, colon);
  if (!host.empty() && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  try {
    int p = std::stoi(listen.substr(colon + 1));
    if (p <= 0 || p > 65535) return false;
    port = static_cast<unsigned short>(p);
  } catch (...) {
    return false;
  }
  return true;
}

#endif  // AIOS_HAVE_CUOBJECT

}  // namespace

std::shared_ptr<CuObjectEndpoint> make_cuobject_endpoint(const Config& cfg) {
  if (cfg.cuobject_listen.empty()) return make_null_cuobject_endpoint();

#if defined(AIOS_HAVE_CUOBJECT) && AIOS_HAVE_CUOBJECT
  std::string host;
  unsigned short port = 0;
  if (!split_listen(cfg.cuobject_listen, host, port)) {
    AIOS_LOG_ERROR("bad cuobject_listen: ", cfg.cuobject_listen);
    return make_null_cuobject_endpoint();
  }
  if (host == "0.0.0.0" || host == "*" || host.empty()) host = "0.0.0.0";
  auto ep = std::make_shared<NvidiaCuObjectEndpoint>(host, port);
  if (!ep->available()) {
    AIOS_LOG_WARN("cuobject_listen set but endpoint not available; RDMA disabled");
    return make_null_cuobject_endpoint();
  }
  return ep;
#else
  AIOS_LOG_WARN("cuobject_listen set but AIOS built without cuObjServer (AIOS_HAVE_CUOBJECT=0)");
  return make_null_cuobject_endpoint();
#endif
}

}  // namespace aios
