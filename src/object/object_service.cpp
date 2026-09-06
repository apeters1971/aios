#include "object/object_service.hpp"

#include "cluster/place.hpp"
#include "ec/codec_factory.hpp"
#include "ec/ec_attrs.hpp"
#include "net/object_client.hpp"
#include "object/archive_bag.hpp"
#include "object/archive_pack.hpp"
#include "object/object_layout.hpp"
#include "util/auth.hpp"
#include "util/base64.hpp"
#include "util/compression.hpp"
#include "util/crc32c.hpp"
#include "util/file_io.hpp"
#include "util/log.hpp"

#include <utility>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace aios {
namespace {

struct PutPayload {
  const std::uint8_t* data{nullptr};
  std::size_t len{0};
  std::vector<std::uint8_t> owned;
  bool compressed{false};
  std::uint64_t logical_size{0};
  std::uint32_t logical_crc{0};
};

// Optionally compress a full PUT body. `expected_crc32c` is the logical CRC.
bool prepare_put_payload(const Config& cfg, OpsRegistry& ops, const std::uint8_t* data,
                         std::size_t len, std::optional<std::uint32_t> expected_crc32c,
                         PutPayload& out, std::string& err) {
  out.logical_size = len;
  out.logical_crc = crc32c(data, len);
  if (expected_crc32c && *expected_crc32c != out.logical_crc) {
    err = "crc32c mismatch";
    return false;
  }
  out.data = data;
  out.len = len;
  out.compressed = false;
  if (cfg.compression != kCompAlgoZstd || !zstd_available()) return true;
  if (len < cfg.compression_min_bytes) {
    ops.note_compress_skipped();
    return true;
  }
  std::string cerr;
  if (!zstd_compress(data, len, cfg.compression_level, out.owned, cerr)) {
    ops.note_compress_skipped();
    return true;
  }
  if (out.owned.size() >= len) {
    out.owned.clear();
    ops.note_compress_skipped();
    return true;
  }
  out.compressed = true;
  out.data = out.owned.data();
  out.len = out.owned.size();
  return true;
}

bool decompress_api_result(ApiResult& r, std::string& err, std::uint64_t max_object_bytes) {
  if (!attrs_are_compressed(r.attrs)) return true;
  auto logical = compression_full_size(r.attrs);
  if (!logical) {
    err = "compressed object missing aios.compression.full_size";
    return false;
  }
  if (*logical > max_object_bytes) {
    err = "compressed object full_size exceeds max_object_bytes";
    return false;
  }
  if (!r.data) {
    if (r.body_path.empty()) {
      err = "compressed object has no body";
      return false;
    }
    std::vector<std::uint8_t> stored;
    if (!file_read_all(r.body_path, stored, err)) {
      if (err.empty()) err = "cannot open compressed body";
      return false;
    }
    r.data = std::move(stored);
    r.body_path.clear();
  }
  std::vector<std::uint8_t> plain;
  if (!zstd_decompress(r.data->data(), r.data->size(), *logical, plain, err)) return false;
  r.data = std::move(plain);
  if (r.info) {
    r.info->size = *logical;
    auto it = r.attrs.find(kCompAttrFullCrc);
    if (it != r.attrs.end()) {
      try {
        r.info->crc32c = static_cast<std::uint32_t>(std::stoul(it->second));
        r.info->crc32c_known = true;
      } catch (...) {
      }
    }
  }
  return true;
}

// ---- Checked JSON scalar reads -------------------------------------------------
// Request bodies come off the wire; nlohmann's value() throws type_error when the
// element exists with a different type, which would unwind through the RPC worker.
// These return the default for absent/null keys and throw std::invalid_argument
// for a present value of the wrong type, which ObjectService::handle turns into a
// bad_request reply.

[[noreturn]] void bad_field(const char* key, const char* want) {
  throw std::invalid_argument(std::string("field '") + key + "' must be " + want);
}

const nlohmann::json* json_field(const nlohmann::json& body, const char* key) {
  if (!body.is_object()) return nullptr;
  auto it = body.find(key);
  if (it == body.end() || it->is_null()) return nullptr;
  return &*it;
}

std::uint64_t json_u64(const nlohmann::json& body, const char* key, std::uint64_t def = 0) {
  const auto* v = json_field(body, key);
  if (!v) return def;
  if (!v->is_number_integer()) bad_field(key, "an integer");
  if (v->is_number_unsigned()) return v->get<std::uint64_t>();
  const auto i = v->get<std::int64_t>();
  if (i < 0) bad_field(key, "a non-negative integer");
  return static_cast<std::uint64_t>(i);
}

std::uint32_t json_u32(const nlohmann::json& body, const char* key, std::uint32_t def = 0) {
  const auto v = json_u64(body, key, def);
  if (v > 0xffffffffull) bad_field(key, "a 32-bit integer");
  return static_cast<std::uint32_t>(v);
}

int json_int(const nlohmann::json& body, const char* key, int def) {
  const auto* v = json_field(body, key);
  if (!v) return def;
  if (!v->is_number_integer()) bad_field(key, "an integer");
  const auto i = v->get<std::int64_t>();
  if (i < INT32_MIN || i > INT32_MAX) bad_field(key, "a 32-bit integer");
  return static_cast<int>(i);
}

std::string json_str(const nlohmann::json& body, const char* key, const std::string& def = {}) {
  const auto* v = json_field(body, key);
  if (!v) return def;
  if (!v->is_string()) bad_field(key, "a string");
  return v->get<std::string>();
}

bool json_bool(const nlohmann::json& body, const char* key, bool def) {
  const auto* v = json_field(body, key);
  if (!v) return def;
  if (!v->is_boolean()) bad_field(key, "a boolean");
  return v->get<bool>();
}

// Present and non-null (used for optional scalars like seq / crc32c).
bool json_has(const nlohmann::json& body, const char* key) {
  return json_field(body, key) != nullptr;
}

// ---- Service lock ---------------------------------------------------------------
// mu_ is recursive because api_* nest (txn → prepare → install → replicate). A
// blocking peer RPC must run with the mutex fully released or two coordinators
// doing cross-node work deadlock (each waiting on the other's session worker).
// ServiceLock tracks the per-thread depth so UnlockForRpc can drop every level.

thread_local int t_service_lock_depth = 0;

struct ServiceLock {
  std::recursive_mutex& m;
  explicit ServiceLock(std::recursive_mutex& mu) : m(mu) {
    m.lock();
    ++t_service_lock_depth;
  }
  ~ServiceLock() {
    --t_service_lock_depth;
    m.unlock();
  }
  ServiceLock(const ServiceLock&) = delete;
  ServiceLock& operator=(const ServiceLock&) = delete;
};

// Release every level of mu_ this thread holds (none is fine) for the scope, then
// re-acquire the same depth.
struct UnlockForRpc {
  std::recursive_mutex& m;
  int depth;
  explicit UnlockForRpc(std::recursive_mutex& mu) : m(mu), depth(t_service_lock_depth) {
    for (int i = 0; i < depth; ++i) m.unlock();
    t_service_lock_depth = 0;
  }
  ~UnlockForRpc() {
    for (int i = 0; i < depth; ++i) m.lock();
    t_service_lock_depth = depth;
  }
  UnlockForRpc(const UnlockForRpc&) = delete;
  UnlockForRpc& operator=(const UnlockForRpc&) = delete;
};

// Joins on scope exit so an exception between emplace_back and join cannot
// destroy a joinable std::thread (which would std::terminate).
struct ThreadJoiner {
  std::vector<std::thread>& threads;
  explicit ThreadJoiner(std::vector<std::thread>& t) : threads(t) {}
  ~ThreadJoiner() {
    for (auto& w : threads) {
      if (w.joinable()) w.join();
    }
  }
  ThreadJoiner(const ThreadJoiner&) = delete;
  ThreadJoiner& operator=(const ThreadJoiner&) = delete;
};

}  // namespace

ObjectService::ObjectService(Config cfg, ClusterMap& map, LocalStores& stores)
    : cfg_(std::move(cfg)), map_(map), stores_(stores) {
  epoch_.store(map_.epoch, std::memory_order_release);
}

void ObjectService::set_advertise(std::string advertise) {
  advertise_ = std::move(advertise);
}

std::uint64_t ObjectService::update_cluster_map(ClusterMap m) {
  ServiceLock lock(mu_);
  const auto prev = map_.epoch;
  map_ = std::move(m);
  epoch_.store(map_.epoch, std::memory_order_release);
  if (gate_.consensus) fence_epoch_ = std::max(fence_epoch_, map_.epoch);
  if (prev != map_.epoch) {
    // Leases are scoped to the primary that granted them: fence the ones whose
    // object this node no longer owns, so a holder that keeps talking to us
    // (before it sees the redirect) cannot land a write the new primary never
    // saw. The new primary refuses the unknown token on its own.
    const auto fenced = locks_.fence_if([this](const std::string& oid) {
      const auto p = place(oid, map_, cfg_.default_storage_class);
      return p.acting_set.empty() || p.acting_set[0].node_id != cfg_.node_id;
    });
    if (fenced > 0) {
      AIOS_LOG_INFO("cluster map epoch ", map_.epoch, ": fenced ", fenced,
                    " lease(s) on objects that moved to another primary");
    }
  }
  return prev;
}

void ObjectService::set_map_gate(MapGate gate) {
  ServiceLock lock(mu_);
  gate_ = std::move(gate);
  if (gate_.consensus) fence_epoch_ = std::max(fence_epoch_, map_.epoch);
}

ObjectService::MapGate ObjectService::map_gate() const {
  ServiceLock lock(mu_);
  return gate_;
}

ClusterMap ObjectService::map_snapshot() const {
  ServiceLock lock(mu_);
  return map_;
}

void ObjectService::shutdown_waiters() {
  // No mu_ here: waiters release it before blocking, and the hubs have their own.
  watches_.shutdown();
  pubsub_.shutdown();
}

Frame ObjectService::reply_ok(std::uint64_t epoch) const {
  Frame f;
  f.type = MsgType::ObjectReply;
  f.body = {{"ok", true}, {"epoch", epoch}};
  return f;
}

Frame ObjectService::reply_err(std::uint64_t epoch, const std::string& code,
                               const std::string& error) const {
  Frame f;
  f.type = MsgType::ObjectReply;
  f.body = {{"ok", false}, {"epoch", epoch}, {"code", code}, {"error", error}};
  return f;
}

ApiResult ObjectService::fail(const std::string& code, const std::string& error) const {
  // not_primary and not_local are redirect signals, not operator errors.
  if (code != "not_primary" && code != "not_local" && code != "map_transition" &&
      code != "no_map_lease") {
    ops_.note_error();
  }
  ApiResult r;
  r.ok = false;
  r.code = code;
  r.error = error;
  r.epoch = cur_epoch();
  return r;
}

ApiResult ObjectService::require_primary(const std::string& oid, Placement& placement_out,
                                         const std::string& storage_class) {
  const std::string sc =
      storage_class.empty() ? cfg_.default_storage_class : storage_class;
  placement_out = place(oid, map_, sc);
  if (placement_out.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement_out.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement_out;
    return r;
  }
  if (auto g = primary_gate(oid, placement_out); !g.ok) return g;
  ApiResult ok;
  ok.ok = true;
  ok.epoch = cur_epoch();
  ok.placement = placement_out;
  return ok;
}

ApiResult ObjectService::primary_gate(const std::string& oid, const Placement& placement) {
  ApiResult ok;
  ok.ok = true;
  ok.epoch = cur_epoch();
  if (!gate_.consensus) return ok;
  // Consensus mode: we may only act as primary while the map leader still
  // vouches for us, and, when this epoch made us primary of `oid`, only once the
  // previous primary has provably stopped (see MapGate).
  if (!gate_.lease_valid) {
    return fail("no_map_lease", "no cluster-map lease from the leader; retry");
  }
  if (map_.epoch > gate_.active_epoch) {
    bool was_primary = false;
    if (gate_.previous && !gate_.previous->targets.empty()) {
      const auto n = static_cast<int>(placement.acting_set.size());
      const auto prev = place(oid, *gate_.previous, n > 0 ? n : gate_.previous->replica_count,
                              placement.storage_class);
      was_primary = !prev.acting_set.empty() && prev.acting_set[0].node_id == cfg_.node_id;
    }
    if (!was_primary) {
      return fail("map_transition",
                  "primary changed in a map epoch that is not active yet; retry");
    }
  }
  return ok;
}

ApiResult ObjectService::enforce_lock(const std::string& oid,
                                     const std::optional<std::string>& token) {
  if (auto code = locks_.check_mutate(oid, token)) {
    return fail(*code, "object is locked");
  }
  ApiResult ok;
  ok.ok = true;
  ok.epoch = cur_epoch();
  return ok;
}

void ObjectService::signal_watch(const std::string& oid, std::uint64_t seq,
                                const std::string& op) {
  WatchEvent ev;
  ev.oid = oid;
  ev.seq = seq;
  ev.op = op;
  ev.ts_ms = now_ms();
  watches_.notify(std::move(ev));
}

int ObjectService::quorum_need(const Placement& placement) const {
  const int configured = cfg_.write_quorum > 0 ? cfg_.write_quorum : map_.replica_count;
  return std::min(configured, static_cast<int>(placement.acting_set.size()));
}

bool ObjectService::epoch_ok(std::uint64_t req_epoch, Frame& err_out) const {
  if (req_epoch == 0 || req_epoch == cur_epoch()) return true;
  // Slow path: the mirror may lag a map that was swapped in from outside
  // update_cluster_map; consult the real map under the lock and refresh.
  ServiceLock lock(mu_);
  epoch_.store(map_.epoch, std::memory_order_release);
  if (req_epoch == map_.epoch) return true;
  if (gate_.consensus) {
    // Monotonic epochs: a request from a newer committed map is fine (we are
    // behind and will catch up); anything older than the newest epoch we have
    // seen comes from a primary that has been superseded, and is fenced.
    auto* self = const_cast<ObjectService*>(this);
    if (req_epoch > fence_epoch_) {
      self->fence_epoch_ = req_epoch;
      return true;
    }
    if (req_epoch == fence_epoch_) return true;
  }
  err_out = reply_err(map_.epoch, "epoch_mismatch", "cluster map epoch mismatch");
  err_out.body["cluster_map"] = map_.to_json();
  return false;
}

ObjectStore* ObjectService::primary_store(const Placement& p, std::string& err) {
  if (p.acting_set.empty()) {
    err = "no targets";
    return nullptr;
  }
  const auto& t = p.acting_set[0];
  if (t.node_id != cfg_.node_id) {
    err = "not primary node";
    return nullptr;
  }
  auto* store = stores_.get(t.aios_path);
  if (!store) {
    err = "no local store for primary path";
    return nullptr;
  }
  return store;
}

PrecondResult ObjectService::check_preds_on(ObjectStore* store, const std::string& oid,
                                           const std::vector<AttrPrecondition>& preds,
                                           std::string& err) {
  if (!store) {
    err = "no store";
    return PrecondResult::NotFound;
  }
  return store->check_preconditions(oid, preds, err);
}

bool ObjectService::local_install(const std::string& aios_path, const PreparedVersion& v,
                                 const std::uint8_t* data, std::size_t len,
                                 const std::unordered_map<std::string, std::string>& attrs,
                                 std::string& err) {
  auto* store = stores_.get(aios_path);
  if (!store) {
    err = "no local store for " + aios_path;
    return false;
  }
  return store->install_version(v, data, len, attrs, err);
}

bool ObjectService::local_install_file(
    const std::string& aios_path, const PreparedVersion& v, const std::string& abs_body_path,
    const std::unordered_map<std::string, std::string>& attrs, std::string& err) {
  auto* store = stores_.get(aios_path);
  if (!store) {
    err = "no local store for " + aios_path;
    return false;
  }
  PreparedVersion pv = v;
  std::string rel;
  if (!v.is_delete && v.redirect_oid.empty() && v.size > 0) {
    // Always copy — source path is shared across replica installs / primary body.
    std::string staging;
    if (!store->create_staging_file(v.oid, staging, err)) return false;
    if (!file_copy(abs_body_path, staging, err)) {
      err = "copy staging failed";
      return false;
    }
    if (!store->place_staging_as_version(v.oid, v.seq, staging, rel, err)) return false;
    pv.fs_path = rel;
    pv.inline_body = false;
  }
  return store->install_version(pv, nullptr, 0, attrs, err);
}

bool ObjectService::local_publish(const std::string& aios_path, const std::string& oid,
                                 std::uint64_t seq, std::string& err) {
  auto* store = stores_.get(aios_path);
  if (!store) {
    err = "no local store for " + aios_path;
    return false;
  }
  return store->publish_tip(oid, seq, err);
}

bool ObjectService::local_abort(const std::string& aios_path, const std::string& oid,
                               std::uint64_t seq, std::string& err) {
  auto* store = stores_.get(aios_path);
  if (!store) {
    err = "no local store for " + aios_path;
    return false;
  }
  return store->abort_version(oid, seq, err);
}

namespace {

bool placement_has_remote(const Placement& placement, const std::string& node_id) {
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    if (placement.acting_set[i].node_id != node_id) return true;
  }
  return false;
}

// Per-oid mutation guard. Acquisition releases the service lock while waiting:
// callers such as txn/pubsub paths already hold mu_ when they reach api_put, and
// the holder of the oid guard needs mu_ to finish (lock-order inversion otherwise).
struct MutatingOid {
  std::mutex& mu;
  std::condition_variable& cv;
  std::unordered_set<std::string>& oids;
  std::string oid;
  MutatingOid(std::recursive_mutex& svc_mu, std::mutex& mu, std::condition_variable& cv,
              std::unordered_set<std::string>& oids, std::string o)
      : mu(mu), cv(cv), oids(oids), oid(std::move(o)) {
    UnlockForRpc unlock(svc_mu);
    std::unique_lock lk(mu);
    cv.wait(lk, [&] { return this->oids.count(oid) == 0; });
    this->oids.insert(oid);
  }
  ~MutatingOid() {
    std::lock_guard lk(mu);
    oids.erase(oid);
    cv.notify_all();
  }
  MutatingOid(const MutatingOid&) = delete;
  MutatingOid& operator=(const MutatingOid&) = delete;
};

}  // namespace

int ObjectService::replicate_install(
    const Placement& placement, const PreparedVersion& v, const std::uint8_t* data,
    std::size_t len, const std::unordered_map<std::string, std::string>& attrs,
    const std::string& abs_body_path) {
  const bool use_file =
      !v.inline_body && !v.is_delete && v.redirect_oid.empty() && v.size > 0 &&
      (!abs_body_path.empty() || (data == nullptr));
  // Always stage from the FS body when we have a path. The old ">4MiB" gate left
  // 256KiB–4MiB replicas on base64 ObjectPut (JSON blow-up + huge copies).
  const bool file_stream = use_file && !abs_body_path.empty();

  if (placement.acting_set.size() <= 1) return 0;
  std::optional<UnlockForRpc> unlock;
  if (placement_has_remote(placement, cfg_.node_id)) unlock.emplace(mu_);

  // One primary read shared by all replica workers (avoids 2× re-read for r=3).
  // Above the cap each peer streams the FS body in kStageChunkSize pieces instead
  // of pinning a whole-object copy per in-flight PUT.
  constexpr std::uint64_t kSharedFanoutMax = 32ull * 1024ull * 1024ull;
  std::shared_ptr<const std::vector<std::uint8_t>> shared_body;
  const std::uint8_t* fanout = data;
  std::size_t fanout_len = len;
  bool use_shared = false;
  if (data != nullptr && len == v.size && len > 0) {
    use_shared = true;
  } else if (file_stream && v.size > 0 && v.size <= kSharedFanoutMax) {
    auto buf = std::make_shared<std::vector<std::uint8_t>>();
    std::string rerr;
    if (file_read_exact(abs_body_path, static_cast<std::size_t>(v.size), *buf, rerr)) {
      shared_body = std::move(buf);
      fanout = shared_body->data();
      fanout_len = shared_body->size();
      use_shared = true;
    }
  }

  std::atomic<int> ok{0};
  std::vector<std::thread> workers;
  ThreadJoiner joiner(workers);
  workers.reserve(placement.acting_set.size() - 1);
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    workers.emplace_back([&, i] {
      const auto& t = placement.acting_set[i];
      if (t.node_id == cfg_.node_id) {
        std::string err;
        bool done = false;
        if (use_shared) {
          done = local_install(t.aios_path, v, fanout, fanout_len, attrs, err);
        } else if (file_stream) {
          done = local_install_file(t.aios_path, v, abs_body_path, attrs, err);
        } else {
          done = local_install(t.aios_path, v, data, len, attrs, err);
        }
        if (done) {
          ok.fetch_add(1, std::memory_order_relaxed);
        } else {
          AIOS_LOG_WARN("local replica install failed ", t.aios_path, " oid=", v.oid, ": ",
                        err);
        }
        return;
      }
      ObjectRpcResult r;
      if (use_shared) {
        r = object_install_bytes_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                        cfg_.auth_skew_ms, placement.epoch, t.aios_path, v,
                                        attrs, fanout, fanout_len);
      } else if (file_stream) {
        r = object_install_file_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                       cfg_.auth_skew_ms, placement.epoch, t.aios_path, v,
                                       attrs, abs_body_path);
      } else {
        r = object_install_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                  cfg_.auth_skew_ms, placement.epoch, t.aios_path, v, data,
                                  len, attrs);
      }
      if (r.ok) {
        ok.fetch_add(1, std::memory_order_relaxed);
      } else {
        AIOS_LOG_WARN("remote replica install failed ", t.addr, ": ", r.error);
      }
    });
  }
  for (auto& w : workers) w.join();
  return ok.load();
}

int ObjectService::replicate_publish(const Placement& placement, const std::string& oid,
                                     std::uint64_t seq) {
  if (placement.acting_set.size() <= 1) return 0;
  std::optional<UnlockForRpc> unlock;
  if (placement_has_remote(placement, cfg_.node_id)) unlock.emplace(mu_);

  std::atomic<int> ok{0};
  std::vector<std::thread> workers;
  ThreadJoiner joiner(workers);
  workers.reserve(placement.acting_set.size() - 1);
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    workers.emplace_back([&, i] {
      const auto& t = placement.acting_set[i];
      if (t.node_id == cfg_.node_id) {
        std::string err;
        if (local_publish(t.aios_path, oid, seq, err)) {
          ok.fetch_add(1, std::memory_order_relaxed);
        } else {
          AIOS_LOG_WARN("local replica publish failed ", t.aios_path, ": ", err);
        }
        return;
      }
      auto r = object_publish_tip_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                         cfg_.auth_skew_ms, placement.epoch, t.aios_path, oid,
                                         seq);
      if (r.ok) {
        ok.fetch_add(1, std::memory_order_relaxed);
      } else {
        AIOS_LOG_WARN("remote replica publish failed ", t.addr, ": ", r.error);
      }
    });
  }
  for (auto& w : workers) w.join();
  return ok.load();
}

void ObjectService::replicate_abort(const Placement& placement, const std::string& oid,
                                    std::uint64_t seq) {
  if (placement.acting_set.size() <= 1) return;
  UnlockForRpc unlock(mu_);

  std::vector<std::thread> workers;
  ThreadJoiner joiner(workers);
  workers.reserve(placement.acting_set.size() - 1);
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    workers.emplace_back([&, i] {
      const auto& t = placement.acting_set[i];
      if (t.node_id == cfg_.node_id) {
        std::string err;
        local_abort(t.aios_path, oid, seq, err);
        return;
      }
      object_abort_version_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                  cfg_.auth_skew_ms, placement.epoch, t.aios_path, oid, seq);
    });
  }
  for (auto& w : workers) w.join();
}

ApiResult ObjectService::install_prepared(
    ObjectStore* store, const Placement& placement, PreparedVersion& pv,
    const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs,
    const std::string& abs_body_path) {
  std::string body_path = abs_body_path;
  if (body_path.empty() && !pv.inline_body && !pv.is_delete && pv.redirect_oid.empty() &&
      pv.size > 0) {
    std::string err;
    if (auto p = store->fs_body_path(pv.oid, pv.seq, err)) body_path = *p;
  }
  const int total_ok = 1 + replicate_install(placement, pv, data, len, attrs, body_path);
  if (total_ok < quorum_need(placement)) {
    std::string aerr;
    store->abort_version(pv.oid, pv.seq, aerr);
    replicate_abort(placement, pv.oid, pv.seq);
    return fail("quorum_failed", "quorum failed");
  }
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.replicas = total_ok;
  r.placement = placement;
  r.attrs = attrs;
  r.info = ObjectInfo{};
  r.info->oid = pv.oid;
  r.info->seq = pv.seq;
  r.info->size = pv.size;
  r.info->crc32c = pv.crc32c;
  r.info->crc32c_known = true;
  r.info->is_delete = pv.is_delete;
  r.info->redirect_oid = pv.redirect_oid;
  r.info->inline_body = pv.inline_body;
  r.info->fs_path = pv.fs_path;
  if (!pv.redirect_oid.empty()) {
    r.redirect_oid = pv.redirect_oid;
    r.code = "redirect";
  }
  return r;
}

ApiResult ObjectService::commit_prepared(
    ObjectStore* store, const Placement& placement, PreparedVersion& pv,
    const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs,
    const std::string& abs_body_path) {
  auto r = install_prepared(store, placement, pv, data, len, attrs, abs_body_path);
  if (!r.ok) return r;
  std::string err;
  if (!store->publish_tip(pv.oid, pv.seq, err)) {
    store->abort_version(pv.oid, pv.seq, err);
    replicate_abort(placement, pv.oid, pv.seq);
    return fail("store_error", err);
  }
  replicate_publish(placement, pv.oid, pv.seq);
  signal_watch(pv.oid, pv.seq, pv.is_delete ? "del" : "put");

  if (auto st = store->stat(pv.oid, err)) {
    r.info = st;
    if (!st->redirect_oid.empty()) {
      r.redirect_oid = st->redirect_oid;
      r.code = "redirect";
    }
  } else if (!pv.redirect_oid.empty()) {
    r.redirect_oid = pv.redirect_oid;
    r.code = "redirect";
  }
  return r;
}

Frame ObjectService::handle(const Frame& req) {
  // Intentionally no outer mu_ lock: handlers that call api_*/replicate_* rely on
  // UnlockForRpc to release the mutex across peer RPC. An outer lock_guard would
  // keep the mutex held and reintroduce multi-node PUT/publish deadlocks under load.
  try {
    if (!req.raw_empty()) {
      // The HMAC covers only the JSON envelope; clients bind the raw trailer to it
      // with sha256. Required for authenticated (signed) frames; frames handed to
      // handle() directly (in-process callers, tests) carry no sig and may omit it.
      const bool signed_frame = json_has(req.body, "sig");
      if (json_has(req.body, "sha256")) {
        const auto got = sha256_hex(
            std::string(reinterpret_cast<const char*>(req.raw_data()), req.raw_size()));
        if (json_str(req.body, "sha256") != got) {
          return reply_err(cur_epoch(), "bad_request", "raw body sha256 mismatch");
        }
      } else if (signed_frame) {
        return reply_err(cur_epoch(), "bad_request", "raw body requires sha256");
      }
    }
    return dispatch(req);
  } catch (const std::exception& e) {
    AIOS_LOG_WARN("object rpc ", msg_type_name(req.type), " rejected: ", e.what());
    return reply_err(cur_epoch(), "bad_request", std::string("malformed request: ") + e.what());
  }
}

Frame ObjectService::dispatch(const Frame& req) {
  switch (req.type) {
    case MsgType::ObjectPut:
      return handle_put(req);
    case MsgType::ObjectPutRange:
      return handle_put_range(req);
    case MsgType::ObjectGet:
      return handle_get(req.body);
    case MsgType::ObjectDel:
      return handle_del(req.body);
    case MsgType::ObjectStat:
      return handle_stat(req.body);
    case MsgType::ObjectPublishTip:
      return handle_publish_tip(req.body);
    case MsgType::ObjectAbortVersion:
      return handle_abort_version(req.body);
    case MsgType::ObjectListVersions:
      return handle_list_versions(req.body);
    case MsgType::ObjectPurgeVersions:
      return handle_purge_versions(req.body);
    case MsgType::ObjectStageBegin:
      return handle_stage_begin(req.body);
    case MsgType::ObjectStageData:
      return handle_stage_data(req);
    case MsgType::ObjectStageCommit:
      return handle_stage_commit(req.body);
    case MsgType::ObjectList:
      return handle_list(req.body);
    default:
      return reply_err(cur_epoch(), "bad_type", "unsupported object message");
  }
}

static std::unordered_map<std::string, std::string> parse_attrs_json(
    const nlohmann::json& body) {
  std::unordered_map<std::string, std::string> attrs;
  if (body.contains("attrs") && body["attrs"].is_object()) {
    for (auto it = body["attrs"].begin(); it != body["attrs"].end(); ++it) {
      if (it.value().is_string()) attrs[it.key()] = it.value().get<std::string>();
    }
  }
  return attrs;
}

static std::vector<AttrPrecondition> parse_preds_json(const nlohmann::json& body) {
  std::vector<AttrPrecondition> preds;
  if (!body.contains("preconditions") || !body["preconditions"].is_array()) return preds;
  for (const auto& j : body["preconditions"]) {
    if (!j.is_object()) continue;
    const std::string op = json_str(j, "op");
    AttrPrecondition p;
    if (op == "eq") {
      p.kind = AttrPrecondition::Kind::Eq;
      p.key = json_str(j, "key");
      p.value = json_str(j, "value");
    } else if (op == "ne") {
      p.kind = AttrPrecondition::Kind::Ne;
      p.key = json_str(j, "key");
      p.value = json_str(j, "value");
    } else if (op == "absent") {
      p.kind = AttrPrecondition::Kind::Absent;
      p.key = json_str(j, "key");
    } else if (op == "present") {
      p.kind = AttrPrecondition::Kind::Present;
      p.key = json_str(j, "key");
    } else if (op == "must_exist") {
      p.kind = AttrPrecondition::Kind::MustExist;
    } else if (op == "must_not_exist") {
      p.kind = AttrPrecondition::Kind::MustNotExist;
    } else {
      continue;
    }
    preds.push_back(std::move(p));
  }
  return preds;
}

Frame ObjectService::handle_put(const Frame& req) {
  Frame errf;
  const auto& body = req.body;
  if (!epoch_ok(json_u64(body, "epoch"), errf)) return errf;

  const std::string oid = json_str(body, "oid");
  const std::string aios_path = json_str(body, "aios_path");
  const std::string role = json_str(body, "role", "primary");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(cur_epoch(), "bad_request", "oid and aios_path required");
  }

  std::vector<std::uint8_t> data;
  const bool is_delete = json_bool(body, "is_delete", false);
  const std::string redirect_oid = json_str(body, "redirect");
  const bool is_redirect = !redirect_oid.empty();
  if (!is_delete && !is_redirect) {
    if (!req.raw_empty()) {
      data.assign(req.raw_data(), req.raw_data() + req.raw_size());
    } else if (body.contains("data_b64") && body["data_b64"].is_string()) {
      std::string derr;
      if (!base64_decode(body["data_b64"].get<std::string>(), data, derr)) {
        return reply_err(cur_epoch(), "bad_request", "invalid data_b64: " + derr);
      }
    } else if (json_u64(body, "size") != 0) {
      return reply_err(cur_epoch(), "bad_request", "missing put body");
    }
  }

  auto attrs = parse_attrs_json(body);

  std::optional<std::uint32_t> expected_crc;
  if (json_has(body, "crc32c")) {
    expected_crc = json_u32(body, "crc32c");
    if (!is_delete && !is_redirect && crc32c(data.data(), data.size()) != *expected_crc) {
      return reply_err(cur_epoch(), "crc_mismatch", "crc32c mismatch");
    }
  }

  // Replica install of a prepared version (seq present).
  if (role == "replica" && body.contains("seq")) {
    ServiceLock lock(mu_);
    if (!in_acting_set(oid, map_, storage_class_of_target(map_, cfg_.node_id, aios_path), cfg_.node_id, aios_path)) {
      return reply_err(cur_epoch(), "not_replica", "not in acting set for oid");
    }
    PreparedVersion v;
    v.oid = oid;
    v.seq = json_u64(body, "seq");
    v.prev_tip = json_u64(body, "base_seq");
    v.size = (is_delete || is_redirect)
                 ? 0
                 : json_u64(body, "size", data.size());
    if (v.size == 0 && !is_delete && !is_redirect) v.size = data.size();
    v.crc32c = expected_crc.value_or(
        (is_delete || is_redirect) ? crc32c(nullptr, 0) : crc32c(data.data(), data.size()));
    v.inline_body = json_bool(body, "inline_body", false);
    // fs_path is never taken from the wire: the store derives the version's own
    // relpath (a peer-supplied path would be a filesystem traversal vector).
    v.fs_path.clear();
    v.is_delete = is_delete;
    v.redirect_oid = redirect_oid;
    std::string err;
    if (!local_install(aios_path, v, data.data(), data.size(), attrs, err)) {
      return reply_err(cur_epoch(), "store_error", err);
    }
    auto f = reply_ok(cur_epoch());
    f.body["seq"] = v.seq;
    return f;
  }

  if (role == "replica") {
    // Legacy full put (publish immediately) — kept for repair tooling.
    ServiceLock lock(mu_);
    if (!in_acting_set(oid, map_, storage_class_of_target(map_, cfg_.node_id, aios_path), cfg_.node_id, aios_path)) {
      return reply_err(cur_epoch(), "not_replica", "not in acting set for oid");
    }
    auto* store = stores_.get(aios_path);
    if (!store) return reply_err(cur_epoch(), "store_error", "no local store");
    std::string err;
    if (is_redirect) {
      if (!store->put_redirect(oid, redirect_oid, attrs, true, nullptr, err)) {
        return reply_err(cur_epoch(), "store_error", err);
      }
    } else if (!store->put(oid, data.data(), data.size(), attrs, true, expected_crc, err)) {
      return reply_err(cur_epoch(), "store_error", err);
    }
    return reply_ok(cur_epoch());
  }

  // Full primary PUT with layout (replica or EC). Redirects keep the legacy path.
  // api_put locks mu_ itself (and UnlockForRpc must not see an outer hold).
  if (!is_delete && !is_redirect) {
    const LayoutRequest layout_req = layout_request_from_json(body);
    const bool do_publish = json_bool(body, "publish", true);
    if (do_publish) {
      auto r = api_put(oid, data.data(), data.size(), attrs, true, {}, expected_crc, layout_req);
      if (!r.ok) {
        auto f = reply_err(cur_epoch(), r.code, r.error);
        if (r.code == "not_primary") {
          f.body["acting_set"] = nlohmann::json::array();
          for (const auto& t : r.placement.acting_set) {
            f.body["acting_set"].push_back(
                {{"node_id", t.node_id}, {"addr", t.addr}, {"aios_path", t.aios_path}});
          }
        }
        return f;
      }
      auto f = reply_ok(cur_epoch());
      f.body["replicas"] = r.replicas;
      if (r.info) f.body["seq"] = r.info->seq;
      f.body["published"] = true;
      return f;
    }
    // prepare-only (publish=false): replica layout only for now.
    ObjectLayout layout;
    std::string lerr;
    if (!resolve_object_layout(cfg_, oid, layout_req, layout, lerr)) {
      return reply_err(cur_epoch(), "bad_request", lerr);
    }
    if (layout.is_ec()) {
      return reply_err(cur_epoch(), "bad_request",
                       "ec layout not supported for unpublished ObjectPut");
    }
    apply_layout_attrs(attrs, layout);
  }

  ServiceLock lock(mu_);
  const std::string sc_for_primary = storage_class_for_attrs(attrs, cfg_.default_storage_class);
  const auto placement = place(oid, map_, sc_for_primary);
  if (placement.acting_set.empty()) {
    return reply_err(cur_epoch(), "no_targets", "no storage targets in cluster map");
  }

  if (!is_primary_for(oid, map_, sc_for_primary, cfg_.node_id, aios_path)) {
    auto f = reply_err(cur_epoch(), "not_primary", "this node/target is not primary");
    f.body["acting_set"] = nlohmann::json::array();
    for (const auto& t : placement.acting_set) {
      f.body["acting_set"].push_back(
          {{"node_id", t.node_id}, {"addr", t.addr}, {"aios_path", t.aios_path}});
    }
    return f;
  }

  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(cur_epoch(), "store_error", "no local store");
  std::string err;
  const bool do_publish = json_bool(body, "publish", true);
  std::optional<std::string> lock_token;
  if (body.contains("lock_token") && body["lock_token"].is_string()) {
    lock_token = body["lock_token"].get<std::string>();
  }
  if (auto lk = enforce_lock(oid, lock_token); !lk.ok) {
    return reply_err(cur_epoch(), lk.code, lk.error);
  }
  auto preds = parse_preds_json(body);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return reply_err(cur_epoch(), "not_found", err);
  if (pr == PrecondResult::Conflict) return reply_err(cur_epoch(), "precondition_failed", err);

  PreparedVersion pv;
  if (is_redirect) {
    if (!store->prepare_redirect(oid, redirect_oid, attrs, true, pv, err)) {
      return reply_err(cur_epoch(), "store_error", err);
    }
  } else if (!store->prepare_put(oid, data.data(), data.size(), attrs, true, expected_crc, pv,
                                 err)) {
    if (err == "crc32c mismatch") return reply_err(cur_epoch(), "crc_mismatch", err);
    return reply_err(cur_epoch(), "store_error", err);
  }
  ApiResult r = do_publish ? commit_prepared(store, placement, pv, data.data(), data.size(), attrs)
                           : install_prepared(store, placement, pv, data.data(), data.size(),
                                              attrs);
  if (!r.ok) return reply_err(cur_epoch(), r.code, r.error);
  auto f = reply_ok(cur_epoch());
  f.body["replicas"] = r.replicas;
  f.body["seq"] = pv.seq;
  f.body["prev_tip"] = pv.prev_tip;
  f.body["published"] = do_publish;
  if (!pv.redirect_oid.empty()) f.body["redirect"] = pv.redirect_oid;
  return f;
}

Frame ObjectService::handle_put_range(const Frame& req) {
  Frame errf;
  const auto& body = req.body;
  if (!epoch_ok(json_u64(body, "epoch"), errf)) return errf;

  const std::string oid = json_str(body, "oid");
  const std::string aios_path = json_str(body, "aios_path");
  const std::string role = json_str(body, "role", "primary");
  const auto offset = json_u64(body, "offset");
  const bool replace_attrs = json_bool(body, "replace_attrs", false);
  if (oid.empty() || aios_path.empty()) {
    return reply_err(cur_epoch(), "bad_request", "oid and aios_path required");
  }

  const auto* data = req.raw_data();
  const auto len = req.raw_size();
  auto attrs = parse_attrs_json(body);

  if (json_has(body, "range_crc32c")) {
    const auto expect = json_u32(body, "range_crc32c");
    if (!data || crc32c(data, len) != expect) {
      return reply_err(cur_epoch(), "crc_mismatch", "range crc32c mismatch");
    }
  }

  if (role == "replica") {
    // Range replicas are installed as full versions via ObjectPut+seq.
    return reply_err(cur_epoch(), "bad_request",
                     "use ObjectPut with seq to install prepared range versions");
  }

  const LayoutRequest layout_req = layout_request_from_json(body);
  auto r = api_put_range(oid, offset, data, len, attrs, replace_attrs, {}, layout_req);
  if (!r.ok) return reply_err(cur_epoch(), r.code, r.error);
  auto f = reply_ok(cur_epoch());
  f.body["replicas"] = r.replicas;
  if (r.info) f.body["seq"] = r.info->seq;
  return f;
}

Frame ObjectService::handle_get(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(json_u64(body, "epoch"), errf)) return errf;
  const std::string oid = json_str(body, "oid");
  const std::string aios_path = json_str(body, "aios_path");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(cur_epoch(), "bad_request", "oid and aios_path required");
  }
  ServiceLock lock(mu_);
  if (!map_.targets.empty() && !in_acting_set(oid, map_, storage_class_of_target(map_, cfg_.node_id, aios_path), cfg_.node_id, aios_path)) {
    return reply_err(cur_epoch(), "not_replica", "not in acting set for oid");
  }
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(cur_epoch(), "store_error", "no local store");
  std::optional<std::uint64_t> seq;
  if (json_has(body, "seq")) {
    seq = json_u64(body, "seq");
  }
  std::string err;
  auto st = store->stat(oid, seq, err);
  if (!st) return reply_err(cur_epoch(), "not_found", err);
  if (!st->redirect_oid.empty()) {
    auto f = reply_ok(cur_epoch());
    f.body["redirect"] = st->redirect_oid;
    f.body["seq"] = st->seq;
    f.body["code"] = "redirect";
    return f;
  }
  auto f = reply_ok(cur_epoch());
  f.body["seq"] = st->seq;
  f.body["size"] = st->size;
  if (st->crc32c_known) f.body["crc32c"] = st->crc32c;

  auto attach_raw = [&](std::vector<std::uint8_t>&& data, std::uint64_t offset) {
    f.body["offset"] = offset;
    f.body["length"] = data.size();
    f.body["size"] = st->size;
    // The reply HMAC covers only the JSON envelope; bind the trailer to it so
    // the requesting node can tell a swapped body from ours (crc32c alone is
    // not collision-resistant).
    if (!data.empty()) {
      f.body["sha256"] =
          sha256_hex(std::string(reinterpret_cast<const char*>(data.data()), data.size()));
    }
    f.flags |= kFlagRawBody;
    f.raw = std::move(data);
  };

  // Ranged or full raw get (avoids base64 / extra copies on the wire).
  if (body.contains("offset")) {
    const auto offset = json_u64(body, "offset");
    const auto len = json_u64(body, "length");
    if (len == 0 || len > kMaxBodySize) {
      return reply_err(cur_epoch(), "bad_request", "invalid get length");
    }
    std::optional<std::vector<std::uint8_t>> data;
    {
      UnlockForRpc unlock(mu_);
      data = store->get_range(oid, seq, offset, static_cast<std::size_t>(len), err);
    }
    if (!data) {
      if (err == "range unsatisfiable") {
        return reply_err(cur_epoch(), "range_unsatisfiable", err);
      }
      return reply_err(cur_epoch(), "not_found", err);
    }
    attach_raw(std::move(*data), offset);
    return f;
  }

  if (st->size > kMaxBodySize) {
    return reply_err(cur_epoch(), "bad_request", "object exceeds RPC body limit; use ranged get");
  }
  std::optional<std::vector<std::uint8_t>> data;
  {
    UnlockForRpc unlock(mu_);
    data = store->get(oid, seq, err);
  }
  if (!data) return reply_err(cur_epoch(), "not_found", err);
  attach_raw(std::move(*data), 0);
  return f;
}

Frame ObjectService::handle_del(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(json_u64(body, "epoch"), errf)) return errf;
  const std::string oid = json_str(body, "oid");
  const std::string aios_path = json_str(body, "aios_path");
  const std::string role = json_str(body, "role", "primary");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(cur_epoch(), "bad_request", "oid and aios_path required");
  }
  ServiceLock lock(mu_);
  const auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) {
    return reply_err(cur_epoch(), "no_targets", "no storage targets");
  }

  // Replica install of delete-marker version.
  if (role == "replica" && body.contains("seq")) {
    if (!in_acting_set(oid, map_, storage_class_of_target(map_, cfg_.node_id, aios_path), cfg_.node_id, aios_path)) {
      return reply_err(cur_epoch(), "not_replica", "not in acting set");
    }
    PreparedVersion v;
    v.oid = oid;
    v.seq = json_u64(body, "seq");
    v.prev_tip = json_u64(body, "base_seq");
    v.is_delete = true;
    v.crc32c = crc32c(nullptr, 0);
    std::string err;
    if (!local_install(aios_path, v, nullptr, 0, {}, err)) {
      return reply_err(cur_epoch(), "store_error", err);
    }
    return reply_ok(cur_epoch());
  }

  if (role == "replica") {
    if (!in_acting_set(oid, map_, storage_class_of_target(map_, cfg_.node_id, aios_path), cfg_.node_id, aios_path)) {
      return reply_err(cur_epoch(), "not_replica", "not in acting set");
    }
    auto* store = stores_.get(aios_path);
    if (!store) return reply_err(cur_epoch(), "store_error", "no local store");
    std::string err;
    if (!store->del(oid, err) && err != "object not found") {
      return reply_err(cur_epoch(), "store_error", err);
    }
    return reply_ok(cur_epoch());
  }

  if (!is_primary_for(oid, map_, storage_class_of_target(map_, cfg_.node_id, aios_path), cfg_.node_id, aios_path)) {
    return reply_err(cur_epoch(), "not_primary", "not primary");
  }
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(cur_epoch(), "store_error", "no local store");
  std::string err;
  std::optional<std::string> lock_token;
  if (body.contains("lock_token") && body["lock_token"].is_string()) {
    lock_token = body["lock_token"].get<std::string>();
  }
  if (auto lk = enforce_lock(oid, lock_token); !lk.ok) {
    return reply_err(cur_epoch(), lk.code, lk.error);
  }
  auto preds = parse_preds_json(body);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return reply_err(cur_epoch(), "not_found", err);
  if (pr == PrecondResult::Conflict) return reply_err(cur_epoch(), "precondition_failed", err);

  PreparedVersion pv;
  if (!store->prepare_delete(oid, pv, err)) {
    if (err == "object not found") return reply_err(cur_epoch(), "not_found", err);
    return reply_err(cur_epoch(), "store_error", err);
  }
  const bool do_publish = json_bool(body, "publish", true);
  ApiResult r = do_publish ? commit_prepared(store, placement, pv, nullptr, 0, {})
                           : install_prepared(store, placement, pv, nullptr, 0, {});
  if (!r.ok) return reply_err(cur_epoch(), r.code, r.error);
  auto f = reply_ok(cur_epoch());
  f.body["replicas"] = r.replicas;
  f.body["seq"] = pv.seq;
  f.body["prev_tip"] = pv.prev_tip;
  f.body["published"] = do_publish;
  return f;
}

Frame ObjectService::handle_stat(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(json_u64(body, "epoch"), errf)) return errf;
  const std::string oid = json_str(body, "oid");
  const std::string aios_path = json_str(body, "aios_path");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(cur_epoch(), "bad_request", "oid and aios_path required");
  }
  ServiceLock lock(mu_);
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(cur_epoch(), "store_error", "no local store");
  std::optional<std::uint64_t> seq;
  if (json_has(body, "seq")) {
    seq = json_u64(body, "seq");
  }
  std::string err;
  auto info = store->stat(oid, seq, err);
  if (!info && !seq.has_value() && json_bool(body, "include_deleted", false)) {
    // Tip stat() hides delete markers; repair needs the raw tip so it never
    // resurrects a deleted object from a stale live replica.
    std::uint64_t tip = 0;
    std::string terr;
    if (store->tip_seq(oid, tip, terr) && tip > 0) {
      info = store->stat(oid, tip, terr);
      if (info && !info->is_delete) info.reset();
    }
  }
  if (!info) return reply_err(cur_epoch(), "not_found", err);
  auto f = reply_ok(cur_epoch());
  f.body["deleted"] = info->is_delete;
  f.body["size"] = info->size;
  f.body["mtime_ms"] = info->mtime_ms;
  f.body["ctime_ms"] = info->ctime_ms;
  f.body["shard"] = info->shard;
  f.body["inline_body"] = info->inline_body;
  f.body["seq"] = info->seq;
  f.body["is_delete"] = info->is_delete;
  if (!info->redirect_oid.empty()) f.body["redirect"] = info->redirect_oid;
  if (info->crc32c_known) f.body["crc32c"] = info->crc32c;
  nlohmann::json attrs_j = nlohmann::json::object();
  for (const auto& [k, v] : store->list_attrs(oid, err)) attrs_j[k] = v;
  f.body["attrs"] = std::move(attrs_j);
  return f;
}

Frame ObjectService::handle_publish_tip(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(json_u64(body, "epoch"), errf)) return errf;
  const std::string oid = json_str(body, "oid");
  const std::string aios_path = json_str(body, "aios_path");
  const auto seq = json_u64(body, "seq");
  const std::string role = json_str(body, "role", "replica");
  if (oid.empty() || aios_path.empty() || seq == 0) {
    return reply_err(cur_epoch(), "bad_request", "oid, aios_path, seq required");
  }
  // api_publish_version locks and UnlockForRpc across replica publish — no outer hold.
  if (role == "primary") {
    auto r = api_publish_version(oid, seq);
    if (!r.ok) return reply_err(cur_epoch(), r.code, r.error);
    return reply_ok(cur_epoch());
  }
  ServiceLock lock(mu_);
  if (!in_acting_set(oid, map_, storage_class_of_target(map_, cfg_.node_id, aios_path), cfg_.node_id, aios_path)) {
    return reply_err(cur_epoch(), "not_replica", "not in acting set");
  }
  std::string err;
  if (!local_publish(aios_path, oid, seq, err)) {
    return reply_err(cur_epoch(), "store_error", err);
  }
  return reply_ok(cur_epoch());
}

Frame ObjectService::handle_abort_version(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(json_u64(body, "epoch"), errf)) return errf;
  const std::string oid = json_str(body, "oid");
  const std::string aios_path = json_str(body, "aios_path");
  const auto seq = json_u64(body, "seq");
  const std::string role = json_str(body, "role", "replica");
  if (oid.empty() || aios_path.empty() || seq == 0) {
    return reply_err(cur_epoch(), "bad_request", "oid, aios_path, seq required");
  }
  if (role == "primary") {
    auto r = api_abort_prepared(oid, seq);
    if (!r.ok) return reply_err(cur_epoch(), r.code, r.error);
    return reply_ok(cur_epoch());
  }
  ServiceLock lock(mu_);
  if (!in_acting_set(oid, map_, storage_class_of_target(map_, cfg_.node_id, aios_path), cfg_.node_id, aios_path)) {
    return reply_err(cur_epoch(), "not_replica", "not in acting set");
  }
  // An aborted pipelined/staged version leaves its stage session (fd + tmp file)
  // behind otherwise.
  close_stage_session(stage_key(aios_path, oid, seq), /*remove_file=*/true);
  std::string err;
  if (!local_abort(aios_path, oid, seq, err)) {
    return reply_err(cur_epoch(), "store_error", err);
  }
  return reply_ok(cur_epoch());
}

Frame ObjectService::handle_list_versions(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(json_u64(body, "epoch"), errf)) return errf;
  const std::string oid = json_str(body, "oid");
  const std::string aios_path = json_str(body, "aios_path");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(cur_epoch(), "bad_request", "oid and aios_path required");
  }
  ServiceLock lock(mu_);
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(cur_epoch(), "store_error", "no local store");
  std::string err;
  auto vers = store->list_versions(oid, err);
  auto f = reply_ok(cur_epoch());
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& v : vers) {
    nlohmann::json j = {{"seq", v.seq},
                        {"size", v.size},
                        {"ctime_ms", v.ctime_ms},
                        {"is_delete", v.is_delete},
                        {"inline_body", v.inline_body}};
    if (v.crc32c_known) j["crc32c"] = v.crc32c;
    if (!v.redirect_oid.empty()) j["redirect"] = v.redirect_oid;
    arr.push_back(std::move(j));
  }
  f.body["versions"] = std::move(arr);
  return f;
}

Frame ObjectService::handle_purge_versions(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(json_u64(body, "epoch"), errf)) return errf;
  const std::string oid = json_str(body, "oid");
  const std::string aios_path = json_str(body, "aios_path");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(cur_epoch(), "bad_request", "oid and aios_path required");
  }
  ServiceLock lock(mu_);
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(cur_epoch(), "store_error", "no local store");
  std::string err;
  if (json_has(body, "seq")) {
    const auto seq = json_u64(body, "seq");
    const bool allow_tip = json_bool(body, "allow_tip", false);
    if (!store->purge_version(oid, seq, allow_tip, err)) {
      return reply_err(cur_epoch(), "store_error", err);
    }
  } else {
    int keep = json_int(body, "keep", store->options().max_versions);
    if (!store->trim_versions(oid, keep, err)) {
      return reply_err(cur_epoch(), "store_error", err);
    }
  }
  return reply_ok(cur_epoch());
}

ApiResult ObjectService::commit_ec_put(
    ObjectStore* store, const Placement& placement, const std::string& oid,
    const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    std::optional<std::uint32_t> expected_crc32c, const ObjectLayout& layout) {
  std::string err;
  auto codec = make_erasure_codec(layout.ec_k, layout.ec_m, layout.ec_codec, err);
  if (!codec) return fail("bad_request", err);
  if (static_cast<int>(placement.acting_set.size()) < codec->shard_count()) {
    return fail("no_targets", "acting set smaller than k+m");
  }
  const std::uint32_t full_crc = crc32c(data, len);
  if (expected_crc32c && *expected_crc32c != full_crc) {
    return fail("crc_mismatch", "crc32c mismatch");
  }

  std::vector<std::vector<std::uint8_t>> shards;
  if (!codec->encode(std::span<const std::uint8_t>(data, len), shards, err)) {
    return fail("store_error", err);
  }

  std::unordered_map<std::string, std::string> base = attrs;
  apply_layout_attrs(base, layout);
  set_ec_attrs(base, codec->k(), codec->m(), 0, codec->name(), len, full_crc);

  auto shard_attrs = [&](int i) {
    auto a = base;
    a[kEcAttrI] = std::to_string(i);
    return a;
  };

  PreparedVersion pv;
  const auto a0 = shard_attrs(0);
  if (!store->prepare_put(oid, shards[0].data(), shards[0].size(), a0, replace_attrs,
                          std::nullopt, pv, err)) {
    return fail("store_error", err);
  }

  int total_ok = 1;
  for (int i = 1; i < codec->shard_count(); ++i) {
    const auto& t = placement.acting_set[static_cast<std::size_t>(i)];
    PreparedVersion sv = pv;
    sv.size = shards[static_cast<std::size_t>(i)].size();
    sv.crc32c = crc32c(shards[static_cast<std::size_t>(i)].data(),
                       shards[static_cast<std::size_t>(i)].size());
    sv.inline_body = sv.size <= 64 * 1024;
    sv.fs_path.clear();
    const auto ai = shard_attrs(i);
    const auto* sd = shards[static_cast<std::size_t>(i)].data();
    const auto sl = shards[static_cast<std::size_t>(i)].size();
    bool done = false;
    if (t.node_id == cfg_.node_id) {
      done = local_install(t.aios_path, sv, sd, sl, ai, err);
    } else {
      ObjectRpcResult r;
      {
        UnlockForRpc unlock(mu_);
        r = object_install_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                  cfg_.auth_skew_ms, placement.epoch, t.aios_path, sv, sd, sl,
                                  ai);
      }
      done = r.ok;
      if (!done) err = r.error;
    }
    if (done) ++total_ok;
    else
      AIOS_LOG_WARN("ec shard install failed i=", i, " ", err);
  }

  // An EC object needs any k of its k+m shards to decode, so k is a hard floor
  // regardless of the configured replication write quorum.
  const int ec_need = std::max(codec->k(), quorum_need(placement));
  if (total_ok < ec_need) {
    store->abort_version(oid, pv.seq, err);
    replicate_abort(placement, oid, pv.seq);
    return fail("quorum_failed", "ec shard quorum failed: installed " +
                                     std::to_string(total_ok) + " of " +
                                     std::to_string(codec->shard_count()) + ", need " +
                                     std::to_string(ec_need));
  }

  if (!store->publish_tip(oid, pv.seq, err)) {
    store->abort_version(oid, pv.seq, err);
    replicate_abort(placement, oid, pv.seq);
    return fail("store_error", err);
  }
  replicate_publish(placement, oid, pv.seq);
  signal_watch(oid, pv.seq, "put");

  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.replicas = total_ok;
  r.placement = placement;
  r.attrs = a0;
  r.info = ObjectInfo{};
  r.info->oid = oid;
  r.info->seq = pv.seq;
  r.info->size = len;
  r.info->crc32c = full_crc;
  r.info->crc32c_known = true;
  return r;
}

ApiResult ObjectService::reconstruct_ec_object(
    const Placement& placement, const std::string& oid, std::optional<std::uint64_t> seq,
    const std::unordered_map<std::string, std::string>& tip_attrs,
    std::optional<std::uint64_t> range_off, std::optional<std::uint64_t> range_end) {
  auto meta = parse_ec_attrs(tip_attrs);
  if (!meta) return fail("store_error", "missing ec attrs");
  std::string err;
  auto codec = make_erasure_codec(meta->k, meta->m, meta->codec, err);
  if (!codec || codec->m() != meta->m) return fail("store_error", "unsupported ec profile");
  if (static_cast<int>(placement.acting_set.size()) < codec->shard_count()) {
    return fail("no_targets", "acting set smaller than k+m");
  }

  const auto shard_count = static_cast<std::size_t>(codec->shard_count());
  const auto k = static_cast<std::size_t>(codec->k());
  const auto full_size = static_cast<std::size_t>(meta->full_size);
  const std::size_t shard_len =
      full_size == 0 ? 0 : (full_size + k - 1) / k;

  auto shard_index_for = [&](const StorageTarget& t, std::size_t fallback) -> std::size_t {
    std::unordered_map<std::string, std::string> a;
    if (t.node_id == cfg_.node_id) {
      auto* s = stores_.get(t.aios_path);
      if (!s) return fallback;
      std::string e;
      a = s->list_attrs(oid, e);
    } else {
      auto st = object_stat_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                   cfg_.auth_skew_ms, cur_epoch(), t.aios_path, oid);
      if (!st.ok || !st.body.contains("attrs") || !st.body["attrs"].is_object()) return fallback;
      for (auto it = st.body["attrs"].begin(); it != st.body["attrs"].end(); ++it) {
        if (it.value().is_string()) a[it.key()] = it.value().get<std::string>();
      }
    }
    const auto sm = parse_ec_attrs(a);
    if (!sm || sm->shard_i < 0) return fallback;
    return static_cast<std::size_t>(sm->shard_i);
  };

  std::vector<std::optional<std::vector<std::uint8_t>>> shards(shard_count);
  std::mutex shard_mu;
  std::atomic<int> got{0};
  std::vector<std::thread> workers;
  ThreadJoiner joiner(workers);
  workers.reserve(placement.acting_set.size());
  for (std::size_t ti = 0; ti < placement.acting_set.size(); ++ti) {
    workers.emplace_back([&, ti] {
      const auto& t = placement.acting_set[ti];
      const auto i = shard_index_for(t, ti);
      if (i >= shard_count) return;
      std::vector<std::uint8_t> data;
      std::string e;
      if (t.node_id == cfg_.node_id) {
        auto* s = stores_.get(t.aios_path);
        if (!s) return;
        auto got_data = s->get(oid, seq, e);
        if (!got_data) return;
        data = std::move(*got_data);
      } else if (seq.has_value()) {
        auto st = object_stat_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                     cfg_.auth_skew_ms, cur_epoch(), t.aios_path, oid);
        if (!st.ok) return;
        auto r = object_get_range_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                         cfg_.auth_skew_ms, cur_epoch(), t.aios_path, oid, 0,
                                         static_cast<std::size_t>(st.size), seq);
        if (r.ok && !r.raw.empty()) data = std::move(r.raw);
        else if (r.ok && r.data) data = std::move(*r.data);
        else return;
      } else {
        auto r = object_get_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                   cfg_.auth_skew_ms, cur_epoch(), t.aios_path, oid);
        if (!r.ok || !r.data) return;
        data = std::move(*r.data);
      }
      std::lock_guard lock(shard_mu);
      if (shards[i]) return;
      shards[i] = std::move(data);
      got.fetch_add(1, std::memory_order_relaxed);
    });
  }
  for (auto& w : workers) w.join();
  if (got.load() < meta->k) return fail("quorum_failed", "not enough ec shards to reconstruct");

  const bool ranged = range_off.has_value() || range_end.has_value();
  const std::uint64_t lo = range_off.value_or(0);
  if (ranged && lo >= meta->full_size) {
    return fail("range_unsatisfiable", "range unsatisfiable");
  }
  std::uint64_t hi = range_end.value_or(full_size == 0 ? 0 : full_size - 1);
  if (full_size == 0) {
    hi = 0;
  } else if (hi >= full_size) {
    hi = full_size - 1;
  }
  if (full_size > 0 && hi < lo) return fail("range_unsatisfiable", "range unsatisfiable");

  auto slice_from_data_shards = [&](std::vector<std::uint8_t>& out) -> bool {
    if (full_size == 0) {
      out.clear();
      return true;
    }
    if (shard_len == 0) return false;
    const auto i0 = static_cast<std::size_t>(lo / shard_len);
    const auto i1 = static_cast<std::size_t>(hi / shard_len);
    if (i1 >= k) return false;
    for (auto i = i0; i <= i1; ++i) {
      if (!shards[i]) return false;
    }
    out.resize(static_cast<std::size_t>(hi - lo + 1));
    std::size_t w = 0;
    for (auto i = i0; i <= i1; ++i) {
      const std::uint64_t shard_base = static_cast<std::uint64_t>(i) * shard_len;
      const std::uint64_t copy_lo = std::max(lo, shard_base);
      const std::uint64_t copy_hi = std::min(hi, shard_base + shard_len - 1);
      const auto src = static_cast<std::size_t>(copy_lo - shard_base);
      const auto n = static_cast<std::size_t>(copy_hi - copy_lo + 1);
      if (src + n > shards[i]->size()) return false;
      std::memcpy(out.data() + w, shards[i]->data() + src, n);
      w += n;
    }
    return w == out.size();
  };

  std::vector<std::uint8_t> out;
  if (!slice_from_data_shards(out)) {
    std::vector<std::uint8_t> full;
    if (!codec->decode(shards, full_size, full, err)) {
      return fail("store_error", err);
    }
    if (meta->full_crc_known && crc32c(full.data(), full.size()) != meta->full_crc) {
      return fail("crc_mismatch", "reconstructed object crc mismatch");
    }
    if (full_size == 0) {
      out.clear();
    } else {
      out.assign(full.begin() + static_cast<std::ptrdiff_t>(lo),
                 full.begin() + static_cast<std::ptrdiff_t>(hi + 1));
    }
  } else if (out.size() == full_size && meta->full_crc_known &&
             crc32c(out.data(), out.size()) != meta->full_crc) {
    return fail("crc_mismatch", "reconstructed object crc mismatch");
  }

  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  r.attrs = tip_attrs;
  r.data = std::move(out);
  r.info = ObjectInfo{};
  r.info->oid = oid;
  r.info->size = meta->full_size;
  r.info->crc32c = meta->full_crc;
  r.info->crc32c_known = meta->full_crc_known;
  if (seq.has_value()) r.info->seq = *seq;
  return r;
}

ApiResult ObjectService::api_put(const std::string& oid, const std::uint8_t* data,
                                std::size_t len,
                                const std::unordered_map<std::string, std::string>& attrs,
                                bool replace_attrs,
                                const std::vector<AttrPrecondition>& preds,
                                std::optional<std::uint32_t> expected_crc32c,
                                const LayoutRequest& layout_req,
                                const std::optional<std::string>& lock_token) {
  gc_client_writes();
  MutatingOid mutating(mu_, mutating_mu_, mutating_cv_, mutating_oids_, oid);
  ServiceLock lock(mu_);
  ObjectLayout layout;
  std::string err;
  if (!resolve_object_layout(cfg_, oid, layout_req, layout, err)) {
    return fail("bad_request", err);
  }
  auto placement = place(oid, map_, layout.n, layout.storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  if (auto lk = enforce_lock(oid, lock_token); !lk.ok) return lk;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  {
    auto tip_attrs = store->list_attrs(oid, err);
    if (attrs_are_frozen(tip_attrs)) {
      return fail("frozen", "object is archived/frozen; recall before mutate");
    }
  }
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

  PutPayload payload;
  if (!prepare_put_payload(cfg_, ops_, data, len, expected_crc32c, payload, err)) {
    if (err == "crc32c mismatch") return fail("crc_mismatch", err);
    return fail("store_error", err);
  }
  auto put_attrs = attrs;
  if (payload.compressed) {
    set_compression_attrs(put_attrs, kCompAlgoZstd, payload.logical_size, payload.logical_crc);
  }

  if (layout.is_ec()) {
    auto r = commit_ec_put(store, placement, oid, payload.data, payload.len, put_attrs,
                           replace_attrs, std::nullopt, layout);
    if (r.ok) {
      ops_.note_put(payload.logical_size);
      if (payload.compressed) ops_.note_compress(payload.logical_size, payload.len);
      if (r.info) {
        r.info->size = payload.logical_size;
        r.info->crc32c = payload.logical_crc;
        r.info->crc32c_known = true;
      }
    }
    return r;
  }

  apply_layout_attrs(put_attrs, layout);
  PreparedVersion pv;
  if (!store->prepare_put(oid, payload.data, payload.len, put_attrs, replace_attrs, std::nullopt,
                          pv, err)) {
    if (err == "crc32c mismatch") return fail("crc_mismatch", err);
    return fail("store_error", err);
  }
  auto r = commit_prepared(store, placement, pv, payload.data, payload.len, put_attrs);
  if (r.ok) {
    ops_.note_put(payload.logical_size);
    if (payload.compressed) ops_.note_compress(payload.logical_size, payload.len);
    if (r.info) {
      r.info->size = payload.logical_size;
      r.info->crc32c = payload.logical_crc;
      r.info->crc32c_known = true;
    }
  }
  return r;
}

std::string ObjectService::stage_key(const std::string& aios_path, const std::string& oid,
                                     std::uint64_t seq) {
  return aios_path + '\n' + oid + '\n' + std::to_string(seq);
}

void ObjectService::close_stage_session(const std::string& key, bool remove_file) {
  auto it = stages_.find(key);
  if (it == stages_.end()) return;
  auto sess = std::move(it->second);
  stages_.erase(it);
  if (!sess) return;
  // Waits for a StageData chunk that is mid-pwrite on this session.
  std::lock_guard slock(sess->mu);
  sess->closed = true;
  if (sess->fd >= 0) {
    ::close(sess->fd);
    sess->fd = -1;
  }
  if (remove_file && !sess->path.empty()) {
    std::error_code ec;
    std::filesystem::remove(sess->path, ec);
  }
}

namespace {
// Sessions the coordinator never committed or aborted (crash, dropped connection).
constexpr std::int64_t kStageIdleMaxMs = 10 * 60 * 1000;
// LIST page cap; limit=0 used to mean unbounded.
constexpr std::size_t kListLimitMax = 10000;
}  // namespace

void ObjectService::gc_stage_sessions() {
  const auto now = now_ms();
  std::vector<std::string> stale;
  for (const auto& [key, sess] : stages_) {
    if (!sess) {
      stale.push_back(key);
      continue;
    }
    std::int64_t last = 0;
    {
      std::lock_guard slock(sess->mu);
      last = sess->last_used_ms;
    }
    if (now - last > kStageIdleMaxMs) stale.push_back(key);
  }
  for (const auto& key : stale) {
    AIOS_LOG_WARN("closing idle stage session ", key.substr(0, key.find('\n')));
    close_stage_session(key, /*remove_file=*/true);
  }
}

ApiResult ObjectService::api_begin_put_staging(const std::string& oid,
                                              const LayoutRequest& layout_req,
                                              std::string& staging_abs_out) {
  staging_abs_out.clear();
  ServiceLock lock(mu_);
  ObjectLayout layout;
  std::string err;
  if (!resolve_object_layout(cfg_, oid, layout_req, layout, err)) {
    return fail("bad_request", err);
  }
  auto placement = place(oid, map_, layout.n, layout.storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  if (!store->create_staging_file(oid, staging_abs_out, err)) {
    return fail("store_error", err);
  }
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  return r;
}

void ObjectService::destroy_pipeline(const std::shared_ptr<PutPipeline>& pl, bool abort_peers) {
  if (!pl) return;
  std::lock_guard plock(pl->mu);
  if (pl->fd >= 0) {
    ::close(pl->fd);
    pl->fd = -1;
  }
  if (!pl->staging_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(pl->staging_path, ec);
    pl->staging_path.clear();
  }
  for (auto& lp : pl->local_peers) {
    if (lp.fd >= 0) {
      ::close(lp.fd);
      lp.fd = -1;
    }
    if (!lp.stage_path.empty()) {
      std::error_code ec;
      std::filesystem::remove(lp.stage_path, ec);
      lp.stage_path.clear();
    }
    if (abort_peers && pl->seq > 0) {
      std::string err;
      local_abort(lp.aios_path, pl->oid, pl->seq, err);
    }
  }
  pl->local_peers.clear();
  for (auto& rs : pl->remote_peers) {
    if (abort_peers) rs.abort();
  }
  pl->remote_peers.clear();
}

ApiResult ObjectService::api_begin_put_pipeline(const std::string& oid,
                                               const LayoutRequest& layout_req,
                                               std::uint64_t expected_size,
                                               std::string& staging_abs_out) {
  staging_abs_out.clear();
  ObjectLayout layout;
  std::string err;
  if (!resolve_object_layout(cfg_, oid, layout_req, layout, err)) {
    return fail("bad_request", err);
  }
  if (layout.is_ec()) {
    return fail("not_supported", "pipeline put not supported for erasure-coded objects");
  }
  if (cfg_.compression == kCompAlgoZstd && zstd_available()) {
    return fail("not_supported", "pipeline put not supported with compression");
  }

  gc_client_writes();
  {
    // Checked before taking the oid guard: an open pipeline holds it, so a second
    // begin would otherwise block until that pipeline finishes instead of failing.
    ServiceLock lock(mu_);
    if (pipelines_.count(oid) || client_writes_.count(oid)) {
      return fail("conflict", "pipelined put already in progress for oid");
    }
  }
  auto pl = std::make_shared<PutPipeline>();
  // The pipeline peeks its seq at begin and installs it much later; holding the
  // per-oid mutation guard until finish/abort keeps other writers from taking the
  // same seq (and abort from tearing down a version it did not install).
  pl->oid_guard = std::make_shared<MutatingOid>(mu_, mutating_mu_, mutating_cv_, mutating_oids_, oid);
  {
    ServiceLock lock(mu_);
    if (pipelines_.count(oid) || client_writes_.count(oid)) {
      return fail("conflict", "pipelined put already in progress for oid");
    }
    auto placement = place(oid, map_, layout.n, layout.storage_class);
    if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
    if (placement.acting_set[0].node_id != cfg_.node_id) {
      auto r = fail("not_primary", "this node is not primary for oid");
      r.placement = placement;
      return r;
    }
    if (auto g = primary_gate(oid, placement); !g.ok) return g;
    auto* store = primary_store(placement, err);
    if (!store) return fail("store_error", err);
    {
      auto tip_attrs = store->list_attrs(oid, err);
      if (attrs_are_frozen(tip_attrs)) {
        return fail("frozen", "object is archived/frozen; recall before mutate");
      }
    }
    if (!store->peek_next_seq(oid, pl->seq, pl->prev_tip, err)) {
      return fail("store_error", err);
    }
    if (!store->create_staging_file(oid, pl->staging_path, err)) {
      return fail("store_error", err);
    }
    pl->fd = ::open(pl->staging_path.c_str(), O_RDWR | O_TRUNC, 0644);
    if (pl->fd < 0) {
      std::error_code ec;
      std::filesystem::remove(pl->staging_path, ec);
      return fail("store_error", std::string("open staging: ") + std::strerror(errno));
    }
    pl->oid = oid;
    pl->expected_size = expected_size;
    pl->placement = placement;
    pl->layout = layout;
    pl->meta.oid = oid;
    pl->meta.seq = pl->seq;
    pl->meta.prev_tip = pl->prev_tip;
    pl->meta.size = expected_size;
    pl->meta.crc32c = 0;
    pl->meta.inline_body = false;
    pl->meta.is_delete = false;

    // Open local replica staging sessions before advertising the pipeline.
    for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
      const auto& t = placement.acting_set[i];
      if (t.node_id != cfg_.node_id) continue;
      PutPipeline::LocalPeer lp;
      lp.aios_path = t.aios_path;
      if (!stores_.get(t.aios_path) ||
          !stores_.get(t.aios_path)->stage_path_for(oid, pl->seq, lp.stage_path, err) ||
          !stores_.get(t.aios_path)->stage_truncate(lp.stage_path, err)) {
        destroy_pipeline(pl, false);
        return fail("store_error", err.empty() ? "local replica stage begin failed" : err);
      }
      lp.fd = ::open(lp.stage_path.c_str(), O_RDWR, 0644);
      if (lp.fd < 0) {
        destroy_pipeline(pl, false);
        return fail("store_error", std::string("open local stage: ") + std::strerror(errno));
      }
      pl->local_peers.push_back(std::move(lp));
    }
    pipelines_[oid] = pl;
  }

  // Remote StageBegin off the service lock (avoids gossip/RPC deadlock).
  std::vector<RemoteStageSession> remotes;
  remotes.reserve(pl->placement.acting_set.size());
  for (std::size_t i = 1; i < pl->placement.acting_set.size(); ++i) {
    const auto& t = pl->placement.acting_set[i];
    if (t.node_id == cfg_.node_id) continue;
    RemoteStageSession sess;
    if (!sess.begin(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key, cfg_.auth_skew_ms,
                    pl->placement.epoch, t.aios_path, pl->meta)) {
      {
        ServiceLock lock(mu_);
        pipelines_.erase(oid);
      }
      destroy_pipeline(pl, true);
      for (auto& r : remotes) r.abort();
      return fail("quorum_failed", "stage begin failed: " + sess.error());
    }
    remotes.push_back(std::move(sess));
  }
  {
    std::lock_guard plock(pl->mu);
    pl->remote_peers = std::move(remotes);
  }
  staging_abs_out = pl->staging_path;
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = pl->placement;
  return r;
}

ApiResult ObjectService::api_put_pipeline_data(const std::string& oid, std::uint64_t offset,
                                              const std::uint8_t* data, std::size_t len,
                                              PipelineDataKind kind) {
  std::shared_ptr<PutPipeline> pl;
  {
    ServiceLock lock(mu_);
    auto it = pipelines_.find(oid);
    if (it == pipelines_.end()) return fail("not_found", "no pipelined put for oid");
    pl = it->second;
  }
  if (len > 0 && data == nullptr) return fail("bad_request", "null pipeline chunk");

  if (kind == PipelineDataKind::Local || kind == PipelineDataKind::All) {
    std::lock_guard plock(pl->mu);
    if (pl->fd < 0) return fail("store_error", "pipeline staging closed");
    if (offset != pl->bytes) {
      return fail("bad_request", "pipeline offset mismatch");
    }
    if (pl->expected_size > 0 && offset + len > pl->expected_size) {
      return fail("bad_request", "pipeline exceeds expected size");
    }

    std::size_t done = 0;
    while (done < len) {
      const ssize_t n =
          ::pwrite(pl->fd, data + done, len - done, static_cast<off_t>(offset + done));
      if (n < 0) {
        return fail("store_error", std::string("pwrite: ") + std::strerror(errno));
      }
      if (n == 0) return fail("store_error", "pwrite short write");
      done += static_cast<std::size_t>(n);
    }
    for (auto& lp : pl->local_peers) {
      done = 0;
      while (done < len) {
        const ssize_t n =
            ::pwrite(lp.fd, data + done, len - done, static_cast<off_t>(offset + done));
        if (n < 0) {
          return fail("store_error", std::string("local peer pwrite: ") + std::strerror(errno));
        }
        if (n == 0) return fail("store_error", "local peer pwrite short write");
        done += static_cast<std::size_t>(n);
      }
      lp.crc = crc32c_update(lp.crc, data, len);
      lp.bytes += len;
    }
    pl->crc = crc32c_update(pl->crc, data, len);
    pl->bytes += len;
  }

  if (kind == PipelineDataKind::Remote || kind == PipelineDataKind::All) {
    // Fan-out without pl->mu so the next Local pwrite can run while peers ingest.
    std::size_t npeers = 0;
    {
      std::lock_guard plock(pl->mu);
      npeers = pl->remote_peers.size();
    }
    std::atomic<int> fail_count{0};
    std::string peer_err;
    std::mutex err_mu;
    std::vector<std::thread> workers;
    ThreadJoiner joiner(workers);
    workers.reserve(npeers);
    for (std::size_t i = 0; i < npeers; ++i) {
      workers.emplace_back([&, i] {
        if (!pl->remote_peers[i].data(offset, data, len)) {
          fail_count.fetch_add(1, std::memory_order_relaxed);
          std::lock_guard elock(err_mu);
          if (peer_err.empty()) peer_err = pl->remote_peers[i].error();
        }
      });
    }
    for (auto& w : workers) w.join();
    if (fail_count.load() > 0) {
      return fail("quorum_failed", "stage data failed: " + peer_err);
    }
  }

  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = pl->placement;
  return r;
}

ApiResult ObjectService::api_put_pipeline_abort(const std::string& oid) {
  std::shared_ptr<PutPipeline> pl;
  {
    ServiceLock lock(mu_);
    auto it = pipelines_.find(oid);
    if (it == pipelines_.end()) {
      ApiResult r;
      r.ok = true;
      r.epoch = cur_epoch();
      return r;
    }
    pl = it->second;
    pipelines_.erase(it);
  }
  {
    // Callers (pipeline_finish failure paths) may hold mu_; remote abort must not.
    UnlockForRpc unlock(mu_);
    destroy_pipeline(pl, true);
  }
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  return r;
}

ApiResult ObjectService::api_put_pipeline_finish(
    const std::string& oid, const std::unordered_map<std::string, std::string>& attrs,
    bool replace_attrs, const std::vector<AttrPrecondition>& preds,
    std::optional<std::uint32_t> expected_crc32c, const std::optional<std::string>& lock_token) {
  std::shared_ptr<PutPipeline> pl;
  {
    ServiceLock lock(mu_);
    auto it = pipelines_.find(oid);
    if (it == pipelines_.end()) return fail("not_found", "no pipelined put for oid");
    pl = it->second;
  }

  Placement placement;
  ObjectLayout layout;
  std::uint64_t seq = 0;
  std::uint64_t prev_tip = 0;
  std::uint64_t size = 0;
  std::uint32_t crc = 0;
  std::string staging;
  {
    std::unique_lock plock(pl->mu);
    if (pl->expected_size > 0 && pl->bytes != pl->expected_size) {
      plock.unlock();
      api_put_pipeline_abort(oid);
      return fail("bad_request", "pipeline size mismatch");
    }
    if (expected_crc32c && *expected_crc32c != pl->crc) {
      plock.unlock();
      api_put_pipeline_abort(oid);
      return fail("crc_mismatch", "crc32c mismatch");
    }
    placement = pl->placement;
    layout = pl->layout;
    seq = pl->seq;
    prev_tip = pl->prev_tip;
    size = pl->bytes;
    crc = pl->crc;
    staging = pl->staging_path;
    if (pl->fd >= 0) {
      if (cfg_.data_fsync) ::fsync(pl->fd);
      ::close(pl->fd);
      pl->fd = -1;
    }
    pl->staging_path.clear();
  }

  // Never take mu_ while holding pl->mu (abort/finish lock inversion).
  std::string err;
  ObjectStore* store = nullptr;
  {
    ServiceLock lock(mu_);
    if (auto lk = enforce_lock(oid, lock_token); !lk.ok) {
      api_put_pipeline_abort(oid);
      return lk;
    }
    store = primary_store(placement, err);
    if (!store) {
      api_put_pipeline_abort(oid);
      return fail("store_error", err);
    }
    auto pr = check_preds_on(store, oid, preds, err);
    if (pr == PrecondResult::NotFound) {
      api_put_pipeline_abort(oid);
      return fail("not_found", err);
    }
    if (pr == PrecondResult::Conflict) {
      api_put_pipeline_abort(oid);
      return fail("precondition_failed", err);
    }
  }

  auto put_attrs = attrs;
  apply_layout_attrs(put_attrs, layout);
  PreparedVersion pv;
  {
    ServiceLock lock(mu_);
    store = primary_store(placement, err);
    if (!store) {
      api_put_pipeline_abort(oid);
      return fail("store_error", err);
    }
    if (!store->prepare_put_file_at_seq(oid, seq, prev_tip, staging, size, crc, put_attrs,
                                        replace_attrs, expected_crc32c, pv, err)) {
      api_put_pipeline_abort(oid);
      if (err == "crc32c mismatch") return fail("crc_mismatch", err);
      return fail("store_error", err);
    }
  }

  // Commit replicas that already have the body staged.
  std::atomic<int> peer_ok{0};
  std::string peer_err;
  std::mutex err_mu;
  {
    std::lock_guard plock(pl->mu);
    std::vector<std::thread> workers;
    ThreadJoiner joiner(workers);
    workers.reserve(pl->local_peers.size() + pl->remote_peers.size());
    for (std::size_t i = 0; i < pl->local_peers.size(); ++i) {
      workers.emplace_back([&, i] {
        auto& lp = pl->local_peers[i];
        if (lp.bytes != size || lp.crc != crc) {
          std::lock_guard elock(err_mu);
          if (peer_err.empty()) peer_err = "local peer stage mismatch";
          return;
        }
        if (lp.fd >= 0) {
          if (cfg_.data_fsync) ::fsync(lp.fd);
          ::close(lp.fd);
          lp.fd = -1;
        }
        std::string lerr;
        auto* rs = stores_.get(lp.aios_path);
        if (!rs) {
          std::lock_guard elock(err_mu);
          if (peer_err.empty()) peer_err = "no local replica store";
          return;
        }
        PreparedVersion rv = pv;
        std::string rel;
        if (!rs->place_staging_as_version(oid, seq, lp.stage_path, rel, lerr)) {
          std::lock_guard elock(err_mu);
          if (peer_err.empty()) peer_err = lerr;
          return;
        }
        lp.stage_path.clear();
        rv.fs_path = rel;
        rv.crc_verified = true;
        if (!rs->install_version(rv, nullptr, 0, put_attrs, lerr)) {
          std::lock_guard elock(err_mu);
          if (peer_err.empty()) peer_err = lerr;
          return;
        }
        peer_ok.fetch_add(1, std::memory_order_relaxed);
      });
    }
    for (std::size_t i = 0; i < pl->remote_peers.size(); ++i) {
      workers.emplace_back([&, i] {
        if (!pl->remote_peers[i].commit(pv, put_attrs)) {
          std::lock_guard elock(err_mu);
          if (peer_err.empty()) peer_err = pl->remote_peers[i].error();
          return;
        }
        peer_ok.fetch_add(1, std::memory_order_relaxed);
      });
    }
    for (auto& w : workers) w.join();
  }

  const int total_ok = 1 + peer_ok.load();
  if (total_ok < quorum_need(placement)) {
    {
      ServiceLock lock(mu_);
      store->abort_version(oid, seq, err);
      pipelines_.erase(oid);
      // replicate_* use UnlockForRpc and require mu_ to be held by the caller.
      replicate_abort(placement, oid, seq);
    }
    destroy_pipeline(pl, true);
    return fail("quorum_failed",
                peer_err.empty() ? "quorum failed" : ("quorum failed: " + peer_err));
  }

  {
    std::lock_guard plock(pl->mu);
    pl->local_peers.clear();
    pl->remote_peers.clear();
  }
  destroy_pipeline(pl, false);

  ApiResult r;
  {
    ServiceLock lock(mu_);
    if (!store->publish_tip(oid, seq, err)) {
      store->abort_version(oid, seq, err);
      pipelines_.erase(oid);
      replicate_abort(placement, oid, seq);
      return fail("store_error", err);
    }
    pipelines_.erase(oid);
    // replicate_* use UnlockForRpc and require mu_ to be held by the caller.
    replicate_publish(placement, oid, seq);
    signal_watch(oid, seq, "put");
    ops_.note_put(size);

    r.ok = true;
    r.epoch = cur_epoch();
    r.replicas = total_ok;
    r.placement = placement;
    r.attrs = put_attrs;
    r.info = ObjectInfo{};
    r.info->oid = oid;
    r.info->seq = seq;
    r.info->size = size;
    r.info->crc32c = crc;
    r.info->crc32c_known = true;
    r.info->inline_body = false;
    r.info->fs_path = pv.fs_path;
  }
  pl->oid_guard.reset();
  return r;
}

ApiResult ObjectService::api_put_file(
    const std::string& oid, const std::string& staging_abs_path, std::uint64_t size,
    std::uint32_t crc32c_val, const std::unordered_map<std::string, std::string>& attrs,
    bool replace_attrs, const std::vector<AttrPrecondition>& preds,
    std::optional<std::uint32_t> expected_crc32c, const LayoutRequest& layout_req,
    const std::optional<std::string>& lock_token) {
  ServiceLock lock(mu_);
  ObjectLayout layout;
  std::string err;
  if (!resolve_object_layout(cfg_, oid, layout_req, layout, err)) {
    return fail("bad_request", err);
  }
  auto placement = place(oid, map_, layout.n, layout.storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  if (auto lk = enforce_lock(oid, lock_token); !lk.ok) return lk;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  {
    auto tip_attrs = store->list_attrs(oid, err);
    if (attrs_are_frozen(tip_attrs)) {
      return fail("frozen", "object is archived/frozen; recall before mutate");
    }
  }
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

  // Compression (and EC) need the body in memory. Cap keeps RAM bounded.
  constexpr std::uint64_t kMemCompressLimit = 64ull * 1024ull * 1024ull;
  const bool want_compress = cfg_.compression == kCompAlgoZstd && zstd_available();
  if (layout.is_ec() || (want_compress && size <= kMemCompressLimit)) {
    if (layout.is_ec()) {
      constexpr std::uint64_t kEcMemLimit = 16ull * 1024ull * 1024ull;
      if (size > kEcMemLimit) {
        return fail("bad_request", "ec v1 supports objects up to 16 MiB");
      }
    }
    std::vector<std::uint8_t> buf;
    if (!file_read_exact(staging_abs_path, static_cast<std::size_t>(size), buf, err)) {
      return fail("store_error", err.empty() ? "cannot read staging file" : err);
    }
    PutPayload payload;
    if (!prepare_put_payload(cfg_, ops_, buf.data(), buf.size(),
                             expected_crc32c.value_or(crc32c_val), payload, err)) {
      if (err == "crc32c mismatch") return fail("crc_mismatch", err);
      return fail("store_error", err);
    }
    auto put_attrs = attrs;
    if (payload.compressed) {
      set_compression_attrs(put_attrs, kCompAlgoZstd, payload.logical_size, payload.logical_crc);
    }
    if (layout.is_ec()) {
      auto r = commit_ec_put(store, placement, oid, payload.data, payload.len, put_attrs,
                             replace_attrs, std::nullopt, layout);
      if (r.ok) {
        ops_.note_put(payload.logical_size);
        if (payload.compressed) ops_.note_compress(payload.logical_size, payload.len);
        if (r.info) {
          r.info->size = payload.logical_size;
          r.info->crc32c = payload.logical_crc;
          r.info->crc32c_known = true;
        }
      }
      return r;
    }
    apply_layout_attrs(put_attrs, layout);
    PreparedVersion pv;
    if (!store->prepare_put(oid, payload.data, payload.len, put_attrs, replace_attrs,
                            std::nullopt, pv, err)) {
      if (err == "crc32c mismatch") return fail("crc_mismatch", err);
      return fail("store_error", err);
    }
    auto r = commit_prepared(store, placement, pv, payload.data, payload.len, put_attrs);
    if (r.ok) {
      ops_.note_put(payload.logical_size);
      if (payload.compressed) ops_.note_compress(payload.logical_size, payload.len);
      if (r.info) {
        r.info->size = payload.logical_size;
        r.info->crc32c = payload.logical_crc;
        r.info->crc32c_known = true;
      }
    }
    return r;
  }
  if (want_compress && size > kMemCompressLimit) ops_.note_compress_skipped();

  auto put_attrs = attrs;
  apply_layout_attrs(put_attrs, layout);
  PreparedVersion pv;
  if (!store->prepare_put_file(oid, staging_abs_path, size, crc32c_val, put_attrs,
                               replace_attrs, expected_crc32c, pv, err)) {
    if (err == "crc32c mismatch") return fail("crc_mismatch", err);
    return fail("store_error", err);
  }
  auto r = commit_prepared(store, placement, pv, nullptr, 0, put_attrs);
  if (r.ok) {
    ops_.note_put(size);
  }
  return r;
}

ApiResult ObjectService::api_put_redirect(
    const std::string& oid, const std::string& target_oid,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    const std::vector<AttrPrecondition>& preds,
    const std::optional<std::string>& lock_token) {
  ServiceLock lock(mu_);
  auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  if (auto lk = enforce_lock(oid, lock_token); !lk.ok) return lk;
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

  PreparedVersion pv;
  if (!store->prepare_redirect(oid, target_oid, attrs, replace_attrs, pv, err)) {
    return fail("store_error", err);
  }
  auto r = commit_prepared(store, placement, pv, nullptr, 0, attrs);
  if (r.ok) r.redirect_oid = target_oid;
  return r;
}

ApiResult ObjectService::api_put_range(
    const std::string& oid, std::uint64_t offset, const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    const std::vector<AttrPrecondition>& preds, const LayoutRequest& layout_req,
    const std::optional<std::string>& lock_token) {
  // Range/append writes materialize the full new version on the primary and
  // replicate the whole body (not a delta), so memory and network cost scale
  // with object size rather than write size. Delta replication is out of scope.
  gc_client_writes();
  MutatingOid mutating(mu_, mutating_mu_, mutating_cv_, mutating_oids_, oid);
  ServiceLock lock(mu_);
  ObjectLayout layout;
  std::string err;
  if (!resolve_object_layout(cfg_, oid, layout_req, layout, err)) {
    return fail("bad_request", err);
  }
  if (layout.is_ec()) {
    return fail("bad_request", "ranged put not supported for erasure-coded objects");
  }
  auto placement = place(oid, map_, layout.n, layout.storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  if (auto lk = enforce_lock(oid, lock_token); !lk.ok) return lk;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

  {
    auto tip_attrs = store->list_attrs(oid, err);
    if (attrs_are_frozen(tip_attrs)) {
      return fail("frozen", "object is archived/frozen; recall before mutate");
    }
    if (attrs_are_ec(tip_attrs)) {
      return fail("bad_request", "ranged put not supported for erasure-coded objects");
    }
    if (attrs_are_compressed(tip_attrs)) {
      return fail("bad_request", "ranged put not supported for compressed objects");
    }
  }

  auto put_attrs = attrs;
  apply_layout_attrs(put_attrs, layout);
  PreparedVersion pv;
  if (!store->prepare_put_range(oid, offset, data, len, put_attrs, replace_attrs, pv, err)) {
    return fail("store_error", err);
  }
  auto full = store->get(oid, pv.seq, err);
  if (!full) {
    store->abort_version(oid, pv.seq, err);
    return fail("store_error", err);
  }
  PreparedVersion install = pv;
  install.inline_body = false;
  auto r = commit_prepared(store, placement, install, full->data(), full->size(), put_attrs);
  if (r.ok) {
    ops_.note_put_range(len);
  }
  return r;
}

ApiResult ObjectService::api_append(
    const std::string& oid, const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    const std::vector<AttrPrecondition>& preds, const LayoutRequest& layout_req,
    const std::optional<std::string>& lock_token) {
  // Range/append writes materialize the full new version on the primary and
  // replicate the whole body (not a delta), so memory and network cost scale
  // with object size rather than write size. Delta replication is out of scope.
  gc_client_writes();
  MutatingOid mutating(mu_, mutating_mu_, mutating_cv_, mutating_oids_, oid);
  ServiceLock lock(mu_);
  ObjectLayout layout;
  std::string err;
  if (!resolve_object_layout(cfg_, oid, layout_req, layout, err)) {
    return fail("bad_request", err);
  }
  if (layout.is_ec()) {
    return fail("bad_request", "append not supported for erasure-coded objects");
  }
  auto placement = place(oid, map_, layout.n, layout.storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  if (auto lk = enforce_lock(oid, lock_token); !lk.ok) return lk;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

  {
    auto tip_attrs = store->list_attrs(oid, err);
    if (attrs_are_ec(tip_attrs)) {
      return fail("bad_request", "append not supported for erasure-coded objects");
    }
    if (attrs_are_compressed(tip_attrs)) {
      return fail("bad_request", "append not supported for compressed objects");
    }
  }

  std::uint64_t offset = 0;
  auto tip = store->stat(oid, std::nullopt, err);
  if (tip && !tip->is_delete) {
    if (!tip->redirect_oid.empty()) {
      return fail("bad_request", "append not supported on redirect tip");
    }
    offset = tip->size;
  }

  auto put_attrs = attrs;
  apply_layout_attrs(put_attrs, layout);
  PreparedVersion pv;
  if (!store->prepare_put_range(oid, offset, data, len, put_attrs, replace_attrs, pv, err)) {
    return fail("store_error", err);
  }
  auto full = store->get(oid, pv.seq, err);
  if (!full) {
    store->abort_version(oid, pv.seq, err);
    return fail("store_error", err);
  }
  PreparedVersion install = pv;
  install.inline_body = false;
  auto r = commit_prepared(store, placement, install, full->data(), full->size(), put_attrs);
  if (r.ok) {
    const std::uint64_t new_size = r.info ? r.info->size : offset + static_cast<std::uint64_t>(len);
    const std::uint64_t seq = r.info ? r.info->seq : 0;
    r.json_body = nlohmann::json{{"offset", offset},
                                 {"size", new_size},
                                 {"seq", seq},
                                 {"epoch", r.epoch}};
    ops_.note_append(len);
  }
  return r;
}

ApiResult ObjectService::api_get(const std::string& oid, std::optional<std::uint64_t> offset,
                                std::optional<std::uint64_t> end_inclusive,
                                const std::vector<AttrPrecondition>& preds,
                                std::optional<std::uint64_t> seq, bool meta_only) {
  ServiceLock lock(mu_);
  std::string err;
  ObjectStore* store = nullptr;
  std::optional<ObjectInfo> info;
  std::unordered_map<std::string, std::string> attrs;

  // Discover tip on any local store first (class-scoped rings may not include us until
  // we know aios.storage_class from attrs).
  for (const auto& path : stores_.paths()) {
    auto* s = stores_.get(path);
    if (!s) continue;
    auto st = s->stat(oid, seq, err);
    if (!st) continue;
    if (st->is_delete && !seq.has_value()) continue;
    store = s;
    info = st;
    attrs = s->list_attrs(oid, err);
    break;
  }

  const std::string sc =
      storage_class_for_attrs(attrs, cfg_.default_storage_class);
  const std::string sc_prev = storage_class_prev_for_attrs(attrs);
  const int n = placement_n_for_attrs(attrs, map_.replica_count);
  auto placement = place(oid, map_, n, sc);
  if (placement.acting_set.empty() && !sc_prev.empty()) {
    placement = place(oid, map_, n, sc_prev);
  }
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");

  // Prefer a local acting-set member for the chosen class when available.
  if (info) {
    for (const auto& t : placement.acting_set) {
      if (t.node_id != cfg_.node_id) continue;
      auto* s = stores_.get(t.aios_path);
      if (!s) continue;
      auto st = s->stat(oid, seq, err);
      if (!st) continue;
      if (st->is_delete && !seq.has_value()) continue;
      store = s;
      info = st;
      attrs = s->list_attrs(oid, err);
      break;
    }
  }

  if (!store) {
    // No local copy — still allow EC reconstruct / remote attr probe if we are
    // contacted as a gateway; require at least one local store for non-EC.
    if (!attrs_are_ec(attrs)) {
      // Probe remotes for attrs when tip class is known from a prior local miss.
      for (const auto& t : placement.acting_set) {
        if (t.node_id == cfg_.node_id) {
          store = stores_.get(t.aios_path);
          if (store) break;
        }
      }
      if (!store) {
        // The object is fine, this node just is not holding it. Carry the acting
        // set so the client is redirected instead of being told the server broke.
        auto r = fail("not_local", "no local replica for oid");
        r.placement = placement;
        return r;
      }
    }
  }

  if (store && !seq.has_value()) {
    auto pr = check_preds_on(store, oid, preds, err);
    if (pr == PrecondResult::NotFound) return fail("not_found", err);
    if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);
  }

  // Degraded read: no local tip — pull attrs from a remote acting-set member.
  if (!info) {
    UnlockForRpc unlock(mu_);
    for (const auto& t : placement.acting_set) {
      if (t.node_id == cfg_.node_id) continue;
      auto st = object_stat_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                   cfg_.auth_skew_ms, cur_epoch(), t.aios_path, oid);
      if (!st.ok) continue;
      if (st.body.contains("attrs") && st.body["attrs"].is_object()) {
        for (auto it = st.body["attrs"].begin(); it != st.body["attrs"].end(); ++it) {
          if (it.value().is_string()) attrs[it.key()] = it.value().get<std::string>();
        }
      }
      if (attrs_are_ec(attrs) || st.ok) {
        info = ObjectInfo{};
        info->oid = oid;
        info->seq = st.body.value("seq", static_cast<std::uint64_t>(0));
        if (attrs_are_ec(attrs)) break;
        if (!info->seq) continue;
        break;
      }
    }
    // Dual-home: try previous class ring during transition.
    if (!info && !sc_prev.empty()) {
      auto prev_p = place(oid, map_, n, sc_prev);
      for (const auto& t : prev_p.acting_set) {
        if (t.node_id == cfg_.node_id) continue;
        auto st = object_stat_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                     cfg_.auth_skew_ms, cur_epoch(), t.aios_path, oid);
        if (!st.ok) continue;
        if (st.body.contains("attrs") && st.body["attrs"].is_object()) {
          for (auto it = st.body["attrs"].begin(); it != st.body["attrs"].end(); ++it) {
            if (it.value().is_string()) attrs[it.key()] = it.value().get<std::string>();
          }
        }
        info = ObjectInfo{};
        info->oid = oid;
        info->seq = st.body.value("seq", static_cast<std::uint64_t>(0));
        placement = std::move(prev_p);
        break;
      }
    }
  }

  if (!info) return fail("not_found", "object not found");
  if (info->is_delete && !seq.has_value()) return fail("not_found", "object not found");

  if (!info->redirect_oid.empty()) {
    ApiResult r;
    r.ok = true;
    r.epoch = cur_epoch();
    r.info = info;
    r.attrs = attrs;
    r.placement = placement;
    r.redirect_oid = info->redirect_oid;
    r.code = "redirect";
    return r;
  }

  if (attrs_are_frozen(attrs)) {
    const std::string st = archive_state_for_attrs(attrs);
    if (st == kArchiveStateOnTape || st == kArchiveStateRestoring) {
      ApiResult r;
      r.ok = false;
      r.code = "restoring";
      r.error = "archived object is on tape / restoring";
      r.epoch = cur_epoch();
      r.info = info;
      r.attrs = attrs;
      r.placement = placement;
      if (auto it = attrs.find(kBagLengthAttr); it != attrs.end()) {
        try {
          info->size = std::stoull(it->second);
        } catch (...) {
        }
      }
      return r;
    }
    if (meta_only) {
      ApiResult r;
      r.ok = true;
      r.epoch = cur_epoch();
      r.info = info;
      r.attrs = attrs;
      r.placement = placement;
      if (auto it = attrs.find(kBagLengthAttr); it != attrs.end()) {
        try {
          info->size = std::stoull(it->second);
          r.info->size = info->size;
        } catch (...) {
        }
      }
      ops_.note_head();
      return r;
    }
    std::vector<std::uint8_t> member;
    std::string ferr;
    bool froze_ok = false;
    {
      UnlockForRpc unlock(mu_);
      froze_ok = read_frozen_member(cfg_, advertise_, map_, stores_, attrs, member, ferr);
    }
    if (!froze_ok) {
      if (ferr == "restoring") {
        ApiResult r;
        r.ok = false;
        r.code = "restoring";
        r.error = ferr;
        r.epoch = cur_epoch();
        r.info = info;
        r.attrs = attrs;
        return r;
      }
      return fail("store_error", ferr);
    }
    ApiResult r;
    r.ok = true;
    r.epoch = cur_epoch();
    r.info = info;
    r.attrs = attrs;
    r.placement = placement;
    r.info->size = member.size();
    // Optional range.
    if (offset.has_value()) {
      const std::uint64_t off = *offset;
      if (off >= member.size()) {
        r.data = std::vector<std::uint8_t>{};
      } else {
        std::size_t len = member.size() - static_cast<std::size_t>(off);
        if (end_inclusive.has_value()) {
          const std::uint64_t end =
              std::min(*end_inclusive, static_cast<std::uint64_t>(member.size() - 1));
          if (end >= off) len = static_cast<std::size_t>(end - off + 1);
          else len = 0;
        }
        r.data = std::vector<std::uint8_t>(member.begin() + static_cast<std::ptrdiff_t>(off),
                                           member.begin() + static_cast<std::ptrdiff_t>(off + len));
      }
    } else {
      r.data = std::move(member);
    }
    ops_.note_get(r.data ? r.data->size() : 0);
    return r;
  }

  // EC objects carry aios.ec.* attrs. Non-EC tips (e.g. txn-prepared full copies) still
  // use the normal local read path even when the cluster default layout is ec.
  auto apply_range = [&](ApiResult& r, std::uint64_t total) -> ApiResult {
    if (!offset.has_value()) return r;
    if (*offset >= total) return fail("range_unsatisfiable", "range unsatisfiable");
    std::uint64_t end = end_inclusive.value_or(total - 1);
    if (end >= total) end = total - 1;
    if (end < *offset) return fail("range_unsatisfiable", "range unsatisfiable");
    r.body_offset = *offset;
    r.body_length = end - *offset + 1;
    return r;
  };

  auto note_get = [this](ApiResult& r) {
    if (!r.ok || r.code == "redirect") return;
    std::uint64_t bytes = 0;
    if (r.body_length > 0) bytes = r.body_length;
    else if (r.data) bytes = r.data->size();
    else if (r.info) bytes = r.info->size;
    ops_.note_get(bytes);
  };

  if (meta_only) {
    ApiResult r;
    r.ok = true;
    r.epoch = cur_epoch();
    r.info = info;
    r.attrs = attrs;
    r.placement = placement;
    if (attrs_are_compressed(attrs)) {
      if (auto sz = compression_full_size(attrs)) r.info->size = *sz;
    } else if (attrs_are_ec(attrs)) {
      if (auto em = parse_ec_attrs(attrs)) r.info->size = em->full_size;
    }
    r = apply_range(r, r.info->size);
    if (!r.ok) return r;
    ops_.note_head();
    return r;
  }

  if (attrs_are_ec(attrs)) {
    const int en = placement_n_for_attrs(attrs, map_.replica_count);
    const std::string esc = storage_class_for_attrs(attrs, cfg_.default_storage_class);
    placement = place(oid, map_, en, esc);
    if (placement.acting_set.empty()) {
      const std::string prev = storage_class_prev_for_attrs(attrs);
      if (!prev.empty()) placement = place(oid, map_, en, prev);
    }
    if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
    const bool compressed = attrs_are_compressed(attrs);
    ApiResult rec;
    {
      UnlockForRpc unlock(mu_);
      // Compressed EC bodies must be reconstructed in full before zstd can run.
      rec = compressed ? reconstruct_ec_object(placement, oid, seq, attrs)
                       : reconstruct_ec_object(placement, oid, seq, attrs, offset, end_inclusive);
    }
    if (!rec.ok) return rec;
    rec.info->seq = info->seq;
    rec.info->mtime_ms = info->mtime_ms;
    rec.info->ctime_ms = info->ctime_ms;
    rec.attrs = attrs;
    if (!decompress_api_result(rec, err, cfg_.max_object_bytes)) return fail("store_error", err);
    if (compressed && offset.has_value()) {
      if (!rec.data) return fail("store_error", "ec reconstruct produced no data");
      if (*offset >= rec.data->size()) return fail("range_unsatisfiable", "range unsatisfiable");
      std::uint64_t end = end_inclusive.value_or(rec.data->size() - 1);
      if (end >= rec.data->size()) end = rec.data->size() - 1;
      if (end < *offset) return fail("range_unsatisfiable", "range unsatisfiable");
      rec.data = std::vector<std::uint8_t>(
          rec.data->begin() + static_cast<std::ptrdiff_t>(*offset),
          rec.data->begin() + static_cast<std::ptrdiff_t>(end + 1));
    }
    note_get(rec);
    return rec;
  }

  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.info = info;
  r.attrs = attrs;
  r.placement = placement;

  const bool compressed = attrs_are_compressed(attrs);
  constexpr std::uint64_t kStreamThreshold = 256u * 1024u;
  if (!compressed && !info->inline_body && info->size >= kStreamThreshold) {
    if (auto path = store->fs_body_path(oid, seq, err)) {
      r.body_path = *path;
      r = apply_range(r, info->size);
      if (!r.ok) return r;
      if (!offset.has_value()) r.body_length = info->size;
      note_get(r);
      return r;
    }
  }

  // Compressed tips: always load full stored body, decompress, then slice.
  if (compressed || !offset.has_value()) {
    {
      UnlockForRpc unlock(mu_);
      r.data = store->get(oid, seq, err);
    }
    if (!r.data) {
      if (info->is_delete) {
        r.data = std::vector<std::uint8_t>{};
        note_get(r);
        return r;
      }
      return fail("not_found", err);
    }
    if (!decompress_api_result(r, err, cfg_.max_object_bytes)) return fail("store_error", err);
    if (!offset.has_value()) {
      note_get(r);
      return r;
    }
    if (*offset >= r.data->size()) return fail("range_unsatisfiable", "range unsatisfiable");
    std::uint64_t end = end_inclusive.value_or(r.data->size() - 1);
    if (end >= r.data->size()) end = r.data->size() - 1;
    if (end < *offset) return fail("range_unsatisfiable", "range unsatisfiable");
    std::vector<std::uint8_t> slice(r.data->begin() + static_cast<std::ptrdiff_t>(*offset),
                                    r.data->begin() + static_cast<std::ptrdiff_t>(end + 1));
    r.data = std::move(slice);
    note_get(r);
    return r;
  }

  if (*offset >= info->size) return fail("range_unsatisfiable", "range unsatisfiable");
  std::uint64_t end = end_inclusive.value_or(info->size - 1);
  if (end >= info->size) end = info->size - 1;
  if (end < *offset) return fail("range_unsatisfiable", "range unsatisfiable");
  const std::size_t len = static_cast<std::size_t>(end - *offset + 1);
  {
    UnlockForRpc unlock(mu_);
    r.data = store->get_range(oid, seq, *offset, len, err);
  }
  if (!r.data) {
    if (err == "range unsatisfiable") return fail("range_unsatisfiable", err);
    return fail("store_error", err);
  }
  note_get(r);
  return r;
}

ApiResult ObjectService::api_head(const std::string& oid,
                                 const std::vector<AttrPrecondition>& preds,
                                 std::optional<std::uint64_t> seq) {
  return api_get(oid, std::nullopt, std::nullopt, preds, seq, /*meta_only=*/true);
}

ApiResult ObjectService::api_del(const std::string& oid,
                                const std::vector<AttrPrecondition>& preds,
                                const std::optional<std::string>& lock_token) {
  ServiceLock lock(mu_);
  std::string tip_class = cfg_.default_storage_class;
  {
    std::string err;
    for (const auto& path : stores_.paths()) {
      auto* s = stores_.get(path);
      if (!s) continue;
      auto st = s->stat(oid, err);
      if (!st || st->is_delete) continue;
      tip_class = storage_class_for_attrs(s->list_attrs(oid, err), tip_class);
      break;
    }
  }
  auto placement = place(oid, map_, tip_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  if (auto lk = enforce_lock(oid, lock_token); !lk.ok) return lk;
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

  PreparedVersion pv;
  if (!store->prepare_delete(oid, pv, err)) {
    if (err == "object not found") return fail("not_found", err);
    return fail("store_error", err);
  }
  auto r = commit_prepared(store, placement, pv, nullptr, 0, {});
  if (r.ok) ops_.note_del();
  return r;
}

namespace {

// LIST cursors are opaque per-source continuation tokens: {source -> cursor}
// (source = store path locally, node_id cluster-wide), base64-encoded. A store's
// own cursor is shard-scoped, so a bare oid cannot resume it, and one shared
// cursor across stores would skip entries of the stores that were still behind.
// A source missing from a non-empty map is exhausted.
constexpr const char* kListCursorTag = "c2:";

bool decode_list_cursor(const std::string& cursor,
                        std::unordered_map<std::string, std::string>& out, std::string& err) {
  out.clear();
  if (cursor.empty()) return true;
  if (cursor.rfind(kListCursorTag, 0) != 0) {
    err = "bad cursor";
    return false;
  }
  std::vector<std::uint8_t> raw;
  if (!base64_decode(cursor.substr(std::strlen(kListCursorTag)), raw, err)) return false;
  try {
    const auto j = nlohmann::json::parse(raw.begin(), raw.end());
    if (!j.is_object()) {
      err = "bad cursor";
      return false;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.value().is_string()) out[it.key()] = it.value().get<std::string>();
    }
  } catch (const std::exception&) {
    err = "bad cursor";
    return false;
  }
  return true;
}

std::string encode_list_cursor(const std::unordered_map<std::string, std::string>& next) {
  if (next.empty()) return {};
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [k, v] : next) j[k] = v;
  return kListCursorTag + base64_encode(j.dump());
}

void sort_dedupe_by_oid(std::vector<ObjectListEntry>& v) {
  std::stable_sort(v.begin(), v.end(), [](const ObjectListEntry& a, const ObjectListEntry& b) {
    return a.oid < b.oid;
  });
  std::vector<ObjectListEntry> uniq;
  uniq.reserve(v.size());
  for (auto& o : v) {
    if (!uniq.empty() && uniq.back().oid == o.oid) continue;
    uniq.push_back(std::move(o));
  }
  v.swap(uniq);
}

}  // namespace

ApiResult ObjectService::api_list(const std::string& prefix, const std::string& attr_eq_key,
                                  const std::string& attr_eq_value, std::size_t limit,
                                  const std::string& cursor, bool include_attrs,
                                  bool cluster) {
  ServiceLock lock(mu_);
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  if (limit == 0 || limit > kListLimitMax) limit = kListLimitMax;

  // Every store contributes its own page (sized so the union fits `limit`);
  // pages are sorted by oid and each store resumes from its own cursor, so no
  // entry is dropped between pages.
  auto list_local = [&](const std::string& local_cursor) -> ApiResult {
    ApiResult lr;
    lr.ok = true;
    lr.epoch = cur_epoch();
    std::unordered_map<std::string, std::string> cursors;
    std::string cerr;
    if (!decode_list_cursor(local_cursor, cursors, cerr)) return fail("bad_request", cerr);
    const auto paths = stores_.paths();
    const std::size_t per_store = std::max<std::size_t>(1, limit / std::max<std::size_t>(1, paths.size()));
    std::unordered_map<std::string, std::string> next;
    bool any_ok = false;
    std::string first_err;
    for (const auto& path : paths) {
      auto* store = stores_.get(path);
      if (!store) continue;
      std::string store_cursor;
      if (!local_cursor.empty()) {
        auto it = cursors.find(path);
        if (it == cursors.end()) continue;  // exhausted
        store_cursor = it->second;
      }
      std::string err;
      auto part = store->list(prefix, attr_eq_key, attr_eq_value, per_store, store_cursor,
                              include_attrs, err);
      if (!err.empty()) {
        if (first_err.empty()) first_err = err;
        continue;
      }
      any_ok = true;
      if (!part.next_cursor.empty()) next[path] = part.next_cursor;
      for (auto& o : part.objects) lr.list.objects.push_back(std::move(o));
    }
    if (!any_ok && !first_err.empty()) return fail("store_error", first_err);
    sort_dedupe_by_oid(lr.list.objects);
    lr.list.next_cursor = encode_list_cursor(next);
    return lr;
  };

  if (!cluster) {
    auto lr = list_local(cursor);
    if (lr.ok) ops_.note_list();
    return lr;
  }

  // Scatter-gather by unique node, each resuming from its own cursor.
  std::unordered_map<std::string, std::string> cursors;
  std::string cerr;
  if (!decode_list_cursor(cursor, cursors, cerr)) return fail("bad_request", cerr);
  std::vector<std::string> node_ids;
  std::vector<std::string> node_addrs;
  {
    std::unordered_set<std::string> seen_nodes;
    for (const auto& t : map_.targets) {
      if (!seen_nodes.insert(t.node_id).second) continue;
      node_ids.push_back(t.node_id);
      node_addrs.push_back(t.addr);
    }
  }
  const std::size_t per_node = std::max<std::size_t>(1, limit / std::max<std::size_t>(1, node_ids.size()));
  std::unordered_map<std::string, std::string> next;
  for (std::size_t i = 0; i < node_ids.size(); ++i) {
    const auto& node_id = node_ids[i];
    std::string node_cursor;
    if (!cursor.empty()) {
      auto it = cursors.find(node_id);
      if (it == cursors.end()) continue;  // exhausted
      node_cursor = it->second;
    }
    ObjectListResult part;
    if (node_id == cfg_.node_id) {
      auto lr = list_local(node_cursor);
      if (!lr.ok) return lr;
      part = std::move(lr.list);
    } else {
      UnlockForRpc unlock(mu_);
      auto remote =
          object_list_remote(node_addrs[i], cfg_.node_id, advertise_, cfg_.cluster_key,
                             cfg_.auth_skew_ms, cur_epoch(), prefix, attr_eq_key,
                             attr_eq_value, per_node, node_cursor, include_attrs);
      if (!remote.ok) {
        AIOS_LOG_WARN("cluster list from ", node_addrs[i], " failed: ", remote.error);
        continue;
      }
      part = std::move(remote.list);
    }
    if (!part.next_cursor.empty()) next[node_id] = part.next_cursor;
    for (auto& o : part.objects) {
      if (!prefix.empty() && o.oid.rfind(prefix, 0) != 0) continue;
      r.list.objects.push_back(std::move(o));
    }
  }
  sort_dedupe_by_oid(r.list.objects);
  r.list.next_cursor = encode_list_cursor(next);
  ops_.note_list();
  return r;
}

ApiResult ObjectService::api_list_versions(const std::string& oid) {
  ServiceLock lock(mu_);
  auto placement = place(oid, map_, cfg_.default_storage_class);
  ObjectStore* store = nullptr;
  std::string err;
  for (const auto& t : placement.acting_set) {
    if (t.node_id != cfg_.node_id) continue;
    store = stores_.get(t.aios_path);
    if (store) break;
  }
  if (!store) {
    for (const auto& path : stores_.paths()) {
      store = stores_.get(path);
      if (store) break;
    }
  }
  if (!store) {
    auto r = fail("not_local", "no local store");
    r.placement = placement;
    return r;
  }
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.versions = store->list_versions(oid, err);
  return r;
}

ApiResult ObjectService::api_purge_version(const std::string& oid, std::uint64_t seq,
                                           bool allow_tip) {
  ServiceLock lock(mu_);
  auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    return fail("not_primary", "this node is not primary for oid");
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  if (!store->purge_version(oid, seq, allow_tip, err)) return fail("store_error", err);
  // Best-effort fan-out.
  UnlockForRpc unlock(mu_);
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    const auto& t = placement.acting_set[i];
    if (t.node_id == cfg_.node_id) {
      local_abort(t.aios_path, oid, seq, err);
      continue;
    }
    object_abort_version_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                cfg_.auth_skew_ms, placement.epoch, t.aios_path, oid, seq);
  }
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  return r;
}

ApiResult ObjectService::api_trim_versions(const std::string& oid, int keep) {
  ServiceLock lock(mu_);
  auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    return fail("not_primary", "this node is not primary for oid");
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  if (keep <= 0) keep = store->options().max_versions;
  if (!store->trim_versions(oid, keep, err)) return fail("store_error", err);
  UnlockForRpc unlock(mu_);
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    const auto& t = placement.acting_set[i];
    if (t.node_id == cfg_.node_id) {
      auto* s = stores_.get(t.aios_path);
      if (s) s->trim_versions(oid, keep, err);
      continue;
    }
    object_purge_versions_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                 cfg_.auth_skew_ms, placement.epoch, t.aios_path, oid, keep);
  }
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  return r;
}

Frame ObjectService::handle_stage_begin(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(json_u64(body, "epoch"), errf)) return errf;
  const std::string oid = json_str(body, "oid");
  const std::string aios_path = json_str(body, "aios_path");
  const auto seq = json_u64(body, "seq");
  if (oid.empty() || aios_path.empty() || seq == 0) {
    return reply_err(cur_epoch(), "bad_request", "oid/aios_path/seq required");
  }
  ServiceLock lock(mu_);
  if (!in_acting_set(oid, map_, storage_class_of_target(map_, cfg_.node_id, aios_path), cfg_.node_id, aios_path)) {
    return reply_err(cur_epoch(), "not_replica", "not in acting set for oid");
  }
  gc_stage_sessions();
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(cur_epoch(), "store_error", "no local store");
  std::string path, err;
  if (!store->stage_path_for(oid, seq, path, err)) {
    return reply_err(cur_epoch(), "store_error", err);
  }
  const std::string key = stage_key(aios_path, oid, seq);
  // Any earlier session for this version is superseded; close it before the
  // truncate so an in-flight StageData on it cannot write into the new file.
  close_stage_session(key);
  if (!store->stage_truncate(path, err)) {
    return reply_err(cur_epoch(), "store_error", err);
  }
  auto sess = std::make_shared<StageSession>();
  sess->path = path;
  sess->fd = ::open(path.c_str(), O_RDWR, 0644);
  if (sess->fd < 0) {
    return reply_err(cur_epoch(), "store_error",
                     std::string("open staging: ") + std::strerror(errno));
  }
  sess->last_used_ms = now_ms();
  stages_[key] = std::move(sess);
  return reply_ok(cur_epoch());
}

Frame ObjectService::handle_stage_data(const Frame& req) {
  Frame errf;
  if (!epoch_ok(json_u64(req.body, "epoch"), errf)) return errf;
  const std::string oid = json_str(req.body, "oid");
  const std::string aios_path = json_str(req.body, "aios_path");
  const auto seq = json_u64(req.body, "seq");
  const auto offset = json_u64(req.body, "offset");
  if (oid.empty() || aios_path.empty() || seq == 0) {
    return reply_err(cur_epoch(), "bad_request", "oid/aios_path/seq required");
  }

  std::shared_ptr<StageSession> sess;
  {
    ServiceLock lock(mu_);
    auto it = stages_.find(stage_key(aios_path, oid, seq));
    if (it == stages_.end() || !it->second) {
      return reply_err(cur_epoch(), "not_found", "no stage session; StageBegin required");
    }
    sess = it->second;
  }

  const auto* pdata = req.raw_data();
  const auto plen = req.raw_size();
  if (!pdata && plen > 0) {
    return reply_err(cur_epoch(), "store_error", "empty stage chunk");
  }

  // Disk I/O off mu_ so concurrent stage streams do not serialize on the service
  // lock; the session mutex keeps the fd alive and bytes/crc consistent with the
  // data actually written even if StageBegin/Commit/Abort race on the same key.
  std::lock_guard slock(sess->mu);
  if (sess->closed || sess->fd < 0) {
    return reply_err(cur_epoch(), "not_found", "stage session closed");
  }
  if (offset != sess->bytes) {
    return reply_err(cur_epoch(), "bad_request", "stage offset mismatch");
  }
  std::size_t done = 0;
  while (done < plen) {
    const ssize_t n = ::pwrite(sess->fd, pdata + done, plen - done,
                               static_cast<off_t>(offset + done));
    if (n < 0) {
      return reply_err(cur_epoch(), "store_error",
                       std::string("pwrite: ") + std::strerror(errno));
    }
    if (n == 0) {
      return reply_err(cur_epoch(), "store_error", "pwrite short write");
    }
    done += static_cast<std::size_t>(n);
  }
  sess->crc = crc32c_update(sess->crc, pdata, plen);
  sess->bytes += plen;
  sess->last_used_ms = now_ms();
  return reply_ok(cur_epoch());
}

Frame ObjectService::handle_stage_commit(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(json_u64(body, "epoch"), errf)) return errf;
  const std::string oid = json_str(body, "oid");
  const std::string aios_path = json_str(body, "aios_path");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(cur_epoch(), "bad_request", "oid/aios_path required");
  }
  ServiceLock lock(mu_);
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(cur_epoch(), "store_error", "no local store");

  PreparedVersion v;
  v.oid = oid;
  v.seq = json_u64(body, "seq");
  v.prev_tip = json_u64(body, "base_seq");
  v.size = json_u64(body, "size");
  v.crc32c = json_u32(body, "crc32c");
  v.inline_body = false;
  v.is_delete = json_bool(body, "is_delete", false);
  v.fs_path.clear();  // never from the wire; set from place_staging_as_version below
  v.redirect_oid = json_str(body, "redirect");
  if (v.seq == 0) return reply_err(cur_epoch(), "bad_request", "seq required");

  const std::string key = stage_key(aios_path, oid, v.seq);
  const bool needs_body = !v.is_delete && v.redirect_oid.empty() && v.size > 0;
  std::string staging;
  auto sit = stages_.find(key);
  if (sit == stages_.end() && needs_body) {
    return reply_err(cur_epoch(), "not_found", "no stage session; StageBegin required");
  }
  if (sit != stages_.end()) {
    auto sess = sit->second;
    stages_.erase(sit);
    std::lock_guard slock(sess->mu);
    sess->closed = true;
    const bool consistent = sess->bytes == v.size && sess->crc == v.crc32c;
    if (consistent && sess->fd >= 0 && cfg_.data_fsync) ::fsync(sess->fd);
    if (sess->fd >= 0) {
      ::close(sess->fd);
      sess->fd = -1;
    }
    if (!consistent) {
      std::error_code ec;
      std::filesystem::remove(sess->path, ec);
      return reply_err(cur_epoch(), "store_error",
                       sess->bytes != v.size ? "stage size mismatch" : "stage crc32c mismatch");
    }
    v.crc_verified = true;
    staging = sess->path;
  }

  auto attrs = parse_attrs_json(body);
  std::string err;
  if (needs_body) {
    if (staging.empty() && !store->stage_path_for(oid, v.seq, staging, err)) {
      return reply_err(cur_epoch(), "store_error", err);
    }
    std::string rel;
    if (!store->place_staging_as_version(oid, v.seq, staging, rel, err)) {
      return reply_err(cur_epoch(), "store_error", err);
    }
    v.fs_path = rel;
  }
  if (!store->install_version(v, nullptr, 0, attrs, err)) {
    return reply_err(cur_epoch(), "store_error", err);
  }
  return reply_ok(cur_epoch());
}

Frame ObjectService::handle_list(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(json_u64(body, "epoch"), errf)) return errf;
  const std::string prefix = json_str(body, "prefix");
  const std::string attr_key = json_str(body, "attr_eq_key");
  const std::string attr_val = json_str(body, "attr_eq_value");
  const std::size_t limit = json_u64(body, "limit", 1000);
  const std::string cursor = json_str(body, "cursor");
  const bool include_attrs = json_bool(body, "attrs", false);

  // Local-only listing for scatter-gather leaves.
  auto r = api_list(prefix, attr_key, attr_val, limit, cursor, include_attrs,
                    /*cluster=*/false);
  if (!r.ok) return reply_err(cur_epoch(), r.code, r.error);
  Frame f = reply_ok(cur_epoch());
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& o : r.list.objects) {
    nlohmann::json jo = {{"oid", o.oid},
                         {"seq", o.seq},
                         {"size", o.size},
                         {"mtime_ms", o.mtime_ms},
                         {"crc32c", o.crc32c},
                         {"is_delete", o.is_delete},
                         {"redirect_oid", o.redirect_oid}};
    if (include_attrs) jo["attrs"] = o.attrs;
    arr.push_back(std::move(jo));
  }
  f.body["objects"] = std::move(arr);
  f.body["next_cursor"] = r.list.next_cursor;
  return f;
}

namespace {

std::string make_txn_id() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::ostringstream os;
  os << std::hex << aios::now_ms() << "-" << rng();
  return os.str();
}

std::string txn_oid(const std::string& txn_id) { return "txn/" + txn_id; }

}  // namespace

ApiResult ObjectService::api_prepare_put(
    const std::string& oid, const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    const std::vector<AttrPrecondition>& preds, std::optional<std::uint32_t> expected_crc32c,
    const std::optional<std::string>& lock_token) {
  ServiceLock lock(mu_);
  auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  if (auto lk = enforce_lock(oid, lock_token); !lk.ok) return lk;
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);
  PreparedVersion pv;
  if (!store->prepare_put(oid, data, len, attrs, replace_attrs, expected_crc32c, pv, err)) {
    if (err == "crc32c mismatch") return fail("crc_mismatch", err);
    return fail("store_error", err);
  }
  return install_prepared(store, placement, pv, data, len, attrs);
}

ApiResult ObjectService::api_prepare_put_file(
    const std::string& oid, const std::string& staging_abs_path, std::uint64_t size,
    std::uint32_t crc32c_val, const std::unordered_map<std::string, std::string>& attrs,
    bool replace_attrs, const std::vector<AttrPrecondition>& preds,
    std::optional<std::uint32_t> expected_crc32c,
    const std::optional<std::string>& lock_token) {
  ServiceLock lock(mu_);
  auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  if (auto lk = enforce_lock(oid, lock_token); !lk.ok) return lk;
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);
  PreparedVersion pv;
  if (!store->prepare_put_file(oid, staging_abs_path, size, crc32c_val, attrs, replace_attrs,
                               expected_crc32c, pv, err)) {
    if (err == "crc32c mismatch") return fail("crc_mismatch", err);
    return fail("store_error", err);
  }
  return install_prepared(store, placement, pv, nullptr, 0, attrs);
}

ApiResult ObjectService::api_prepare_delete(const std::string& oid,
                                           const std::vector<AttrPrecondition>& preds,
                                           const std::optional<std::string>& lock_token) {
  ServiceLock lock(mu_);
  auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  if (auto lk = enforce_lock(oid, lock_token); !lk.ok) return lk;
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);
  PreparedVersion pv;
  if (!store->prepare_delete(oid, pv, err)) {
    if (err == "object not found") return fail("not_found", err);
    return fail("store_error", err);
  }
  return install_prepared(store, placement, pv, nullptr, 0, {});
}

ApiResult ObjectService::api_publish_version(const std::string& oid, std::uint64_t seq) {
  ServiceLock lock(mu_);
  auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  if (!store->publish_tip(oid, seq, err)) return fail("store_error", err);
  replicate_publish(placement, oid, seq);
  std::string op = "put";
  if (auto st = store->stat(oid, err)) {
    if (st->is_delete) op = "del";
  }
  signal_watch(oid, seq, op);
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  if (auto st = store->stat(oid, err)) r.info = st;
  return r;
}

ApiResult ObjectService::api_abort_prepared(const std::string& oid, std::uint64_t seq) {
  ServiceLock lock(mu_);
  auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  if (!store->abort_version(oid, seq, err)) return fail("store_error", err);
  replicate_abort(placement, oid, seq);
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  return r;
}

ApiResult ObjectService::load_txn_state(const std::string& txn_id, nlohmann::json& state_out) {
  const auto oid = txn_oid(txn_id);
  auto got = api_get(oid, std::nullopt, std::nullopt, {}, std::nullopt);
  if (!got.ok) return got;
  if (got.data->empty()) return fail("bad_request", "empty txn state");
  try {
    state_out = nlohmann::json::parse(got.data->begin(), got.data->end());
  } catch (const std::exception& e) {
    return fail("bad_request", std::string("bad txn json: ") + e.what());
  }
  return got;
}

ApiResult ObjectService::save_txn_state(const std::string& txn_id, const nlohmann::json& state) {
  const auto oid = txn_oid(txn_id);
  const auto body = state.dump();
  return api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                 {{"aios.txn", "1"}}, true, {},
                 crc32c(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()),
                 layout_request_replica());
}

ApiResult ObjectService::require_txn_primary(const std::string& txn_id,
                                             nlohmann::json& state_out) {
  const auto oid = txn_oid(txn_id);
  auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not txn coordinator");
    r.placement = placement;
    return r;
  }
  if (auto g = primary_gate(oid, placement); !g.ok) return g;
  auto loaded = load_txn_state(txn_id, state_out);
  if (!loaded.ok) return loaded;
  loaded.placement = placement;
  return loaded;
}

ApiResult ObjectService::api_txn_begin() {
  ServiceLock lock(mu_);
  // Pick a txn id whose primary is this node (coordinator = primary for txn/<id>).
  for (int attempt = 0; attempt < 64; ++attempt) {
    const auto id = make_txn_id();
    const auto oid = txn_oid(id);
    auto placement = place(oid, map_, cfg_.default_storage_class);
    if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
    if (placement.acting_set[0].node_id != cfg_.node_id) continue;
    nlohmann::json state = {{"txn_id", id},
                            {"state", "open"},
                            {"ops", nlohmann::json::array()}};
    const auto body = state.dump();
    std::vector<AttrPrecondition> create_preds = {
        {AttrPrecondition::Kind::MustNotExist, {}, {}}};
    auto saved =
        api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                {{"aios.txn", "1"}}, true, create_preds,
                crc32c(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()),
                layout_request_replica());
    if (!saved.ok) {
      if (saved.code == "precondition_failed") continue;
      return saved;
    }
    ApiResult r;
    r.ok = true;
    r.epoch = cur_epoch();
    r.placement = placement;
    r.attrs["txn_id"] = id;
    // Stash JSON in body for HTTP.
    const auto s = state.dump();
    r.data = std::vector<std::uint8_t>(s.begin(), s.end());
    return r;
  }
  return fail("store_error", "could not allocate txn id");
}

ApiResult ObjectService::api_txn_get(const std::string& txn_id) {
  ServiceLock lock(mu_);
  nlohmann::json state;
  auto r = require_txn_primary(txn_id, state);
  if (!r.ok) return r;
  const auto s = state.dump();
  r.data = std::vector<std::uint8_t>(s.begin(), s.end());
  r.attrs["txn_id"] = txn_id;
  return r;
}

ApiResult ObjectService::api_txn_prepare_put(
    const std::string& txn_id, const std::string& oid, const std::uint8_t* data,
    std::size_t len, const std::unordered_map<std::string, std::string>& attrs,
    const std::vector<AttrPrecondition>& preds, std::optional<std::uint32_t> expected_crc32c,
    const std::optional<std::string>& lock_token) {
  ServiceLock lock(mu_);
  nlohmann::json state;
  auto tr = require_txn_primary(txn_id, state);
  if (!tr.ok) return tr;
  if (state.value("state", "") != "open") return fail("conflict", "txn not open");

  auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");

  std::unordered_map<std::string, std::string> put_attrs = attrs;
  put_attrs["aios.txn"] = txn_id;

  ApiResult prep;
  {
    // Nested api_* / peer RPC run with mu_ fully released (see UnlockForRpc).
    UnlockForRpc unlock(mu_);
    if (placement.acting_set[0].node_id == cfg_.node_id) {
      prep = api_prepare_put(oid, data, len, put_attrs, true, preds, expected_crc32c, lock_token);
    } else {
      auto remote = object_prepare_put_remote(
          placement.acting_set[0].addr, cfg_.node_id, advertise_, cfg_.cluster_key,
          cfg_.auth_skew_ms, cur_epoch(), placement.acting_set[0].aios_path, oid, data, len,
          put_attrs, preds, lock_token);
      if (!remote.ok) {
        auto r = fail(remote.code.empty() ? "rpc_error" : remote.code, remote.error);
        r.placement = placement;
        return r;
      }
      prep.ok = true;
      prep.epoch = remote.epoch;
      prep.placement = placement;
      prep.info = ObjectInfo{};
      prep.info->oid = oid;
      prep.info->seq = remote.body.value("seq", static_cast<std::uint64_t>(0));
    }
  }
  if (!prep.ok) return prep;
  if (!prep.info || prep.info->seq == 0) return fail("store_error", "prepare missing seq");

  state["ops"].push_back({{"oid", oid},
                          {"seq", prep.info->seq},
                          {"kind", "put"},
                          {"primary", placement.acting_set[0].node_id},
                          {"addr", placement.acting_set[0].addr},
                          {"aios_path", placement.acting_set[0].aios_path}});
  auto saved = save_txn_state(txn_id, state);
  if (!saved.ok) {
    // Best-effort abort prepared version.
    UnlockForRpc unlock(mu_);
    if (placement.acting_set[0].node_id == cfg_.node_id) {
      api_abort_prepared(oid, prep.info->seq);
    } else {
      object_abort_prepared_remote(placement.acting_set[0].addr, cfg_.node_id, advertise_,
                                   cfg_.cluster_key, cfg_.auth_skew_ms, cur_epoch(),
                                   placement.acting_set[0].aios_path, oid, prep.info->seq);
    }
    return saved;
  }
  prep.attrs["txn_id"] = txn_id;
  return prep;
}

ApiResult ObjectService::api_txn_prepare_put_file(
    const std::string& txn_id, const std::string& oid, const std::string& staging_abs_path,
    std::uint64_t size, std::uint32_t crc32c_val,
    const std::unordered_map<std::string, std::string>& attrs,
    const std::vector<AttrPrecondition>& preds, std::optional<std::uint32_t> expected_crc32c,
    const std::optional<std::string>& lock_token) {
  ServiceLock lock(mu_);
  nlohmann::json state;
  auto tr = require_txn_primary(txn_id, state);
  if (!tr.ok) return tr;
  if (state.value("state", "") != "open") return fail("conflict", "txn not open");

  auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  // Large file prepare currently requires local primary (staging path is local).
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "large txn put requires oid primary as coordinator host");
    r.placement = placement;
    return r;
  }
  std::unordered_map<std::string, std::string> put_attrs = attrs;
  put_attrs["aios.txn"] = txn_id;
  ApiResult prep;
  {
    UnlockForRpc unlock(mu_);
    prep = api_prepare_put_file(oid, staging_abs_path, size, crc32c_val, put_attrs, true,
                                preds, expected_crc32c, lock_token);
  }
  if (!prep.ok) return prep;
  state["ops"].push_back({{"oid", oid},
                          {"seq", prep.info->seq},
                          {"kind", "put"},
                          {"primary", cfg_.node_id},
                          {"addr", advertise_},
                          {"aios_path", placement.acting_set[0].aios_path}});
  auto saved = save_txn_state(txn_id, state);
  if (!saved.ok) {
    UnlockForRpc unlock(mu_);
    api_abort_prepared(oid, prep.info->seq);
    return saved;
  }
  prep.attrs["txn_id"] = txn_id;
  return prep;
}

ApiResult ObjectService::api_txn_prepare_delete(
    const std::string& txn_id, const std::string& oid,
    const std::vector<AttrPrecondition>& preds, const std::optional<std::string>& lock_token) {
  ServiceLock lock(mu_);
  nlohmann::json state;
  auto tr = require_txn_primary(txn_id, state);
  if (!tr.ok) return tr;
  if (state.value("state", "") != "open") return fail("conflict", "txn not open");

  auto placement = place(oid, map_, cfg_.default_storage_class);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");

  ApiResult prep;
  {
    UnlockForRpc unlock(mu_);
    if (placement.acting_set[0].node_id == cfg_.node_id) {
      prep = api_prepare_delete(oid, preds, lock_token);
    } else {
      auto remote = object_prepare_delete_remote(
          placement.acting_set[0].addr, cfg_.node_id, advertise_, cfg_.cluster_key,
          cfg_.auth_skew_ms, cur_epoch(), placement.acting_set[0].aios_path, oid, preds,
          lock_token);
      if (!remote.ok) {
        return fail(remote.code.empty() ? "rpc_error" : remote.code, remote.error);
      }
      prep.ok = true;
      prep.epoch = remote.epoch;
      prep.placement = placement;
      prep.info = ObjectInfo{};
      prep.info->oid = oid;
      prep.info->seq = remote.body.value("seq", static_cast<std::uint64_t>(0));
      prep.info->is_delete = true;
    }
  }
  if (!prep.ok) return prep;
  state["ops"].push_back({{"oid", oid},
                          {"seq", prep.info->seq},
                          {"kind", "delete"},
                          {"primary", placement.acting_set[0].node_id},
                          {"addr", placement.acting_set[0].addr},
                          {"aios_path", placement.acting_set[0].aios_path}});
  auto saved = save_txn_state(txn_id, state);
  if (!saved.ok) {
    UnlockForRpc unlock(mu_);
    if (placement.acting_set[0].node_id == cfg_.node_id) {
      api_abort_prepared(oid, prep.info->seq);
    } else {
      object_abort_prepared_remote(placement.acting_set[0].addr, cfg_.node_id, advertise_,
                                   cfg_.cluster_key, cfg_.auth_skew_ms, cur_epoch(),
                                   placement.acting_set[0].aios_path, oid, prep.info->seq);
    }
    return saved;
  }
  return prep;
}

ApiResult ObjectService::api_txn_commit(const std::string& txn_id) {
  ServiceLock lock(mu_);
  nlohmann::json state;
  auto tr = require_txn_primary(txn_id, state);
  if (!tr.ok) return tr;
  if (state.value("state", "") != "open") return fail("conflict", "txn not open");
  state["state"] = "committing";
  auto mid = save_txn_state(txn_id, state);
  if (!mid.ok) return mid;

  std::vector<nlohmann::json> ops = state.value("ops", nlohmann::json::array());
  std::sort(ops.begin(), ops.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.value("oid", "") < b.value("oid", "");
  });

  std::vector<nlohmann::json> published;
  for (const auto& op : ops) {
    const auto oid = op.value("oid", "");
    const auto seq = op.value("seq", static_cast<std::uint64_t>(0));
    const auto primary = op.value("primary", "");
    const auto addr = op.value("addr", "");
    const auto aios_path = op.value("aios_path", "");
    bool ok = false;
    {
      // Publish/abort fan-out to primaries (local api_* or peer RPC) off mu_: two
      // coordinators committing cross-node txns otherwise wait on each other.
      UnlockForRpc unlock(mu_);
      std::string perr;
      if (primary == cfg_.node_id) {
        auto pr = api_publish_version(oid, seq);
        ok = pr.ok;
        perr = pr.code + ": " + pr.error;
      } else {
        auto pr = object_publish_prepared_remote(addr, cfg_.node_id, advertise_,
                                                 cfg_.cluster_key, cfg_.auth_skew_ms,
                                                 cur_epoch(), aios_path, oid, seq);
        ok = pr.ok;
        perr = pr.code + ": " + pr.error;
      }
      if (!ok) {
        AIOS_LOG_WARN("txn ", txn_id, " publish failed oid=", oid, " seq=", seq, " primary=",
                      primary, ": ", perr);
      }
    }
    if (!ok) {
      // Abort remaining (including failed) prepared versions.
      UnlockForRpc unlock(mu_);
      for (const auto& mop : ops) {
        bool already = false;
        for (const auto& p : published) {
          if (p.value("oid", "") == mop.value("oid", "")) {
            already = true;
            break;
          }
        }
        if (already) continue;
        const auto moid = mop.value("oid", "");
        const auto mseq = mop.value("seq", static_cast<std::uint64_t>(0));
        if (mop.value("primary", "") == cfg_.node_id) {
          api_abort_prepared(moid, mseq);
        } else {
          object_abort_prepared_remote(mop.value("addr", ""), cfg_.node_id, advertise_,
                                       cfg_.cluster_key, cfg_.auth_skew_ms, cur_epoch(),
                                       mop.value("aios_path", ""), moid, mseq);
        }
      }
      state["state"] = "aborted";
      state["error"] = "publish failed for " + oid;
      save_txn_state(txn_id, state);
      return fail("quorum_failed", "txn commit failed");
    }
    published.push_back(op);
  }

  state["state"] = "committed";
  state["ops"] = ops;
  auto saved = save_txn_state(txn_id, state);
  if (!saved.ok) return saved;
  const auto s = state.dump();
  saved.data = std::vector<std::uint8_t>(s.begin(), s.end());
  saved.attrs["txn_id"] = txn_id;
  return saved;
}

ApiResult ObjectService::api_txn_abort(const std::string& txn_id) {
  ServiceLock lock(mu_);
  nlohmann::json state;
  auto tr = require_txn_primary(txn_id, state);
  if (!tr.ok) return tr;
  const auto cur = state.value("state", "");
  if (cur == "committed") return fail("conflict", "txn already committed");
  {
    UnlockForRpc unlock(mu_);
    for (const auto& op : state.value("ops", nlohmann::json::array())) {
      const auto oid = op.value("oid", "");
      const auto seq = op.value("seq", static_cast<std::uint64_t>(0));
      if (op.value("primary", "") == cfg_.node_id) {
        api_abort_prepared(oid, seq);
      } else {
        object_abort_prepared_remote(op.value("addr", ""), cfg_.node_id, advertise_,
                                     cfg_.cluster_key, cfg_.auth_skew_ms, cur_epoch(),
                                     op.value("aios_path", ""), oid, seq);
      }
    }
  }
  state["state"] = "aborted";
  auto saved = save_txn_state(txn_id, state);
  if (!saved.ok) return saved;
  const auto s = state.dump();
  saved.data = std::vector<std::uint8_t>(s.begin(), s.end());
  return saved;
}

ApiResult ObjectService::api_lock_acquire(const std::string& oid, int ttl_ms) {
  ServiceLock lock(mu_);
  Placement placement;
  auto pr = require_primary(oid, placement);
  if (!pr.ok) return pr;
  std::string token;
  std::int64_t expires = 0;
  std::string err;
  if (!locks_.acquire(oid, ttl_ms, token, expires, err)) {
    return fail("lock_held", err);
  }
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  r.json_body = {{"oid", oid},
                 {"token", token},
                 {"expires_ms", expires},
                 {"epoch", map_.epoch},
                 {"primary", cfg_.node_id}};
  ops_.note_lock_acquire();
  return r;
}

ApiResult ObjectService::api_lock_renew(const std::string& oid, const std::string& token,
                                       int ttl_ms) {
  ServiceLock lock(mu_);
  Placement placement;
  auto pr = require_primary(oid, placement);
  if (!pr.ok) return pr;
  LockTable::Status st;
  std::string err;
  if (!locks_.renew(oid, token, ttl_ms, st, err)) {
    if (err.find("mismatch") != std::string::npos) return fail("lock_held", err);
    if (err.find("expired") != std::string::npos) return fail("lock_expired", err);
    return fail("not_found", err);
  }
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  r.json_body = {{"oid", oid},
                 {"token", token},
                 {"expires_ms", st.expires_ms},
                 {"break_requested", st.break_requested},
                 {"epoch", map_.epoch},
                 {"primary", cfg_.node_id}};
  return r;
}

ApiResult ObjectService::api_lock_break(const std::string& oid, int grace_ms) {
  ServiceLock lock(mu_);
  Placement placement;
  auto pr = require_primary(oid, placement);
  if (!pr.ok) return pr;
  LockTable::Status st;
  std::string err;
  if (!locks_.request_break(oid, grace_ms, st, err)) return fail("not_found", err);
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  r.json_body = {{"oid", oid}, {"held", true}, {"expires_ms", st.expires_ms},
                 {"break_requested", true}};
  return r;
}

ApiResult ObjectService::api_lock_release(const std::string& oid, const std::string& token) {
  ServiceLock lock(mu_);
  Placement placement;
  auto pr = require_primary(oid, placement);
  if (!pr.ok) return pr;
  std::string err;
  if (!locks_.release(oid, token, err)) {
    if (err.find("mismatch") != std::string::npos) return fail("lock_held", err);
    return fail("not_found", err);
  }
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  return r;
}

ApiResult ObjectService::api_lock_stat(const std::string& oid) {
  ServiceLock lock(mu_);
  Placement placement;
  auto pr = require_primary(oid, placement);
  if (!pr.ok) return pr;
  LockTable::Status st;
  if (!locks_.stat(oid, st)) return fail("not_found", "lock not held");
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  r.json_body = {{"oid", oid},
                 {"held", true},
                 {"expires_ms", st.expires_ms},
                 {"break_requested", st.break_requested}};
  return r;
}

ApiResult ObjectService::api_watch_oid(const std::string& oid, std::uint64_t after_seq,
                                      int timeout_ms) {
  Placement placement;
  {
    ServiceLock lock(mu_);
    auto pr = require_primary(oid, placement);
    if (!pr.ok) return pr;
    // Immediate event if tip already advanced past after_seq.
    std::string err;
    auto* store = primary_store(placement, err);
    if (store) {
      auto st = store->stat(oid, err);
      if (st && st->seq > after_seq) {
        ApiResult r;
        r.ok = true;
        r.epoch = cur_epoch();
        r.placement = placement;
        WatchEvent ev;
        ev.oid = oid;
        ev.seq = st->seq;
        ev.op = st->is_delete ? "del" : "put";
        ev.ts_ms = now_ms();
        r.watch_event = ev;
        ops_.note_watch();
        return r;
      }
      if (!st) after_seq = 0;  // wait for first create
    }
  }
  // Block outside ObjectService mutex so mutates can proceed and signal.
  WatchEvent ev;
  if (!watches_.wait_oid(oid, after_seq, timeout_ms, ev)) {
    ApiResult r;
    r.ok = true;
    r.code = "timeout";
    r.epoch = cur_epoch();
    r.placement = placement;
    return r;
  }
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  r.watch_event = ev;
  ops_.note_watch();
  return r;
}

ApiResult ObjectService::api_watch_prefix(const std::string& prefix, int timeout_ms) {
  if (prefix.empty()) return fail("bad_request", "prefix required");
  std::vector<WatchEvent> events;
  if (!watches_.wait_prefix(prefix, timeout_ms, events)) {
    ApiResult r;
    r.ok = true;
    r.code = "timeout";
    r.epoch = cur_epoch();
    return r;
  }
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.watch_events = std::move(events);
  return r;
}

ApiResult ObjectService::load_pubsub_meta(const std::string& topic, DeliveryMode& mode_out,
                                         std::uint64_t& next_id_out) {
  const std::string oid = pubsub_meta_oid(topic);
  auto gr = api_get(oid, std::nullopt, std::nullopt, {});
  if (!gr.ok) return gr;
  if (!gr.data || gr.data->empty()) return fail("bad_request", "empty pubsub meta");
  try {
    const auto j = nlohmann::json::parse(gr.data->begin(), gr.data->end());
    const auto mode_s = json_str(j, "delivery");
    auto mode = parse_delivery_mode(mode_s);
    if (!mode || *mode != DeliveryMode::Durable) {
      return fail("bad_request", "invalid durable pubsub meta");
    }
    mode_out = DeliveryMode::Durable;
    next_id_out = j.value("next_id", static_cast<std::uint64_t>(1));
    if (next_id_out == 0) next_id_out = 1;
    ApiResult ok;
    ok.ok = true;
    ok.epoch = cur_epoch();
    return ok;
  } catch (...) {
    return fail("bad_request", "invalid pubsub meta json");
  }
}

ApiResult ObjectService::save_pubsub_meta(const std::string& topic, DeliveryMode mode,
                                         std::uint64_t next_id) {
  const nlohmann::json j = {{"delivery", delivery_mode_name(mode)}, {"next_id", next_id}};
  const auto s = j.dump();
  const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
  return api_put(pubsub_meta_oid(topic), p, s.size(), {{"content-type", "application/json"}},
                 true, {});
}

ApiResult ObjectService::ensure_pubsub_topic(const std::string& topic,
                                             std::optional<DeliveryMode> mode,
                                             std::size_t capacity, DeliveryMode& mode_out) {
  auto ok_res = [&] {
    ApiResult r;
    r.ok = true;
    r.epoch = cur_epoch();
    return r;
  };

  TopicStat st;
  if (pubsub_.stat(topic, st)) {
    if (mode && *mode != st.delivery) {
      return fail("mode_mismatch", "topic delivery mode mismatch");
    }
    mode_out = st.delivery;
    return ok_res();
  }

  // Try durable meta on disk.
  DeliveryMode disk_mode = DeliveryMode::Durable;
  std::uint64_t next_id = 1;
  auto lr = load_pubsub_meta(topic, disk_mode, next_id);
  if (lr.ok) {
    if (mode && *mode != DeliveryMode::Durable) {
      return fail("mode_mismatch", "topic delivery mode mismatch");
    }
    std::string code, err;
    if (!pubsub_.ensure_durable(topic, next_id, code, err)) {
      return fail(code.empty() ? "conflict" : code, err);
    }
    mode_out = DeliveryMode::Durable;
    return ok_res();
  }
  if (lr.code != "not_found") return lr;

  const DeliveryMode use = mode.value_or(DeliveryMode::Buffered);
  std::string code, err;
  if (!pubsub_.create(topic, use, capacity, code, err)) {
    return fail(code.empty() ? "bad_request" : code, err);
  }
  if (use == DeliveryMode::Durable) {
    auto sr = save_pubsub_meta(topic, DeliveryMode::Durable, 1);
    if (!sr.ok) return sr;
  }
  mode_out = use;
  return ok_res();
}

ApiResult ObjectService::api_pubsub_create(const std::string& topic, DeliveryMode mode,
                                          std::size_t capacity) {
  if (topic.empty()) return fail("bad_request", "empty topic");
  Placement placement;
  {
    ServiceLock lock(mu_);
    auto pr = require_primary(pubsub_meta_oid(topic), placement);
    if (!pr.ok) return pr;
  }

  TopicStat existing;
  if (pubsub_.stat(topic, existing)) {
    if (existing.delivery != mode) {
      return fail("mode_mismatch", "topic delivery mode mismatch");
    }
  } else {
    DeliveryMode disk_mode = DeliveryMode::Durable;
    std::uint64_t next_id = 1;
    auto lr = load_pubsub_meta(topic, disk_mode, next_id);
    if (lr.ok) {
      if (mode != DeliveryMode::Durable) {
        return fail("mode_mismatch", "topic delivery mode mismatch");
      }
      std::string code, err;
      if (!pubsub_.ensure_durable(topic, next_id, code, err)) {
        return fail(code.empty() ? "conflict" : code, err);
      }
    } else if (lr.code == "not_found") {
      std::string code, err;
      if (!pubsub_.create(topic, mode, capacity, code, err)) {
        return fail(code.empty() ? "bad_request" : code, err);
      }
      if (mode == DeliveryMode::Durable) {
        auto sr = save_pubsub_meta(topic, DeliveryMode::Durable, 1);
        if (!sr.ok) return sr;
      }
    } else {
      return lr;
    }
  }

  TopicStat st;
  pubsub_.stat(topic, st);
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  r.json_body = {{"topic", topic},
                 {"delivery", delivery_mode_name(st.delivery)},
                 {"next_id", st.next_id},
                 {"buffered", st.buffered},
                 {"capacity", st.capacity}};
  return r;
}

ApiResult ObjectService::api_pubsub_stat(const std::string& topic) {
  if (topic.empty()) return fail("bad_request", "empty topic");
  Placement placement;
  {
    ServiceLock lock(mu_);
    auto pr = require_primary(pubsub_meta_oid(topic), placement);
    if (!pr.ok) return pr;
  }

  TopicStat st;
  if (!pubsub_.stat(topic, st)) {
    DeliveryMode disk_mode = DeliveryMode::Durable;
    std::uint64_t next_id = 1;
    auto lr = load_pubsub_meta(topic, disk_mode, next_id);
    if (!lr.ok) {
      if (lr.code == "not_found") return fail("not_found", "topic not found");
      return lr;
    }
    std::string code, err;
    pubsub_.ensure_durable(topic, next_id, code, err);
    pubsub_.stat(topic, st);
  }

  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  r.json_body = {{"topic", topic},
                 {"delivery", delivery_mode_name(st.delivery)},
                 {"next_id", st.next_id},
                 {"buffered", st.buffered},
                 {"capacity", st.capacity}};
  return r;
}

ApiResult ObjectService::api_pubsub_publish(const std::string& topic, const std::uint8_t* data,
                                           std::size_t len, const std::string& content_type,
                                           std::optional<DeliveryMode> mode,
                                           std::size_t capacity) {
  if (topic.empty()) return fail("bad_request", "empty topic");
  if (len > TopicHub::kMaxMessageBytes) {
    return fail("payload_too_large", "message exceeds 1 MiB");
  }

  Placement placement;
  {
    ServiceLock lock(mu_);
    auto pr = require_primary(pubsub_meta_oid(topic), placement);
    if (!pr.ok) return pr;
  }

  DeliveryMode mode_out = DeliveryMode::Buffered;
  auto er = ensure_pubsub_topic(topic, mode, capacity, mode_out);
  if (!er.ok) return er;

  PubMessage msg;
  if (mode_out == DeliveryMode::Durable) {
    std::string code, err;
    if (!pubsub_.reserve_durable(topic, data, len, content_type, msg, code, err)) {
      return fail(code.empty() ? "bad_request" : code, err);
    }
    // Persist message object + meta tip.
    std::unordered_map<std::string, std::string> attrs;
    if (!content_type.empty()) attrs["content-type"] = content_type;
    attrs["aios.pubsub.id"] = std::to_string(msg.id);
    attrs["aios.pubsub.ts_ms"] = std::to_string(msg.ts_ms);
    auto put_msg = api_put(pubsub_msg_oid(topic, msg.id), data, len, attrs, true, {});
    if (!put_msg.ok) return put_msg;
    TopicStat st;
    pubsub_.stat(topic, st);
    auto put_meta = save_pubsub_meta(topic, DeliveryMode::Durable, st.next_id);
    if (!put_meta.ok) return put_meta;
    pubsub_.commit_durable(topic, msg);
  } else {
    DeliveryMode published_mode = mode_out;
    std::string code, err;
    if (!pubsub_.publish_memory(topic, mode, capacity, data, len, content_type, msg,
                                published_mode, code, err)) {
      return fail(code.empty() ? "bad_request" : code, err);
    }
    mode_out = published_mode;
  }

  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  r.json_body = {{"topic", topic},
                 {"id", msg.id},
                 {"delivery", delivery_mode_name(mode_out)},
                 {"ts_ms", msg.ts_ms}};
  ops_.note_pubsub_publish();
  return r;
}

ApiResult ObjectService::api_pubsub_subscribe(const std::string& topic, std::uint64_t after_id,
                                             bool after_id_set, int timeout_ms) {
  if (topic.empty()) return fail("bad_request", "empty topic");
  Placement placement;
  {
    ServiceLock lock(mu_);
    auto pr = require_primary(pubsub_meta_oid(topic), placement);
    if (!pr.ok) return pr;
  }

  TopicStat st;
  bool known = pubsub_.stat(topic, st);
  if (!known) {
    DeliveryMode disk_mode = DeliveryMode::Durable;
    std::uint64_t next_id = 1;
    auto lr = load_pubsub_meta(topic, disk_mode, next_id);
    if (lr.ok) {
      std::string code, err;
      pubsub_.ensure_durable(topic, next_id, code, err);
      known = pubsub_.stat(topic, st);
    } else if (lr.code != "not_found") {
      return lr;
    }
  }
  if (!known) {
    // Allow waiting for first publish on this primary.
    if (!after_id_set) after_id = 0;
  } else if (!after_id_set) {
    after_id = st.next_id > 0 ? st.next_id - 1 : 0;
  }

  std::vector<PubMessage> messages;

  // Durable catch-up from object store, bounded per call: a subscriber starting at
  // after_id=0 on a long topic would otherwise load every message into memory.
  // The reply carries next_after_id so the client can page through the rest.
  constexpr std::uint64_t kPubsubCatchupMax = 1000;
  if (known && st.delivery == DeliveryMode::Durable) {
    std::uint64_t tip = st.next_id > 0 ? st.next_id - 1 : 0;
    bool truncated = false;
    if (tip > after_id && tip - after_id > kPubsubCatchupMax) {
      tip = after_id + kPubsubCatchupMax;
      truncated = true;
    }
    for (std::uint64_t id = after_id + 1; id <= tip; ++id) {
      auto gr = api_get(pubsub_msg_oid(topic, id), std::nullopt, std::nullopt, {});
      if (!gr.ok) {
        if (gr.code == "not_found") continue;
        return gr;
      }
      PubMessage m;
      m.id = id;
      m.ts_ms = 0;
      if (gr.attrs.count("aios.pubsub.ts_ms")) {
        try {
          m.ts_ms = std::stoll(gr.attrs.at("aios.pubsub.ts_ms"));
        } catch (...) {
        }
      }
      if (gr.attrs.count("content-type")) m.content_type = gr.attrs.at("content-type");
      if (gr.data) m.data = *gr.data;
      messages.push_back(std::move(m));
    }
    if (!messages.empty()) {
      ApiResult r;
      r.ok = true;
      r.epoch = cur_epoch();
      r.placement = placement;
      r.pub_messages = std::move(messages);
      r.json_body = {{"topic", topic}, {"next_after_id", tip}, {"more", truncated}};
      return r;
    }
  }

  // Buffered catch-up / wait (and ephemeral/durable wait for next).
  if (!pubsub_.subscribe(topic, after_id, timeout_ms, messages)) {
    ApiResult r;
    r.ok = true;
    r.code = "timeout";
    r.epoch = cur_epoch();
    r.placement = placement;
    return r;
  }

  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  r.pub_messages = std::move(messages);
  r.json_body = {{"topic", topic}};
  return r;
}

void ObjectService::gc_client_writes() {
  const auto now = now_ms();
  std::vector<std::shared_ptr<ClientWrite>> stale;
  {
    ServiceLock lock(mu_);
    for (auto it = client_writes_.begin(); it != client_writes_.end();) {
      if (now > it->second->expires_ms) {
        stale.push_back(it->second);
        it = client_writes_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& w : stale) {
    AIOS_LOG_WARN("expiring client write grant oid=", w->oid, " seq=", w->seq);
    std::string err;
    auto* store = primary_store(w->placement, err);
    if (store) store->abort_version(w->oid, w->seq, err);
    replicate_abort(w->placement, w->oid, w->seq);
  }
}

nlohmann::json ObjectService::client_prepare_json(const ClientWrite& w) const {
  nlohmann::json acting = nlohmann::json::array();
  for (std::size_t i = 0; i < w.placement.acting_set.size(); ++i) {
    const auto& t = w.placement.acting_set[i];
    acting.push_back({{"index", i},
                      {"node_id", t.node_id},
                      {"addr", t.addr},
                      {"http_addr", t.http_addr},
                      {"aios_path", t.aios_path}});
  }
  return {{"seq", w.seq},
          {"epoch", w.placement.epoch},
          {"grant", w.grant},
          {"expires_ms", w.expires_ms},
          {"layout", w.layout.is_ec() ? "ec" : "replica"},
          {"n", w.layout.n},
          {"ec_k", w.layout.ec_k},
          {"ec_m", w.layout.ec_m},
          {"ec_codec", w.layout.ec_codec},
          {"storage_class", w.layout.storage_class},
          {"full_size", w.full_size},
          {"full_crc", w.full_crc},
          {"acting_set", std::move(acting)}};
}

ApiResult ObjectService::verify_client_grant(const std::string& oid, const std::string& grant_blob,
                                             WriteGrant& g) {
  if (cfg_.io_path != "client") {
    return fail("not_supported", "client I/O path is disabled (io_path=server)");
  }
  std::string err;
  auto opened = open_write_grant(grant_blob, cfg_.cluster_key, now_ms(), err);
  if (!opened) return fail("bad_request", err);
  if (opened->oid != oid) return fail("bad_request", "grant oid mismatch");
  g = std::move(*opened);
  ApiResult ok;
  ok.ok = true;
  ok.epoch = cur_epoch();
  return ok;
}

int ObjectService::count_client_installs(const WriteGrant& g) {
  int ok = 0;
  for (const auto& t : g.acting_set) {
    if (t.node_id == cfg_.node_id) {
      auto* s = stores_.get(t.aios_path);
      std::string err;
      if (s && s->stat(g.oid, g.seq, err)) ++ok;
      continue;
    }
    UnlockForRpc unlock(mu_);
    auto st = object_stat_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                 cfg_.auth_skew_ms, g.epoch, t.aios_path, g.oid, false, g.seq);
    if (st.ok) ++ok;
  }
  return ok;
}

ApiResult ObjectService::api_client_prepare(
    const std::string& oid, std::uint64_t full_size, std::uint32_t full_crc,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    const std::vector<AttrPrecondition>& preds, const LayoutRequest& layout_req,
    const std::optional<std::string>& lock_token) {
  if (cfg_.io_path != "client") {
    return fail("not_supported", "client I/O path is disabled (io_path=server)");
  }
  gc_client_writes();
  ObjectLayout layout;
  std::string err;
  if (!resolve_object_layout(cfg_, oid, layout_req, layout, err)) {
    return fail("bad_request", err);
  }

  {
    ServiceLock lock(mu_);
    if (pipelines_.count(oid) || client_writes_.count(oid)) {
      return fail("conflict", "write already in progress for oid");
    }
  }

  auto w = std::make_shared<ClientWrite>();
  w->oid_guard =
      std::make_shared<MutatingOid>(mu_, mutating_mu_, mutating_cv_, mutating_oids_, oid);
  {
    ServiceLock lock(mu_);
    if (pipelines_.count(oid) || client_writes_.count(oid)) {
      return fail("conflict", "write already in progress for oid");
    }
    auto placement = place(oid, map_, layout.n, layout.storage_class);
    if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
    if (placement.acting_set[0].node_id != cfg_.node_id) {
      auto r = fail("not_primary", "this node is not primary for oid");
      r.placement = placement;
      return r;
    }
    if (auto g = primary_gate(oid, placement); !g.ok) return g;
    if (auto lk = enforce_lock(oid, lock_token); !lk.ok) return lk;
    auto* store = primary_store(placement, err);
    if (!store) return fail("store_error", err);
    {
      auto tip_attrs = store->list_attrs(oid, err);
      if (attrs_are_frozen(tip_attrs)) {
        return fail("frozen", "object is archived/frozen; recall before mutate");
      }
    }
    auto pr = check_preds_on(store, oid, preds, err);
    if (pr == PrecondResult::NotFound) return fail("not_found", err);
    if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

    std::uint64_t seq = 0;
    std::uint64_t tip = 0;
    if (!store->peek_next_seq(oid, seq, tip, err)) return fail("store_error", err);

    auto put_attrs = attrs;
    apply_layout_attrs(put_attrs, layout);

    WriteGrant grant;
    grant.oid = oid;
    grant.seq = seq;
    grant.epoch = placement.epoch;
    grant.layout = layout.is_ec() ? "ec" : "replica";
    grant.n = layout.n;
    grant.ec_k = layout.ec_k;
    grant.ec_m = layout.ec_m;
    grant.ec_codec = layout.ec_codec;
    grant.storage_class = layout.storage_class;
    grant.full_size = full_size;
    grant.full_crc = full_crc;
    grant.expires_ms = now_ms() + cfg_.io_path_grant_ttl_ms;
    grant.acting_set = placement.acting_set;
    w->grant = seal_write_grant(grant, cfg_.cluster_key, err);
    if (w->grant.empty()) return fail("store_error", err);

    w->oid = oid;
    w->seq = seq;
    w->placement = std::move(placement);
    w->layout = std::move(layout);
    w->attrs = std::move(put_attrs);
    w->full_size = full_size;
    w->full_crc = full_crc;
    w->expires_ms = grant.expires_ms;
    (void)replace_attrs;
    client_writes_[oid] = w;
  }

  ApiResult r;
  r.ok = true;
  r.epoch = w->placement.epoch;
  r.placement = w->placement;
  r.json_body = client_prepare_json(*w);
  r.info = ObjectInfo{};
  r.info->oid = oid;
  r.info->seq = w->seq;
  r.info->size = full_size;
  r.info->crc32c = full_crc;
  r.info->crc32c_known = true;
  return r;
}

ApiResult ObjectService::api_client_install(
    const std::string& oid, int shard, const std::string& grant_blob, const std::uint8_t* data,
    std::size_t len, const std::unordered_map<std::string, std::string>& attrs,
    std::optional<std::uint32_t> expected_crc32c, const std::string& abs_body_path) {
  WriteGrant g;
  if (auto v = verify_client_grant(oid, grant_blob, g); !v.ok) return v;
  if (shard < 0 || shard >= static_cast<int>(g.acting_set.size())) {
    return fail("bad_request", "shard index out of acting set");
  }
  const auto& t = g.acting_set[static_cast<std::size_t>(shard)];
  if (t.node_id != cfg_.node_id) {
    Placement p;
    p.epoch = g.epoch;
    p.storage_class = g.storage_class;
    p.acting_set = g.acting_set;
    if (shard != 0) std::swap(p.acting_set[0], p.acting_set[static_cast<std::size_t>(shard)]);
    auto r = fail("not_primary", "this node does not store this shard");
    r.placement = std::move(p);
    return r;
  }

  const bool use_file = !abs_body_path.empty();
  std::uint32_t body_crc = 0;
  if (use_file) {
    if (!expected_crc32c) return fail("bad_request", "crc32c required for staged install");
    body_crc = *expected_crc32c;
  } else {
    body_crc = crc32c(data, len);
    if (expected_crc32c && *expected_crc32c != body_crc) {
      return fail("crc_mismatch", "crc32c mismatch");
    }
  }

  std::size_t expect_len = static_cast<std::size_t>(g.full_size);
  if (g.is_ec()) {
    const auto k = static_cast<std::size_t>(std::max(1, g.ec_k));
    expect_len = g.full_size == 0 ? 0 : (static_cast<std::size_t>(g.full_size) + k - 1) / k;
  } else if (body_crc != g.full_crc) {
    return fail("crc_mismatch", "replica crc32c does not match grant");
  }
  if (len != expect_len) {
    return fail("bad_request", "install size does not match grant");
  }

  auto merged = attrs;
  ObjectLayout layout;
  layout.kind = g.is_ec() ? ObjectLayout::Kind::Ec : ObjectLayout::Kind::Replica;
  layout.n = g.n;
  layout.ec_k = g.ec_k;
  layout.ec_m = g.ec_m;
  layout.ec_codec = g.ec_codec;
  layout.storage_class = g.storage_class;
  apply_layout_attrs(merged, layout);
  if (g.is_ec()) {
    set_ec_attrs(merged, g.ec_k, g.ec_m, shard, g.ec_codec, g.full_size, g.full_crc);
  }

  PreparedVersion pv;
  pv.oid = oid;
  pv.seq = g.seq;
  pv.size = len;
  pv.crc32c = body_crc;
  pv.crc_verified = true;
  pv.inline_body = !use_file && len <= 64 * 1024;

  std::string err;
  bool done = false;
  if (use_file) {
    done = local_install_file(t.aios_path, pv, abs_body_path, merged, err);
  } else {
    done = local_install(t.aios_path, pv, data, len, merged, err);
  }
  if (!done) {
    if (err == "version already exists") return fail("conflict", err);
    return fail("store_error", err);
  }

  ApiResult r;
  r.ok = true;
  r.epoch = g.epoch;
  r.replicas = 1;
  r.info = ObjectInfo{};
  r.info->oid = oid;
  r.info->seq = g.seq;
  r.info->size = g.is_ec() ? g.full_size : len;
  r.info->crc32c = g.full_crc;
  r.info->crc32c_known = true;
  r.json_body = {{"seq", g.seq}, {"shard", shard}, {"epoch", g.epoch}};
  return r;
}

ApiResult ObjectService::api_client_publish(const std::string& oid, const std::string& grant_blob) {
  WriteGrant g;
  if (auto v = verify_client_grant(oid, grant_blob, g); !v.ok) return v;

  Placement placement;
  placement.epoch = g.epoch;
  placement.storage_class = g.storage_class;
  placement.acting_set = g.acting_set;
  if (placement.acting_set.empty() || placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  if (gate_.consensus && g.epoch < map_.epoch) {
    // The grant was minted under a map that has since been superseded; the
    // acting set it names may no longer be the one that must hold the copies.
    return fail("epoch_mismatch", "write grant is from an older cluster map epoch");
  }
  if (auto gr = primary_gate(oid, placement); !gr.ok) return gr;

  const int installed = count_client_installs(g);
  const int need = g.is_ec() ? std::max(g.ec_k, quorum_need(placement)) : quorum_need(placement);
  if (installed < need) {
    return fail("quorum_failed", "client install quorum failed: installed " +
                                     std::to_string(installed) + " of " +
                                     std::to_string(g.n) + ", need " + std::to_string(need));
  }

  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  if (!store->stat(oid, g.seq, err)) {
    return fail("not_found", "primary shard not installed");
  }
  if (!store->publish_tip(oid, g.seq, err)) {
    return fail("store_error", err);
  }
  replicate_publish(placement, oid, g.seq);
  signal_watch(oid, g.seq, "put");
  ops_.note_put(g.full_size);

  {
    ServiceLock lock(mu_);
    client_writes_.erase(oid);
  }

  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.replicas = installed;
  r.placement = placement;
  r.info = ObjectInfo{};
  r.info->oid = oid;
  r.info->seq = g.seq;
  r.info->size = g.full_size;
  r.info->crc32c = g.full_crc;
  r.info->crc32c_known = true;
  r.json_body = {{"seq", g.seq}, {"replicas", installed}, {"epoch", r.epoch}};
  return r;
}

ApiResult ObjectService::api_client_abort(const std::string& oid, const std::string& grant_blob) {
  if (cfg_.io_path != "client") {
    return fail("not_supported", "client I/O path is disabled (io_path=server)");
  }
  std::string err;
  auto opened = open_write_grant(grant_blob, cfg_.cluster_key, now_ms(), err, /*allow_expired=*/true);
  if (!opened || opened->oid != oid) {
    return fail("bad_request", err.empty() ? "grant oid mismatch" : err);
  }
  WriteGrant g = std::move(*opened);

  Placement placement;
  placement.epoch = g.epoch;
  placement.storage_class = g.storage_class;
  placement.acting_set = g.acting_set;
  if (!placement.acting_set.empty() && placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }

  auto* store = primary_store(placement, err);
  if (store) store->abort_version(oid, g.seq, err);
  replicate_abort(placement, oid, g.seq);
  {
    ServiceLock lock(mu_);
    client_writes_.erase(oid);
  }
  ApiResult r;
  r.ok = true;
  r.epoch = cur_epoch();
  r.placement = placement;
  r.json_body = {{"seq", g.seq}, {"epoch", r.epoch}};
  return r;
}

}  // namespace aios

