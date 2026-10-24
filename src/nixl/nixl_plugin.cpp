// NIXL storage plugin "AIOS". Loaded as libplugin_AIOS.so.
// Build against the same NIXL_PLUGIN_API_VERSION as the libnixl that dlopens it.

#include "nixl/aios_nixl.hpp"

#include "backend/backend_engine.h"
#include "backend/backend_plugin.h"

#include <memory>
#include <string>
#include <vector>

namespace {

class Engine final : public nixlBackendEngine {
 public:
  explicit Engine(const nixlBackendInitParams* init_params) : nixlBackendEngine(init_params) {
    aios::NixlConfig cfg;
    cfg.s3_endpoint = param("s3_endpoint");
    cfg.access_key = param("access_key");
    cfg.secret_key = param("secret_key");
    cfg.region = param("region");
    if (cfg.region.empty()) cfg.region = "us-east-1";
    cfg.rdma_token = param("rdma_token");
    const auto timeout = param("socket_timeout_ms");
    if (!timeout.empty()) {
      try {
        cfg.socket_timeout_ms = std::stoi(timeout);
      } catch (...) {
        cfg.socket_timeout_ms = 30000;
      }
    }
    inner_ = std::make_unique<aios::NixlEngine>(std::move(cfg), nullptr);
    if (!inner_->ok()) initErr = true;
  }

  bool supportsRemote() const override { return false; }
  bool supportsLocal() const override { return true; }
  bool supportsNotif() const override { return false; }

  nixl_mem_list_t getSupportedMems() const override { return {DRAM_SEG, VRAM_SEG, OBJ_SEG}; }

  nixl_status_t connect(const std::string&) override { return NIXL_SUCCESS; }
  nixl_status_t disconnect(const std::string&) override { return NIXL_SUCCESS; }
  nixl_status_t loadLocalMD(nixlBackendMD* input, nixlBackendMD*& output) override {
    output = input;
    return NIXL_SUCCESS;
  }
  nixl_status_t unloadMD(nixlBackendMD*) override { return NIXL_SUCCESS; }

  nixl_status_t registerMem(const nixlBlobDesc& mem, const nixl_mem_t& nixl_mem,
                            nixlBackendMD*& out) override {
    out = nullptr;
    aios::NixlReg reg;
    reg.addr = mem.addr;
    reg.len = mem.len;
    reg.dev_id = mem.devId;
    reg.meta = mem.metaInfo;
    switch (nixl_mem) {
      case DRAM_SEG:
        reg.mem = aios::NixlMem::Dram;
        break;
      case VRAM_SEG:
        reg.mem = aios::NixlMem::Vram;
        break;
      case OBJ_SEG:
        reg.mem = aios::NixlMem::Obj;
        break;
      default:
        return NIXL_ERR_NOT_SUPPORTED;
    }
    aios::NixlMemInfo info;
    std::string err;
    const auto st = inner_->register_mem(reg, info, err);
    if (st != aios::NixlStatus::Success) return to_status(st);
    out = new PluginMd(std::move(info));
    return NIXL_SUCCESS;
  }

  nixl_status_t deregisterMem(nixlBackendMD* meta) override {
    delete static_cast<PluginMd*>(meta);
    return NIXL_SUCCESS;
  }

  nixl_status_t prepXfer(const nixl_xfer_op_t& operation, const nixl_meta_dlist_t& local,
                         const nixl_meta_dlist_t& remote, const std::string&,
                         nixlBackendReqH*& handle, const nixl_opt_b_args_t*) const override {
    handle = nullptr;
    if (operation != NIXL_READ && operation != NIXL_WRITE) return NIXL_ERR_NOT_SUPPORTED;
    aios::NixlReq* req = nullptr;
    std::string err;
    const auto st = inner_->prep(operation == NIXL_READ ? aios::NixlOp::Read : aios::NixlOp::Write,
                                 to_descs(local), to_descs(remote), req, err);
    if (st != aios::NixlStatus::Success) return to_status(st);
    handle = new PluginReq(req);
    return NIXL_SUCCESS;
  }

  nixl_status_t postXfer(const nixl_xfer_op_t&, const nixl_meta_dlist_t&, const nixl_meta_dlist_t&,
                         const std::string&, nixlBackendReqH*& handle,
                         const nixl_opt_b_args_t*) const override {
    auto* wrapped = static_cast<PluginReq*>(handle);
    if (!wrapped || !wrapped->req) return NIXL_ERR_INVALID_PARAM;
    return to_status(inner_->post(wrapped->req));
  }

  nixl_status_t checkXfer(nixlBackendReqH* handle) const override {
    auto* wrapped = static_cast<PluginReq*>(handle);
    if (!wrapped || !wrapped->req) return NIXL_ERR_INVALID_PARAM;
    return to_status(inner_->check(wrapped->req));
  }

  nixl_status_t releaseReqH(nixlBackendReqH* handle) const override {
    auto* wrapped = static_cast<PluginReq*>(handle);
    if (!wrapped) return NIXL_ERR_INVALID_PARAM;
    const auto st = inner_->release(wrapped->req);
    if (st == aios::NixlStatus::Success) delete wrapped;
    return to_status(st);
  }

 private:
  struct PluginMd : nixlBackendMD {
    explicit PluginMd(aios::NixlMemInfo info) : nixlBackendMD(true), info(std::move(info)) {}
    aios::NixlMemInfo info;
  };

  struct PluginReq : nixlBackendReqH {
    explicit PluginReq(aios::NixlReq* req) : req(req) {}
    aios::NixlReq* req{nullptr};
  };

  static nixl_status_t to_status(aios::NixlStatus st) {
    return static_cast<nixl_status_t>(static_cast<int>(st));
  }

  static std::vector<aios::NixlXferDesc> to_descs(const nixl_meta_dlist_t& list) {
    std::vector<aios::NixlXferDesc> out;
    out.reserve(static_cast<std::size_t>(list.descCount()));
    for (int i = 0; i < list.descCount(); ++i) {
      const auto& d = list[static_cast<std::size_t>(i)];
      aios::NixlXferDesc desc;
      desc.addr = d.addr;
      desc.len = d.len;
      desc.dev_id = d.devId;
      if (d.metadataP) desc.info = &static_cast<const PluginMd*>(d.metadataP)->info;
      out.push_back(desc);
    }
    return out;
  }

  std::string param(const char* key) const {
    std::string value;
    if (getInitParam(key, value) != NIXL_SUCCESS) return {};
    return value;
  }

  // prep/post/check/release are const on the NIXL base; the engine still mutates requests.
  mutable std::unique_ptr<aios::NixlEngine> inner_;
};

static_assert(static_cast<int>(aios::NixlStatus::InProg) == static_cast<int>(NIXL_IN_PROG),
              "NixlStatus must match nixl_status_t");
static_assert(static_cast<int>(aios::NixlStatus::Success) == static_cast<int>(NIXL_SUCCESS),
              "NixlStatus must match nixl_status_t");
static_assert(static_cast<int>(aios::NixlStatus::Canceled) == static_cast<int>(NIXL_ERR_CANCELED),
              "NixlStatus must match nixl_status_t");

const char* get_plugin_name() {
  return "AIOS";
}
const char* get_plugin_version() {
  return "1.0.0";
}

nixl_b_params_t get_backend_options() {
  return {{"s3_endpoint", ""},     {"access_key", ""}, {"secret_key", ""},
          {"region", "us-east-1"}, {"rdma_token", ""}, {"socket_timeout_ms", "30000"}};
}

nixl_mem_list_t get_backend_mems() {
  return {DRAM_SEG, VRAM_SEG, OBJ_SEG};
}

nixlBackendEngine* create_engine(const nixlBackendInitParams* init_params) {
  try {
    return new Engine(init_params);
  } catch (const std::exception&) {
    return nullptr;
  }
}

void destroy_engine(nixlBackendEngine* engine) {
  delete engine;
}

}  // namespace

nixlBackendPlugin plugin = {
    NIXL_PLUGIN_API_VERSION, create_engine,       destroy_engine,   get_plugin_name,
    get_plugin_version,      get_backend_options, get_backend_mems,
};

extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin* nixl_plugin_init() {
  return &plugin;
}

extern "C" NIXL_PLUGIN_EXPORT void nixl_plugin_fini() {}
