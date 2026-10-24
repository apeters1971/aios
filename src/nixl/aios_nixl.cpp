#include "nixl/aios_nixl.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <utility>
#include <vector>

namespace aios {
namespace {

struct Slice {
  std::string key;
  std::string token;
  std::size_t len{0};
};

struct Result {
  bool ok{false};
  std::string err;
};

NixlStatus status_of(bool abort, int state, bool failed) {
  if (abort && state != 2) return NixlStatus::Canceled;
  if (state == 0) return NixlStatus::NotPosted;
  if (state == 1) return NixlStatus::InProg;
  if (failed) return NixlStatus::Backend;
  if (abort) return NixlStatus::Canceled;
  return NixlStatus::Success;
}

bool range_inside(std::uint64_t base, std::size_t base_len, std::uint64_t addr, std::size_t len) {
  if (addr != base || len != base_len) return false;
  if (base_len == 0) return false;
  return true;
}

}  // namespace

struct NixlReq {
  NixlOp op{NixlOp::Read};
  std::vector<Slice> slices;
  mutable std::mutex mu;
  std::atomic<bool> abort{false};
  // 0 idle, 1 running, 2 finished.
  int state{0};
  bool failed{false};
  std::string err;
  std::future<Result> fut;
};

struct NixlEngine::Live {
  std::mutex mu;
  std::vector<NixlReq*> reqs;
};

NixlEngine::NixlEngine(NixlConfig cfg, std::shared_ptr<NixlRdmaTransport> transport)
    : cfg_(std::move(cfg)), transport_(std::move(transport)), live_(std::make_unique<Live>()) {
  if (!transport_) {
    transport_ = make_s3_cuobject_transport(cfg_);
    if (!transport_) {
      init_error_ = "NIXL AIOS backend needs s3_endpoint (host:port), access_key, and secret_key";
      return;
    }
  }
  ok_ = true;
}

NixlEngine::~NixlEngine() {
  std::vector<NixlReq*> reqs;
  {
    std::lock_guard lock(live_->mu);
    reqs.swap(live_->reqs);
  }
  for (NixlReq* req : reqs) {
    req->abort.store(true);
    if (req->fut.valid()) req->fut.wait();
    delete req;
  }
}

NixlStatus NixlEngine::register_mem(const NixlReg& reg, NixlMemInfo& out, std::string& err) const {
  out = NixlMemInfo{};
  out.mem = reg.mem;
  out.addr = reg.addr;
  out.len = reg.len;
  out.dev_id = reg.dev_id;
  if (reg.mem == NixlMem::Obj) {
    const auto slash = reg.meta.find('/');
    if (reg.meta.empty() || slash == std::string::npos || slash == 0 ||
        slash + 1 >= reg.meta.size()) {
      err = "OBJ registration metaInfo must be bucket/key";
      return NixlStatus::InvalidParam;
    }
    out.obj_key = reg.meta;
    return NixlStatus::Success;
  }
  if (reg.mem != NixlMem::Dram && reg.mem != NixlMem::Vram) {
    err = "unsupported memory type";
    return NixlStatus::NotSupported;
  }
  out.rdma_token = reg.meta.empty() ? cfg_.rdma_token : reg.meta;
  if (out.rdma_token.empty()) {
    err = "DRAM/VRAM registration needs a cuObject RDMA token in metaInfo";
    return NixlStatus::InvalidParam;
  }
  if (reg.len == 0 || reg.len > kNixlMaxRdmaBytes) {
    err = "buffer length is outside the cuObject transfer limit";
    return NixlStatus::InvalidParam;
  }
  return NixlStatus::Success;
}

NixlStatus NixlEngine::prep(NixlOp op, const std::vector<NixlXferDesc>& local,
                            const std::vector<NixlXferDesc>& remote, NixlReq*& out,
                            std::string& err) {
  out = nullptr;
  if (!ok_ || !transport_) {
    err = init_error_.empty() ? "engine not initialized" : init_error_;
    return NixlStatus::Backend;
  }
  if (local.empty() || local.size() != remote.size()) {
    err = "local and remote descriptor counts must match and be non-empty";
    return NixlStatus::InvalidParam;
  }
  auto req = std::make_unique<NixlReq>();
  req->op = op;
  req->slices.reserve(local.size());
  for (std::size_t i = 0; i < local.size(); ++i) {
    const NixlXferDesc& mem = local[i];
    const NixlXferDesc& obj = remote[i];
    if (!mem.info || !obj.info) {
      err = "descriptor is missing registration metadata";
      return NixlStatus::InvalidParam;
    }
    if (mem.info->mem == NixlMem::Obj || obj.info->mem != NixlMem::Obj) {
      err = "local descriptors must be DRAM or VRAM and remote descriptors OBJ";
      return NixlStatus::NotSupported;
    }
    if (mem.info->rdma_token.empty() || obj.info->obj_key.empty()) {
      err = "missing RDMA token or object key";
      return NixlStatus::InvalidParam;
    }
    if (obj.addr != 0) {
      err = "cuObject RDMA does not support a byte range";
      return NixlStatus::NotSupported;
    }
    if (mem.len != obj.len || mem.len == 0 || mem.len > kNixlMaxRdmaBytes) {
      err = "transfer length must match and be in (0, 1 GiB]";
      return NixlStatus::InvalidParam;
    }
    if (!range_inside(mem.info->addr, mem.info->len, mem.addr, mem.len)) {
      err = "transfer must cover the exact registered buffer (the RDMA token is for that buffer)";
      return NixlStatus::InvalidParam;
    }
    if (obj.info->len != 0 && obj.len > obj.info->len) {
      err = "transfer is larger than the registered object";
      return NixlStatus::InvalidParam;
    }
    req->slices.push_back(Slice{obj.info->obj_key, mem.info->rdma_token, mem.len});
  }
  out = req.get();
  {
    std::lock_guard lock(live_->mu);
    live_->reqs.push_back(req.get());
  }
  req.release();
  return NixlStatus::Success;
}

NixlStatus NixlEngine::post(NixlReq* req) {
  if (!req) return NixlStatus::InvalidParam;
  std::lock_guard lock(req->mu);
  if (req->fut.valid()) {
    if (req->fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      return NixlStatus::RepostActive;
    }
    Result done = req->fut.get();
    req->state = 2;
    req->failed = !done.ok && done.err != "canceled";
    if (done.err == "canceled") req->abort.store(true);
    req->err = std::move(done.err);
  }
  if (req->state == 1) return NixlStatus::RepostActive;
  if (req->failed) return NixlStatus::Backend;
  req->abort.store(false);
  req->state = 1;
  req->failed = false;
  req->err.clear();
  auto transport = transport_;
  auto* raw = req;
  req->fut = std::async(std::launch::async, [transport, raw]() {
    Result result;
    result.ok = true;
    try {
      for (const Slice& slice : raw->slices) {
        if (raw->abort.load()) {
          result.ok = false;
          result.err = "canceled";
          return result;
        }
        std::string err;
        const bool ok = raw->op == NixlOp::Read
                            ? transport->read_object(slice.key, slice.len, slice.token, err)
                            : transport->write_object(slice.key, slice.len, slice.token, err);
        if (!ok) {
          result.ok = false;
          result.err = err.empty() ? "cuObject transfer failed" : err;
          return result;
        }
      }
    } catch (const std::exception& e) {
      result.ok = false;
      result.err = e.what();
    } catch (...) {
      result.ok = false;
      result.err = "unknown transfer error";
    }
    return result;
  });
  if (req->fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    Result done = req->fut.get();
    req->state = 2;
    req->failed = !done.ok && done.err != "canceled";
    if (done.err == "canceled" || req->abort.load()) req->abort.store(true);
    req->err = std::move(done.err);
    if (req->abort.load() && !req->failed) return NixlStatus::Canceled;
    return req->failed ? NixlStatus::Backend : NixlStatus::Success;
  }
  return NixlStatus::InProg;
}

NixlStatus NixlEngine::check(NixlReq* req) {
  if (!req) return NixlStatus::InvalidParam;
  std::lock_guard lock(req->mu);
  if (req->state == 0) return NixlStatus::NotPosted;
  if (req->fut.valid()) {
    if (req->fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      return req->abort.load() ? NixlStatus::Canceled : NixlStatus::InProg;
    }
    Result done = req->fut.get();
    req->state = 2;
    req->failed = !done.ok && done.err != "canceled";
    if (done.err == "canceled") req->abort.store(true);
    req->err = std::move(done.err);
  }
  return status_of(req->abort.load(), req->state, req->failed);
}

NixlStatus NixlEngine::release(NixlReq*& req) {
  if (!req) return NixlStatus::InvalidParam;
  {
    std::lock_guard lock(req->mu);
    req->abort.store(true);
    if (req->fut.valid() &&
        req->fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      return NixlStatus::Backend;
    }
    if (req->fut.valid()) {
      Result done = req->fut.get();
      req->state = 2;
      req->err = std::move(done.err);
    }
  }
  {
    std::lock_guard lock(live_->mu);
    auto& reqs = live_->reqs;
    for (auto it = reqs.begin(); it != reqs.end(); ++it) {
      if (*it == req) {
        reqs.erase(it);
        break;
      }
    }
  }
  delete req;
  req = nullptr;
  return NixlStatus::Success;
}

std::string NixlEngine::last_error(const NixlReq* req) const {
  if (!req) return {};
  std::lock_guard lock(req->mu);
  return req->err;
}

}  // namespace aios
