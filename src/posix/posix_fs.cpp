#include "posix/aios_posix.h"
#include "posix/posix_internal.hpp"
#include "posix/posix_layout.hpp"

#include "client/changelog.hpp"
#include "metrics/frontend_io.hpp"
#include "util/base64.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <exception>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aios {
namespace posix {
uint64_t cas_from_attrs(const std::unordered_map<std::string, std::string>& attrs) {
  auto it = attrs.find(kCasAttr);
  if (it == attrs.end()) return 0;
  try {
    return static_cast<uint64_t>(std::stoull(it->second));
  } catch (...) {
    return 0;
  }
}

int validate_dentry_name(const char* name) {
  if (!name || !*name) return -EINVAL;
  if (std::strchr(name, '/')) return -EINVAL;
  if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) return -EINVAL;
  if (std::strlen(name) > 255) return -ENAMETOOLONG;
  return 0;
}

// Caller may hold st.mu. Takes only dirty_chunk_mu (never chunk_lock).
void drop_dirty_chunks(FsState& st, uint64_t ino, std::optional<uint64_t> min_chunk = std::nullopt) {
  std::lock_guard lock(st.dirty_chunk_mu);
  auto it = st.dirty_chunks.find(ino);
  if (it == st.dirty_chunks.end()) return;
  if (!min_chunk) {
    for (const auto& [c, d] : it->second) {
      (void)c;
      st.dirty_chunk_bytes -= d.accounted;
    }
    st.dirty_chunks.erase(it);
    return;
  }
  for (auto cit = it->second.begin(); cit != it->second.end();) {
    if (cit->first >= *min_chunk) {
      st.dirty_chunk_bytes -= cit->second.accounted;
      cit = it->second.erase(cit);
    } else {
      ++cit;
    }
  }
  if (it->second.empty()) st.dirty_chunks.erase(it);
}

bool has_dirty_chunk(FsState& st, uint64_t ino, uint64_t chunk) {
  std::lock_guard lock(st.dirty_chunk_mu);
  auto it = st.dirty_chunks.find(ino);
  if (it == st.dirty_chunks.end()) return false;
  return it->second.find(chunk) != it->second.end();
}

// Copies the overlapping dirty range into dst (which the caller zeroed). True if
// this mount has an unpublished body for the stripe, even when the range is past
// the dirty length (holes stay zero).
bool copy_dirty_range(FsState& st, uint64_t ino, uint64_t chunk, uint64_t chunk_off, uint8_t* dst,
                      size_t n) {
  std::lock_guard lock(st.dirty_chunk_mu);
  auto it = st.dirty_chunks.find(ino);
  if (it == st.dirty_chunks.end()) return false;
  auto cit = it->second.find(chunk);
  if (cit == it->second.end()) return false;
  const auto& body = cit->second.body;
  if (chunk_off < body.size()) {
    const size_t avail = static_cast<size_t>(body.size() - chunk_off);
    const size_t take = std::min(n, avail);
    std::memcpy(dst, body.data() + chunk_off, take);
  }
  return true;
}

void account_dirty_locked(FsState& st, DirtyChunk& d) {
  st.dirty_chunk_bytes -= d.accounted;
  d.accounted = d.body.size();
  st.dirty_chunk_bytes += d.accounted;
}

void resize_dirty_chunk(FsState& st, uint64_t ino, uint64_t chunk, size_t keep) {
  std::lock_guard chunk_guard(st.chunk_lock(ino, chunk));
  std::lock_guard lock(st.dirty_chunk_mu);
  auto it = st.dirty_chunks.find(ino);
  if (it == st.dirty_chunks.end()) return;
  auto cit = it->second.find(chunk);
  if (cit == it->second.end()) return;
  if (cit->second.body.size() > keep) cit->second.body.resize(keep);
  account_dirty_locked(st, cit->second);
  cit->second.gen++;
}

void flush_one_dirty_chunk(FsState& st, uint64_t ino, uint64_t chunk) {
  std::lock_guard chunk_guard(st.chunk_lock(ino, chunk));
  std::string body;
  uint64_t cas = 0;
  uint64_t gen = 0;
  {
    std::lock_guard dlock(st.dirty_chunk_mu);
    auto it = st.dirty_chunks.find(ino);
    if (it == st.dirty_chunks.end()) return;
    auto cit = it->second.find(chunk);
    if (cit == it->second.end()) return;
    body = cit->second.body;
    cas = cit->second.cas;
    gen = cit->second.gen;
  }
  const auto layout = data_layout_for_ino(st, ino);
  const std::string oid = chunk_oid(st.volume, ino, chunk);
  for (int attempt = 0; attempt < kChunkWriteRetries; ++attempt) {
    if (attempt > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1 + (attempt % 4)));
    }
    try {
      const uint64_t new_cas =
          st.session.put_bytes(oid, body, {}, cas, std::nullopt, layout);
      bool published = false;
      {
        std::lock_guard dlock(st.dirty_chunk_mu);
        auto it = st.dirty_chunks.find(ino);
        if (it != st.dirty_chunks.end()) {
          auto cit = it->second.find(chunk);
          if (cit != it->second.end() && cit->second.gen == gen) {
            st.dirty_chunk_bytes -= cit->second.accounted;
            it->second.erase(cit);
            if (it->second.empty()) st.dirty_chunks.erase(it);
            published = true;
          } else if (cit != it->second.end()) {
            cit->second.cas = new_cas;
          }
        }
      }
      if (published) st.chunk_cache.store(ino, chunk, std::move(body), new_cas);
      return;
    } catch (const client_error& e) {
      if (e.code() != "conflict") throw;
      auto existing = st.session.get_object(oid);
      cas = existing.exists ? cas_from_attrs(existing.attrs) : 0;
    }
  }
  throw client_error("conflict", "chunk flush failed");
}

void flush_dirty_chunks(FsState& st, uint64_t ino) {
  std::vector<uint64_t> chunks;
  {
    std::lock_guard lock(st.dirty_chunk_mu);
    auto it = st.dirty_chunks.find(ino);
    if (it == st.dirty_chunks.end()) return;
    chunks.reserve(it->second.size());
    for (const auto& [c, d] : it->second) {
      (void)d;
      chunks.push_back(c);
    }
  }
  for (uint64_t chunk : chunks) flush_one_dirty_chunk(st, ino, chunk);
}

bool verify_dir_link(FsState& st, uint64_t parent, const char* name, uint64_t expected_ino) {
  DirTable verify = make_dir(st, parent);
  verify.load(false);
  auto it = verify.entries().find(name);
  return it != verify.entries().end() && it->second == expected_ino;
}

void delete_orphan_inode(FsState& st, uint64_t ino) {
  try {
    st.session.delete_object(ino_oid(st.volume, ino));
  } catch (...) {
  }
  drop_dirty_chunks(st, ino);
  std::lock_guard lock(st.mu);
  st.inode_cache.erase(ino);
  st.dirty_sizes.erase(ino);
  st.chunk_cache.drop(ino);
}

namespace {

void delete_file_chunks(FsState& st, uint64_t ino, uint64_t size, uint64_t stripe_unit,
                        uint32_t project_id, uint32_t uid, uint32_t gid) {
  const uint64_t unit = stripe_unit ? stripe_unit : st.stripe_unit;
  const uint64_t nchunk = size == 0 ? 0 : (size + unit - 1) / unit;
  drop_dirty_chunks(st, ino);
  st.chunk_cache.drop(ino);
  for (uint64_t c = 0; c < nchunk; ++c) {
    try {
      st.session.delete_object(chunk_oid(st.volume, ino, c));
    } catch (...) {
    }
  }
  if (st.quota && size > 0) {
    st.quota->note_delta(project_id, uid, gid, -static_cast<std::int64_t>(size));
  }
}

#define AIOS_POSIX_CATCH_ALL \
  catch (const std::exception&) { \
    return -EIO; \
  } catch (...) { \
    return -EIO; \
  }

}  // namespace

std::string super_oid(const std::string& vol) { return "posix/" + vol + "/super"; }
std::string ino_oid(const std::string& vol, uint64_t ino) {
  return "posix/" + vol + "/ino/" + std::to_string(ino);
}
std::string dir_meta_oid(const std::string& vol, uint64_t ino) {
  return "posix/" + vol + "/dir/" + std::to_string(ino) + "/meta";
}
std::string dir_log_oid(const std::string& vol, uint64_t ino) {
  return "posix/" + vol + "/dir/" + std::to_string(ino) + "/log";
}
std::string dir_snap_oid(const std::string& vol, uint64_t ino) {
  return "posix/" + vol + "/dir/" + std::to_string(ino) + "/snap";
}
std::string chunk_oid(const std::string& vol, uint64_t ino, uint64_t chunk) {
  return "posix/" + vol + "/data/" + std::to_string(ino) + "/c/" + std::to_string(chunk);
}

uint64_t now_ns() {
  using clock = std::chrono::system_clock;
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch())
          .count());
}

int map_error(const client_error& e) {
  if (e.code() == "not_found") return -ENOENT;
  if (e.code() == "conflict") return -EAGAIN;
  if (e.code() == "lock_held") return -EAGAIN;
  if (e.code() == "bad_request") return -EINVAL;
  if (e.code() == "payload_too_large") return -EFBIG;
  if (e.code() == "no_targets") return -ENOSPC;
  AIOS_LOG_DEBUG("posix: client error mapped to EIO code=", e.code(), " what=", e.what());
  return -EIO;
}

constexpr size_t kMaxXattrName = 255;
constexpr size_t kMaxXattrValue = 64 * 1024;
constexpr size_t kMaxXattrCount = 128;
constexpr int kFlockTtlMs = 120000;
#ifdef __APPLE__
constexpr int kXattrMissing = ENOATTR;
#else
constexpr int kXattrMissing = ENODATA;
#endif

bool valid_xattr_name(const char* name) {
  if (!name || !*name) return false;
  return std::strlen(name) <= kMaxXattrName;
}

void fill_stat(const InodeMeta& m, aios_posix_stat* st) {
  if (!st) return;
  std::memset(st, 0, sizeof(*st));
  st->ino = m.ino;
  st->mode = m.mode;
  st->nlink = m.nlink;
  st->uid = m.uid;
  st->gid = m.gid;
  st->size = m.size;
  st->atime_ns = m.atime_ns;
  st->mtime_ns = m.mtime_ns;
  st->ctime_ns = m.ctime_ns;
  st->stripe_unit = m.stripe_unit;
  st->stripe_width = m.stripe_width;
  st->parent_ino = m.parent_ino;
}

int check_access(const aios_posix_cred& cred, const InodeMeta& m, int want) {
  if (want == 0) return 0;
  if (cred.uid == 0) return 0;  // root
  const unsigned mode = m.mode & 0777;
  unsigned bits = 0;
  if (cred.uid == m.uid)
    bits = (mode >> 6) & 7u;
  else if (cred.gid == m.gid)
    bits = (mode >> 3) & 7u;
  else
    bits = mode & 7u;
  if ((static_cast<int>(bits) & want) == want) return 0;
  return -EACCES;
}

int check_sticky_unlink(const aios_posix_cred& cred, const InodeMeta& parent,
                        const InodeMeta& victim) {
  if (cred.uid == 0) return 0;
  if ((parent.mode & S_ISVTX) == 0) return 0;
  if (cred.uid == parent.uid || cred.uid == victim.uid) return 0;
  return -EACCES;
}

InodeMeta inode_from_json(const std::string& body, uint64_t cas_hint) {
  auto j = nlohmann::json::parse(body);
  InodeMeta m;
  m.exists = true;
  m.ino = j.value("ino", static_cast<uint64_t>(0));
  m.mode = j.value("mode", static_cast<uint32_t>(0));
  m.nlink = j.value("nlink", static_cast<uint32_t>(1));
  m.uid = j.value("uid", static_cast<uint32_t>(0));
  m.gid = j.value("gid", static_cast<uint32_t>(0));
  m.project_id = j.value("project_id", static_cast<uint32_t>(0));
  m.parent_ino = j.value("parent_ino", static_cast<uint64_t>(0));
  m.size = j.value("size", static_cast<uint64_t>(0));
  m.atime_ns = j.value("atime_ns", static_cast<uint64_t>(0));
  m.mtime_ns = j.value("mtime_ns", static_cast<uint64_t>(0));
  m.ctime_ns = j.value("ctime_ns", static_cast<uint64_t>(0));
  m.stripe_unit = j.value("stripe_unit", kDefaultStripeUnit);
  m.stripe_width = j.value("stripe_width", kDefaultStripeWidth);
  m.rbytes = j.value("rbytes", static_cast<uint64_t>(0));
  m.rfiles = j.value("rfiles", static_cast<uint64_t>(0));
  m.rdirs = j.value("rdirs", static_cast<uint64_t>(0));
  m.rtime_ns = j.value("rtime_ns", static_cast<uint64_t>(0));
  m.cas = cas_hint;
  m.symlink = j.value("symlink", std::string{});
  if (j.contains("xattrs") && j["xattrs"].is_object()) {
    for (auto it = j["xattrs"].begin(); it != j["xattrs"].end(); ++it) {
      if (!it.value().is_string()) continue;
      std::vector<uint8_t> raw;
      std::string err;
      if (!base64_decode(it.value().get<std::string>(), raw, err)) continue;
      m.xattrs[it.key()] = std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
    }
  }
  return m;
}

std::string inode_to_json(const InodeMeta& m) {
  nlohmann::json j{{"aios_posix_ino", 1},
                   {"ino", m.ino},
                   {"mode", m.mode},
                   {"nlink", m.nlink},
                   {"uid", m.uid},
                   {"gid", m.gid},
                   {"project_id", m.project_id},
                   {"parent_ino", m.parent_ino},
                   {"size", m.size},
                   {"atime_ns", m.atime_ns},
                   {"mtime_ns", m.mtime_ns},
                   {"ctime_ns", m.ctime_ns},
                   {"stripe_unit", m.stripe_unit},
                   {"stripe_width", m.stripe_width},
                   {"rbytes", m.rbytes},
                   {"rfiles", m.rfiles},
                   {"rdirs", m.rdirs},
                   {"rtime_ns", m.rtime_ns}};
  if (!m.symlink.empty()) j["symlink"] = m.symlink;
  if (!m.xattrs.empty()) {
    nlohmann::json xa = nlohmann::json::object();
    for (const auto& [k, v] : m.xattrs) {
      xa[k] = base64_encode(reinterpret_cast<const uint8_t*>(v.data()), v.size());
    }
    j["xattrs"] = std::move(xa);
  }
  return j.dump();
}

SuperMeta super_from_json(const std::string& body, uint64_t cas_hint) {
  auto j = nlohmann::json::parse(body);
  SuperMeta m;
  m.exists = true;
  m.next_ino = j.value("next_ino", static_cast<uint64_t>(2));
  m.stripe_unit = j.value("stripe_unit", kDefaultStripeUnit);
  m.stripe_width = j.value("stripe_width", kDefaultStripeWidth);
  m.uuid = j.value("uuid", "");
  m.frozen = j.value("frozen", false);
  m.cas = cas_hint;
  return m;
}

std::string super_to_json(const SuperMeta& m) {
  return nlohmann::json{{"aios_posix_super", 1},
                        {"next_ino", m.next_ino},
                        {"stripe_unit", m.stripe_unit},
                        {"stripe_width", m.stripe_width},
                        {"uuid", m.uuid},
                        {"frozen", m.frozen}}
      .dump();
}

void HeldLocks::release_all() {
  if (!session) return;
  for (auto it = held.rbegin(); it != held.rend(); ++it) {
    try {
      session->lock_release(it->first, it->second);
    } catch (...) {
    }
  }
  held.clear();
}

HeldLocks::~HeldLocks() { release_all(); }

void HeldLocks::acquire_sorted(std::vector<std::string> oids, int ttl_ms) {
  std::sort(oids.begin(), oids.end());
  oids.erase(std::unique(oids.begin(), oids.end()), oids.end());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kLeaseWaitMs);
  bool broke = false;
  int sleep_ms = 2;
  for (;;) {
    release_all();
    bool ok = true;
    std::string blocked;
    for (const auto& oid : oids) {
      std::string token;
      if (!session->lock_try_acquire(oid, token, ttl_ms)) {
        ok = false;
        blocked = oid;
        break;
      }
      held.emplace_back(oid, std::move(token));
    }
    if (ok) return;
    release_all();
    if (std::chrono::steady_clock::now() >= deadline) {
      throw client_error("lock_held", "lease not returned in time: " + blocked);
    }
    // A holder that batches work under the lease releases early once it sees
    // the break; a dead one loses the lease at the grace deadline.
    if (!broke) {
      try {
        session->lock_break(blocked);
      } catch (const client_error&) {
      }
      broke = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    sleep_ms = std::min(sleep_ms * 2, 200);
  }
}

std::optional<std::string> HeldLocks::token_for(const std::string& oid) const {
  for (const auto& [o, t] : held) {
    if (o == oid) return t;
  }
  return std::nullopt;
}

void txn_put_dir(Session& session, const std::string& txn_id, DirTable& dir,
                 const HeldLocks& locks) {
  std::string meta, snap, log;
  dir.plan_compact_bodies(meta, snap, log);
  session.txn_prepare_put(txn_id, dir.snap_oid(), snap, std::nullopt,
                          locks.token_for(dir.snap_oid()));
  session.txn_prepare_put(txn_id, dir.log_oid(), log, std::nullopt,
                          locks.token_for(dir.log_oid()));
  session.txn_prepare_put(txn_id, dir.meta_oid(), meta, dir.meta_cas(),
                          locks.token_for(dir.meta_oid()));
}

DirTable::DirTable(Session& session, std::string vol, uint64_t ino, FsState* cache)
    : session_(session),
      cache_(cache),
      vol_(std::move(vol)),
      ino_(ino),
      meta_oid_(dir_meta_oid(vol_, ino_)),
      log_oid_(dir_log_oid(vol_, ino_)),
      snap_oid_(dir_snap_oid(vol_, ino_)) {}

void DirTable::apply_record(uint64_t /*op_id*/, uint32_t op, const std::vector<std::string>& args) {
  if (op == kOpLink && args.size() >= 2) {
    entries_[args[0]] = static_cast<uint64_t>(std::stoull(args[1]));
  } else if (op == kOpUnlink && args.size() >= 1) {
    entries_.erase(args[0]);
  } else if (op == kOpRename && args.size() >= 2) {
    auto it = entries_.find(args[0]);
    if (it != entries_.end()) {
      const uint64_t child = it->second;
      entries_.erase(it);
      entries_[args[1]] = child;
    }
  }
}

void DirTable::publish_cache() {
  if (!cache_) return;
  DirCacheEnt e;
  e.entries = entries_;
  e.meta_cas = meta_cas_;
  e.next_op = next_op_;
  e.log_bytes = log_bytes_;
  e.snapshot_op = snapshot_op_;
  e.loaded = std::chrono::steady_clock::now();
  std::lock_guard lock(cache_->mu);
  cache_->dir_cache[ino_] = std::move(e);
  dir_cache_evict_locked(*cache_);
}

bool DirTable::lease_commit(uint32_t op, std::vector<std::string> args, bool must_be_absent,
                            uint64_t expected_ino, int& rc) {
  if (!cache_ || !cache_->leases) return false;
  auto l = cache_->leases->get(ino_, put_layout_);
  if (!l) {
    // A lease we hold but cannot use (break requested, expiring) is handed back
    // so the synchronous protocol below can take the lock itself.
    cache_->leases->drop(ino_);
    return false;
  }
  rc = cache_->leases->queue(*l, op, std::move(args), must_be_absent, expected_ino);
  if (rc == -EAGAIN) {
    cache_->leases->drop(ino_);
    return false;
  }
  load(true);
  return true;
}

void DirTable::load(bool allow_cache) {
  if (cache_ && cache_->leases) {
    // Under our lease (or while a lost lease still has records queued) the
    // lease's table is the truth; the server tip alone would be behind it.
    if (auto l = cache_->leases->authoritative(ino_)) {
      std::lock_guard lock(l->mu);
      entries_ = l->entries;
      meta_cas_ = l->meta_cas;
      next_op_ = l->next_op;
      log_bytes_ = l->log_bytes;
      snapshot_op_ = l->snapshot_op;
      return;
    }
  }
  if (allow_cache && cache_) {
    std::lock_guard lock(cache_->mu);
    auto it = cache_->dir_cache.find(ino_);
    if (it != cache_->dir_cache.end() &&
        std::chrono::steady_clock::now() - it->second.loaded < kDirCacheTtl) {
      entries_ = it->second.entries;
      meta_cas_ = it->second.meta_cas;
      next_op_ = it->second.next_op;
      log_bytes_ = it->second.log_bytes;
      snapshot_op_ = it->second.snapshot_op;
      return;
    }
  }

  entries_.clear();
  next_op_ = 1;
  log_bytes_ = 0;
  snapshot_op_ = 0;
  meta_cas_ = 0;

  auto meta = session_.get_object(meta_oid_);
  if (!meta.exists) return;
  meta_cas_ = cas_from_attrs(meta.attrs);
  auto j = nlohmann::json::parse(meta.body);
  next_op_ = j.value("next_op", static_cast<uint64_t>(1));
  log_bytes_ = j.value("log_bytes", static_cast<uint64_t>(0));
  snapshot_op_ = j.value("snapshot_op", static_cast<uint64_t>(0));

  if (snapshot_op_ > 0) {
    auto snap = session_.get_object(snap_oid_);
    if (snap.exists && !snap.body.empty()) {
      auto sj = nlohmann::json::parse(snap.body);
      if (sj.contains("entries") && sj["entries"].is_object()) {
        for (auto it = sj["entries"].begin(); it != sj["entries"].end(); ++it) {
          entries_[it.key()] = it.value().get<uint64_t>();
        }
      }
    }
  }

  if (log_bytes_ == 0) return;
  auto log = session_.get_range(log_oid_, 0, log_bytes_ - 1);
  if (!log.exists || log.body.empty()) return;
  std::vector<changelog::Record> recs;
  changelog::decode_records(log.body, recs);
  for (const auto& r : recs) {
    if (r.op_id <= snapshot_op_) continue;
    apply_record(r.op_id, static_cast<uint32_t>(r.op), r.args);
  }
  if (allow_cache) publish_cache();
}

void DirTable::store_meta() {
  nlohmann::json j{{"aios_posix_dir", 1},
                   {"next_op", next_op_},
                   {"log_bytes", log_bytes_},
                   {"snapshot_op", snapshot_op_},
                   {"snapshot_oid", snap_oid_}};
  meta_cas_ = session_.put_bytes(meta_oid_, j.dump(), {}, meta_cas_, std::nullopt, put_layout_);
}

void DirTable::append_ops(const std::vector<std::pair<uint32_t, std::vector<std::string>>>& ops) {
  if (ops.empty()) return;
  if (ops.size() == 1) {
    int rc = 0;
    if (lease_commit(ops[0].first, ops[0].second, false, 0, rc)) {
      if (rc == -ENOENT || rc == 0) return;  // unlink/rename of a vanished name: no-op
      throw client_error("conflict", "dir op refused under lease");
    }
  }
  // Reserve op ids via CAS on meta. Against a lease holder (a kernel client with
  // a directory delegation) the append is refused with lock_held; ask for the
  // lease back once and wait it out instead of burning the attempts.
  bool broke = false;
  int lease_sleep_ms = 5;
  for (int attempt = 0; attempt < 24; ++attempt) {
    load(false);
    const uint64_t start = next_op_;
    std::string batch;
    uint64_t op = start;
    for (const auto& [code, args] : ops) {
      changelog::Record r;
      r.op_id = op++;
      r.op = static_cast<changelog::Op>(code);
      r.args = args;
      batch += changelog::encode_record(r);
    }
    try {
      next_op_ = op;
      auto ar = session_.append(log_oid_, batch);
      log_bytes_ = ar.size;
      store_meta();
      for (const auto& [code, args] : ops) apply_record(0, code, args);
      compact_if_needed();
      publish_cache();
      return;
    } catch (const client_error& e) {
      if (e.code() == "conflict") continue;
      if (e.code() == "lock_held") {
        if (!broke) {
          for (const auto& oid : {meta_oid_, log_oid_}) {
            try {
              session_.lock_break(oid);
            } catch (const client_error&) {
            }
          }
          broke = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(lease_sleep_ms));
        lease_sleep_ms = std::min(lease_sleep_ms * 2, 500);
        continue;
      }
      throw;
    }
  }
  throw client_error("conflict", "dir changelog append failed");
}

void DirTable::link(const std::string& name, uint64_t child) {
  append_ops({{kOpLink, {name, std::to_string(child)}}});
}

bool DirTable::link_if_absent(const std::string& name, uint64_t child) {
  {
    int rc = 0;
    if (lease_commit(kOpLink, {name, std::to_string(child)}, true, 0, rc)) return rc == 0;
  }
  // Serialize create/link against peers and against directory compaction so two
  // racers cannot both observe a missing name and both return success.
  for (int attempt = 0; attempt < 16; ++attempt) {
    HeldLocks locks;
    locks.session = &session_;
    try {
      locks.acquire_sorted({meta_oid_, log_oid_, snap_oid_});
    } catch (const client_error& e) {
      if (e.code() == "lock_held") {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      throw;
    }
    load(false);
    if (entries_.count(name)) return false;

    const uint64_t start = next_op_;
    changelog::Record r;
    r.op_id = start;
    r.op = static_cast<changelog::Op>(kOpLink);
    r.args = {name, std::to_string(child)};
    const std::string batch = changelog::encode_record(r);
    try {
      next_op_ = start + 1;
      auto ar = session_.append(log_oid_, batch, locks.token_for(log_oid_));
      log_bytes_ = ar.size;
      meta_cas_ = session_.put_bytes(
          meta_oid_,
          nlohmann::json{{"aios_posix_dir", 1},
                         {"next_op", next_op_},
                         {"log_bytes", log_bytes_},
                         {"snapshot_op", snapshot_op_},
                         {"snapshot_oid", snap_oid_}}
              .dump(),
          {}, meta_cas_, locks.token_for(meta_oid_), put_layout_);
      apply_record(0, kOpLink, r.args);
      // Compaction under the same locks we already hold.
      if (log_bytes_ >= changelog::kAutoCompactBytes) {
        std::string txn_id;
        try {
          txn_id = session_.txn_begin();
          txn_put_dir(session_, txn_id, *this, locks);
          session_.txn_commit(txn_id);
          txn_id.clear();
          snapshot_op_ = next_op_ > 0 ? next_op_ - 1 : 0;
          log_bytes_ = 0;
          meta_cas_ += 1;
        } catch (const client_error&) {
          if (!txn_id.empty()) {
            try {
              session_.txn_abort(txn_id);
            } catch (...) {
            }
          }
          // Link already committed; compaction is best-effort.
        }
      }
      publish_cache();
      return true;
    } catch (const client_error& e) {
      if (e.code() == "conflict" || e.code() == "lock_held") continue;
      throw;
    }
  }
  throw client_error("conflict", "dir link_if_absent exhausted retries");
}

void DirTable::unlink(const std::string& name) { append_ops({{kOpUnlink, {name}}}); }

int DirTable::unlink_if(const std::string& name, uint64_t expected_ino,
                        std::vector<std::string> extra_locks,
                        const std::function<int()>& guard) {
  if (extra_locks.empty() && !guard) {
    int rc = 0;
    if (lease_commit(kOpUnlink, {name}, false, expected_ino, rc)) return rc;
  } else if (cache_ && cache_->leases) {
    // rmdir: the child's tip is locked below to re-check emptiness; only the
    // parent's dentry removal can go through the lease.
    if (auto l = cache_->leases->get(ino_, put_layout_)) {
      HeldLocks locks;
      locks.session = &session_;
      try {
        locks.acquire_sorted(extra_locks);
      } catch (const client_error& e) {
        if (e.code() != "lock_held") throw;
        return -EBUSY;
      }
      if (guard) {
        if (int g = guard()) return g;
      }
      int rc = cache_->leases->queue(*l, kOpUnlink, {name}, false, expected_ino);
      if (rc != -EAGAIN) {
        load(true);
        return rc;
      }
    }
    cache_->leases->drop(ino_);
  }
  for (int attempt = 0; attempt < 16; ++attempt) {
    HeldLocks locks;
    locks.session = &session_;
    std::vector<std::string> oids = {meta_oid_, log_oid_, snap_oid_};
    oids.insert(oids.end(), extra_locks.begin(), extra_locks.end());
    try {
      locks.acquire_sorted(std::move(oids));
    } catch (const client_error& e) {
      if (e.code() == "lock_held") {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      throw;
    }
    load(false);
    auto it = entries_.find(name);
    if (it == entries_.end() || it->second != expected_ino) {
      publish_cache();
      return -ENOENT;
    }
    if (guard) {
      if (int rc = guard()) return rc;
    }

    const uint64_t start = next_op_;
    changelog::Record r;
    r.op_id = start;
    r.op = static_cast<changelog::Op>(kOpUnlink);
    r.args = {name};
    const std::string batch = changelog::encode_record(r);
    try {
      next_op_ = start + 1;
      auto ar = session_.append(log_oid_, batch, locks.token_for(log_oid_));
      log_bytes_ = ar.size;
      meta_cas_ = session_.put_bytes(
          meta_oid_,
          nlohmann::json{{"aios_posix_dir", 1},
                         {"next_op", next_op_},
                         {"log_bytes", log_bytes_},
                         {"snapshot_op", snapshot_op_},
                         {"snapshot_oid", snap_oid_}}
              .dump(),
          {}, meta_cas_, locks.token_for(meta_oid_), put_layout_);
      apply_record(0, kOpUnlink, r.args);
      if (log_bytes_ >= changelog::kAutoCompactBytes) {
        std::string txn_id;
        try {
          txn_id = session_.txn_begin();
          txn_put_dir(session_, txn_id, *this, locks);
          session_.txn_commit(txn_id);
          txn_id.clear();
          snapshot_op_ = next_op_ > 0 ? next_op_ - 1 : 0;
          log_bytes_ = 0;
          meta_cas_ += 1;
        } catch (const client_error&) {
          if (!txn_id.empty()) {
            try {
              session_.txn_abort(txn_id);
            } catch (...) {
            }
          }
        }
      }
      publish_cache();
      return 0;
    } catch (const client_error& e) {
      if (e.code() == "conflict" || e.code() == "lock_held") continue;
      throw;
    }
  }
  throw client_error("conflict", "dir unlink_if exhausted retries");
}

void DirTable::rename_same(const std::string& old_name, const std::string& new_name) {
  append_ops({{kOpRename, {old_name, new_name}}});
}

void DirTable::compact_if_needed() {
  if (log_bytes_ < changelog::kAutoCompactBytes) return;
  if (cache_ && cache_->leases && cache_->leases->authoritative(ino_)) return;
  // Snapshot, log truncation and meta must land together and must not race an
  // append from another client: hold the directory locks and commit through /txn.
  for (int attempt = 0; attempt < 4; ++attempt) {
    HeldLocks locks;
    locks.session = &session_;
    try {
      locks.acquire_sorted({meta_oid_, log_oid_, snap_oid_});
    } catch (const client_error& e) {
      if (e.code() == "lock_held") return;  // another client is compacting
      return;
    }
    // Reload under the locks so records appended during the window survive.
    load(false);
    if (log_bytes_ < changelog::kAutoCompactBytes) return;
    std::string txn_id;
    try {
      txn_id = session_.txn_begin();
      txn_put_dir(session_, txn_id, *this, locks);
      session_.txn_commit(txn_id);
      txn_id.clear();
      snapshot_op_ = next_op_ > 0 ? next_op_ - 1 : 0;
      log_bytes_ = 0;
      meta_cas_ += 1;
      publish_cache();
      return;
    } catch (const client_error& e) {
      if (!txn_id.empty()) {
        try {
          session_.txn_abort(txn_id);
        } catch (...) {
        }
      }
      if (e.code() == "conflict" || e.code() == "lock_held") continue;
      return;  // compaction is an optimization; never fail the caller's op
    }
  }
}

void DirTable::plan_compact_bodies(std::string& meta_out, std::string& snap_out,
                                   std::string& log_out) const {
  uint64_t next = next_op_;
  if (next < 2) next = 2;
  const uint64_t snap_op = next - 1;
  nlohmann::json entries = nlohmann::json::object();
  for (const auto& [n, i] : entries_) entries[n] = i;
  snap_out = nlohmann::json{{"entries", entries}}.dump();
  log_out.clear();
  meta_out = nlohmann::json{{"aios_posix_dir", 1},
                            {"next_op", next},
                            {"log_bytes", 0},
                            {"snapshot_op", snap_op},
                            {"snapshot_oid", snap_oid_}}
                 .dump();
}

// True when `ino` is `dir` itself or appears on dir's parent chain up to the root.
// Bypasses the inode cache so a stale parent_ino cannot hide a cycle.
bool is_ancestor_of(FsState& st, uint64_t ino, uint64_t dir) {
  uint64_t cur = dir;
  for (int guard = 0; guard < 1024 && cur != 0; ++guard) {
    if (cur == ino) return true;
    if (cur == kRootIno) return false;
    auto snap = st.session.get_object(ino_oid(st.volume, cur));
    if (!snap.exists) return false;
    const auto m = inode_from_json(snap.body, cas_from_attrs(snap.attrs));
    cur = m.parent_ino == 0 ? kRootIno : m.parent_ino;
  }
  return false;
}

int rename_cross_dir(FsState& st, uint64_t old_parent, const std::string& old_name,
                     uint64_t new_parent, const std::string& new_name) {
  if (int rc = validate_dentry_name(old_name.c_str())) return rc;
  if (int rc = validate_dentry_name(new_name.c_str())) return rc;

  // The transaction below locks both directories itself; our own leases on them
  // would refuse those locks. Flush and hand them back first.
  if (st.leases) {
    st.leases->drop(old_parent);
    st.leases->drop(new_parent);
  }
  for (int attempt = 0; attempt < 8; ++attempt) {
    DirTable old_dir = make_dir(st, old_parent);
    DirTable new_dir = make_dir(st, new_parent);
    old_dir.load(false);
    new_dir.load(false);

    auto it = old_dir.entries().find(old_name);
    if (it == old_dir.entries().end()) return -ENOENT;
    const uint64_t ino = it->second;
    if (ino == new_parent) return -EINVAL;  // would create cycle

    auto moved = load_inode(st, ino);
    if (!moved.exists) return -ENOENT;
    if (S_ISDIR(moved.mode) && is_ancestor_of(st, ino, new_parent)) return -EINVAL;

    uint64_t victim_ino = 0;
    InodeMeta victim;
    if (new_dir.entries().count(new_name)) {
      victim_ino = new_dir.entries().at(new_name);
      if (victim_ino == ino) {
        // Same inode already linked at destination name: just drop source name.
      } else {
        victim = load_inode(st, victim_ino);
        if (victim.exists && S_ISDIR(victim.mode)) return -EISDIR;
        if (moved.exists && S_ISDIR(moved.mode) && victim.exists && S_ISREG(victim.mode)) {
          return -ENOTDIR;
        }
      }
    }

    auto old_p = load_inode(st, old_parent);
    auto new_p = load_inode(st, new_parent);
    if (!old_p.exists || !new_p.exists) return -ENOENT;
    if (!S_ISDIR(old_p.mode) || !S_ISDIR(new_p.mode)) return -ENOTDIR;

    HeldLocks locks;
    locks.session = &st.session;
    std::vector<std::string> lock_oids = {old_dir.meta_oid(), old_dir.log_oid(),
                                          new_dir.meta_oid(), new_dir.log_oid()};
    try {
      locks.acquire_sorted(std::move(lock_oids));
    } catch (const client_error& e) {
      if (e.code() == "lock_held") {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      throw;
    }

    // Reload under locks.
    old_dir.load(false);
    new_dir.load(false);
    it = old_dir.entries().find(old_name);
    if (it == old_dir.entries().end()) return -ENOENT;
    if (it->second != ino) continue;  // raced
    if (new_dir.entries().count(new_name)) {
      const uint64_t cur_victim = new_dir.entries().at(new_name);
      if (cur_victim != victim_ino && cur_victim != ino) continue;
    } else if (victim_ino != 0 && victim_ino != ino) {
      continue;  // victim disappeared; replan
    }

    old_p = load_inode(st, old_parent);
    new_p = load_inode(st, new_parent);
    moved = load_inode(st, ino);
    if (victim_ino && victim_ino != ino) victim = load_inode(st, victim_ino);

    // Apply dentry mutations in memory.
    old_dir.mutable_entries().erase(old_name);
    if (victim_ino && victim_ino != ino) new_dir.mutable_entries().erase(new_name);
    new_dir.mutable_entries()[new_name] = ino;

    const uint64_t ts = now_ns();
    old_p.mtime_ns = old_p.ctime_ns = ts;
    new_p.mtime_ns = new_p.ctime_ns = ts;
    if (S_ISDIR(moved.mode)) {
      if (old_p.nlink > 2) old_p.nlink -= 1;
      new_p.nlink += 1;
    }

    std::string txn_id;
    try {
      txn_id = st.session.txn_begin();
      txn_put_dir(st.session, txn_id, old_dir, locks);
      txn_put_dir(st.session, txn_id, new_dir, locks);
      st.session.txn_prepare_put(txn_id, ino_oid(st.volume, old_parent), inode_to_json(old_p),
                                 old_p.cas);
      st.session.txn_prepare_put(txn_id, ino_oid(st.volume, new_parent), inode_to_json(new_p),
                                 new_p.cas);

      bool delete_victim = false;
      uint64_t gc_size = 0;
      uint64_t gc_stripe_unit = 0;
      uint32_t gc_uid = 0;
      uint32_t gc_gid = 0;
      uint32_t gc_project_id = 0;
      bool gc_chunks = false;
      if (victim_ino && victim_ino != ino && victim.exists) {
        if (victim.nlink > 1) {
          victim.nlink -= 1;
          victim.ctime_ns = ts;
          st.session.txn_prepare_put(txn_id, ino_oid(st.volume, victim_ino),
                                     inode_to_json(victim), victim.cas);
        } else {
          gc_size = victim.size;
          gc_stripe_unit = victim.stripe_unit ? victim.stripe_unit : st.stripe_unit;
          gc_uid = victim.uid;
          gc_gid = victim.gid;
          gc_project_id = victim.project_id;
          gc_chunks = S_ISREG(victim.mode);
          st.session.txn_prepare_delete(txn_id, ino_oid(st.volume, victim_ino));
          delete_victim = true;
        }
      }

      // Parent and project_id must commit with the dentry rewrite. A follow-up
      // PUT was racy with the dirty-size flusher and was swallowed, so reconcile
      // still charged the source project after a rename out.
      const auto old_proj = moved.project_id;
      const bool reproject = moved.project_id != new_p.project_id;
      const bool move_inode = moved.parent_ino != new_parent || reproject;
      if (move_inode) {
        {
          std::lock_guard lock(st.mu);
          auto dit = st.dirty_sizes.find(moved.ino);
          if (dit != st.dirty_sizes.end()) {
            moved.size = std::max(moved.size, dit->second.size);
            moved.mtime_ns = std::max(moved.mtime_ns, dit->second.mtime_ns);
            moved.ctime_ns = std::max(moved.ctime_ns, dit->second.ctime_ns);
          }
        }
        moved.parent_ino = new_parent;
        if (reproject) moved.project_id = new_p.project_id;
        moved.ctime_ns = ts;
        st.session.txn_prepare_put(txn_id, ino_oid(st.volume, ino), inode_to_json(moved),
                                   moved.cas);
      }

      st.session.txn_commit(txn_id);
      txn_id.clear();

      old_p.cas += 1;
      new_p.cas += 1;
      old_p.exists = true;
      new_p.exists = true;
      if (victim_ino && victim_ino != ino && victim.exists && !delete_victim) {
        victim.cas += 1;
      }
      if (move_inode) {
        moved.cas += 1;
        moved.exists = true;
      }
      {
        std::lock_guard lock(st.mu);
        cache_inode_locked(st, old_p);
        cache_inode_locked(st, new_p);
        if (move_inode) {
          auto dit = st.dirty_sizes.find(ino);
          if (dit != st.dirty_sizes.end() && dit->second.size <= moved.size)
            st.dirty_sizes.erase(dit);
          cache_inode_locked(st, moved);
        }
        if (victim_ino && victim_ino != ino) {
          if (delete_victim) {
            cache_erase_locked(st, victim_ino);
          } else if (victim.exists) {
            cache_inode_locked(st, victim);
          }
        }
      }
      if (delete_victim) {
        if (gc_chunks) {
          delete_file_chunks(st, victim_ino, gc_size, gc_stripe_unit, gc_project_id, gc_uid,
                             gc_gid);
        }
      }
      if (reproject && st.quota) {
        st.quota->note_reproject(old_proj, moved.project_id, moved.uid, moved.gid, moved.size);
      }
      old_dir.publish();
      new_dir.publish();
      {
        // Cached paths below the moved entry are stale (layout-rule matching).
        std::lock_guard lock(st.mu);
        if (S_ISDIR(moved.mode)) st.path_cache.clear();
        else st.path_cache.erase(ino);
      }
      mark_rstat_dirty(st, old_parent);
      mark_rstat_dirty(st, new_parent);
      return 0;
    } catch (const client_error& e) {
      if (!txn_id.empty()) {
        try {
          st.session.txn_abort(txn_id);
        } catch (...) {
        }
      }
      if (e.code() == "conflict" || e.code() == "lock_held") {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      return map_error(e);
    }
  }
  return -EAGAIN;
}

int rename_same_dir(FsState& st, uint64_t parent, const std::string& old_name,
                    const std::string& new_name) {
  if (int rc = validate_dentry_name(old_name.c_str())) return rc;
  if (int rc = validate_dentry_name(new_name.c_str())) return rc;
  if (old_name == new_name) return 0;

  if (st.leases) {
    // Under our lease the rename is one queued record; the replaced inode (if
    // any) is dropped synchronously like an unlink. create+rename (editors,
    // rsync, compilers) thus never gives the lease up.
    if (auto l = st.leases->get(parent, meta_layout_for_ino(st, parent))) {
      uint64_t moved = 0;
      uint64_t victim = 0;
      const int prc = st.leases->peek(*l, old_name, moved);
      if (prc == 0 && moved == 0) return -ENOENT;
      if (prc == 0 && st.leases->peek(*l, new_name, victim) == 0) {
        InodeMeta mm = load_inode(st, moved);
        InodeMeta vm;
        if (victim != 0 && victim != moved) {
          vm = load_inode(st, victim);
          if (vm.exists && S_ISDIR(vm.mode)) return -EISDIR;
          if (mm.exists && S_ISDIR(mm.mode) && vm.exists && S_ISREG(vm.mode)) return -ENOTDIR;
        }
        const int rc = st.leases->rename(*l, old_name, new_name, moved, victim);
        if (rc == 0) {
          InodeMeta pmeta;
          touch_dir_inode(st, parent, pmeta, now_ns(), 0);
          if (victim != 0 && victim != moved && vm.exists) drop_nlink(st, victim);
          {
            std::lock_guard lock(st.mu);
            st.path_cache.clear();
          }
          mark_rstat_dirty(st, parent);
          return 0;
        }
        if (rc == -ENOENT) return -ENOENT;
      }
    }
    st.leases->drop(parent);
  }
  for (int attempt = 0; attempt < 8; ++attempt) {
    DirTable dir = make_dir(st, parent);
    dir.load(false);

    auto oit = dir.entries().find(old_name);
    if (oit == dir.entries().end()) return -ENOENT;
    const uint64_t ino = oit->second;

    uint64_t victim_ino = 0;
    InodeMeta victim;
    if (dir.entries().count(new_name)) {
      victim_ino = dir.entries().at(new_name);
      if (victim_ino != ino) {
        victim = load_inode(st, victim_ino);
        if (victim.exists && S_ISDIR(victim.mode)) return -EISDIR;
        auto moved = load_inode(st, ino);
        if (moved.exists && S_ISDIR(moved.mode) && victim.exists && S_ISREG(victim.mode)) {
          return -ENOTDIR;
        }
      }
    }

    auto pmeta = load_inode(st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;

    HeldLocks locks;
    locks.session = &st.session;
    try {
      locks.acquire_sorted({dir.meta_oid(), dir.log_oid()});
    } catch (const client_error& e) {
      if (e.code() == "lock_held") {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      throw;
    }

    dir.load(false);
    oit = dir.entries().find(old_name);
    if (oit == dir.entries().end()) return -ENOENT;
    if (oit->second != ino) continue;
    if (dir.entries().count(new_name)) {
      const uint64_t cur_victim = dir.entries().at(new_name);
      if (cur_victim != victim_ino && cur_victim != ino) continue;
    } else if (victim_ino != 0 && victim_ino != ino) {
      continue;
    }

    pmeta = load_inode(st, parent);
    if (victim_ino && victim_ino != ino) victim = load_inode(st, victim_ino);

    dir.mutable_entries().erase(old_name);
    if (victim_ino && victim_ino != ino) dir.mutable_entries().erase(new_name);
    dir.mutable_entries()[new_name] = ino;

    const uint64_t ts = now_ns();
    pmeta.mtime_ns = pmeta.ctime_ns = ts;

    std::string txn_id;
    try {
      txn_id = st.session.txn_begin();
      txn_put_dir(st.session, txn_id, dir, locks);
      st.session.txn_prepare_put(txn_id, ino_oid(st.volume, parent), inode_to_json(pmeta),
                                 pmeta.cas);

      bool delete_victim = false;
      uint64_t gc_size = 0;
      uint64_t gc_stripe_unit = 0;
      uint32_t gc_uid = 0;
      uint32_t gc_gid = 0;
      uint32_t gc_project_id = 0;
      bool gc_chunks = false;
      if (victim_ino && victim_ino != ino && victim.exists) {
        if (victim.nlink > 1) {
          victim.nlink -= 1;
          victim.ctime_ns = ts;
          st.session.txn_prepare_put(txn_id, ino_oid(st.volume, victim_ino),
                                     inode_to_json(victim), victim.cas);
        } else {
          gc_size = victim.size;
          gc_stripe_unit = victim.stripe_unit ? victim.stripe_unit : st.stripe_unit;
          gc_uid = victim.uid;
          gc_gid = victim.gid;
          gc_project_id = victim.project_id;
          gc_chunks = S_ISREG(victim.mode);
          st.session.txn_prepare_delete(txn_id, ino_oid(st.volume, victim_ino));
          delete_victim = true;
        }
      }

      st.session.txn_commit(txn_id);
      txn_id.clear();

      pmeta.cas += 1;
      pmeta.exists = true;
      if (victim_ino && victim_ino != ino && victim.exists && !delete_victim) {
        victim.cas += 1;
      }
      {
        std::lock_guard lock(st.mu);
        cache_inode_locked(st, pmeta);
        if (victim_ino && victim_ino != ino) {
          if (delete_victim) {
            cache_erase_locked(st, victim_ino);
          } else if (victim.exists) {
            cache_inode_locked(st, victim);
          }
        }
      }
      if (delete_victim && gc_chunks) {
        delete_file_chunks(st, victim_ino, gc_size, gc_stripe_unit, gc_project_id, gc_uid,
                           gc_gid);
      }
      dir.publish();
      {
        std::lock_guard lock(st.mu);
        st.path_cache.clear();
      }
      mark_rstat_dirty(st, parent);
      return 0;
    } catch (const client_error& e) {
      if (!txn_id.empty()) {
        try {
          st.session.txn_abort(txn_id);
        } catch (...) {
        }
      }
      if (e.code() == "conflict" || e.code() == "lock_held") {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      return map_error(e);
    }
  }
  return -EAGAIN;
}

namespace {

// Pending deferred write state must survive any refresh of the cached record.
void merge_dirty_locked(const FsState& st, InodeMeta& m) {
  auto it = st.dirty_sizes.find(m.ino);
  if (it == st.dirty_sizes.end()) return;
  const DirtySize& d = it->second;
  m.size = std::max(m.size, d.size);
  m.mtime_ns = std::max(m.mtime_ns, d.mtime_ns);
  m.ctime_ns = std::max(m.ctime_ns, d.ctime_ns);
}

void inode_cache_evict_locked(FsState& st) {
  if (st.inode_cache.size() <= kInodeCacheMaxEntries) return;
  // Amortized: drop the least recently used clean entries down to 7/8 of the bound.
  std::vector<std::pair<uint64_t, uint64_t>> clean;  // lru, ino
  clean.reserve(st.inode_cache.size());
  for (const auto& [ino, e] : st.inode_cache) {
    if (e.unpublished || st.dirty_sizes.count(ino)) continue;
    clean.emplace_back(e.lru, ino);
  }
  const size_t target = kInodeCacheMaxEntries - kInodeCacheMaxEntries / 8;
  if (st.inode_cache.size() <= target) return;
  const size_t want = std::min(clean.size(), st.inode_cache.size() - target);
  std::partial_sort(clean.begin(), clean.begin() + static_cast<std::ptrdiff_t>(want), clean.end());
  for (size_t i = 0; i < want; ++i) st.inode_cache.erase(clean[i].second);
}

}  // namespace

void cache_inode_locked(FsState& st, const InodeMeta& m) {
  auto& e = st.inode_cache[m.ino];
  e.meta = m;
  e.unpublished = false;
  e.create_path.reset();
  merge_dirty_locked(st, e.meta);
  e.loaded = std::chrono::steady_clock::now();
  e.lru = ++st.inode_cache_clock;
  inode_cache_evict_locked(st);
}

void cache_unpublished_locked(FsState& st, const InodeMeta& m,
                              const std::optional<std::string>& create_path) {
  st.unpublished_dropped.erase(m.ino);
  auto& e = st.inode_cache[m.ino];
  e.meta = m;
  e.meta.exists = true;
  e.unpublished = true;
  e.create_path = create_path;
  merge_dirty_locked(st, e.meta);
  e.loaded = std::chrono::steady_clock::now();
  e.lru = ++st.inode_cache_clock;
  inode_cache_evict_locked(st);
}

std::optional<std::string> unpublished_create_path(FsState& st, uint64_t ino) {
  std::lock_guard lock(st.mu);
  auto it = st.inode_cache.find(ino);
  if (it != st.inode_cache.end() && it->second.unpublished) return it->second.create_path;
  return std::nullopt;
}

void cache_erase_locked(FsState& st, uint64_t ino) { st.inode_cache.erase(ino); }

void dir_cache_evict_locked(FsState& st) {
  if (st.dir_cache.size() <= kDirCacheMaxEntries) return;
  const auto now = std::chrono::steady_clock::now();
  for (auto it = st.dir_cache.begin(); it != st.dir_cache.end();) {
    if (now - it->second.loaded >= kDirCacheTtl) {
      it = st.dir_cache.erase(it);
    } else {
      ++it;
    }
  }
  if (st.dir_cache.size() <= kDirCacheMaxEntries) return;
  std::vector<std::pair<std::chrono::steady_clock::time_point, uint64_t>> order;
  order.reserve(st.dir_cache.size());
  for (const auto& [ino, e] : st.dir_cache) order.emplace_back(e.loaded, ino);
  const size_t target = kDirCacheMaxEntries - kDirCacheMaxEntries / 8;
  const size_t want = st.dir_cache.size() - target;
  std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(want), order.end());
  for (size_t i = 0; i < want; ++i) st.dir_cache.erase(order[i].second);
}

InodeMeta load_inode(FsState& st, uint64_t ino) {
  {
    std::lock_guard lock(st.mu);
    auto it = st.inode_cache.find(ino);
    if (it != st.inode_cache.end() &&
        (it->second.unpublished ||
         std::chrono::steady_clock::now() - it->second.loaded < kInodeCacheTtl)) {
      it->second.lru = ++st.inode_cache_clock;
      return it->second.meta;
    }
  }
  auto snap = st.session.get_object(ino_oid(st.volume, ino));
  if (!snap.exists) {
    InodeMeta m;
    m.ino = ino;
    std::lock_guard lock(st.mu);
    // A deferred write against a record another client removed has nothing to land
    // on; the pending entry is dropped by the flusher when it observes !exists.
    cache_erase_locked(st, ino);
    return m;
  }
  auto m = inode_from_json(snap.body, cas_from_attrs(snap.attrs));
  std::lock_guard lock(st.mu);
  cache_inode_locked(st, m);
  return st.inode_cache[ino].meta;
}

void prefetch_inodes(FsState& st, const std::vector<uint64_t>& inos) {
  std::vector<uint64_t> miss;
  {
    std::lock_guard lock(st.mu);
    const auto now = std::chrono::steady_clock::now();
    for (uint64_t ino : inos) {
      auto it = st.inode_cache.find(ino);
      if (it != st.inode_cache.end() &&
          (it->second.unpublished || now - it->second.loaded < kInodeCacheTtl))
        continue;
      miss.push_back(ino);
    }
  }
  std::sort(miss.begin(), miss.end());
  miss.erase(std::unique(miss.begin(), miss.end()), miss.end());
  if (miss.size() < 2) return;  // the caller's own load is as good
  const size_t nthreads = std::min<size_t>(8, miss.size());
  std::atomic<size_t> next{0};
  std::vector<std::thread> workers;
  workers.reserve(nthreads);
  for (size_t t = 0; t < nthreads; ++t) {
    workers.emplace_back([&] {
      for (;;) {
        const size_t i = next.fetch_add(1);
        if (i >= miss.size()) return;
        try {
          load_inode(st, miss[i]);
        } catch (...) {
        }
      }
    });
  }
  for (auto& w : workers) w.join();
}

void store_inode(FsState& st, InodeMeta& m, const std::optional<std::string>& path_for_layout,
                 const InodeReapply& reapply) {
  const auto layout =
      path_for_layout ? meta_layout_for_path(st, *path_for_layout) : meta_layout_for_ino(st, m.ino);
  for (int attempt = 0; attempt < 8; ++attempt) {
    try {
      m.cas = st.session.put_bytes(ino_oid(st.volume, m.ino), inode_to_json(m), {}, m.cas,
                                   std::nullopt, layout);
      m.exists = true;
      std::lock_guard lock(st.mu);
      // The PUT covered the deferred size only if it was at least as large; a write
      // that raced in between keeps its dirty entry for the next flush.
      auto dit = st.dirty_sizes.find(m.ino);
      if (dit != st.dirty_sizes.end() && dit->second.size <= m.size) st.dirty_sizes.erase(dit);
      cache_inode_locked(st, m);
      return;
    } catch (const client_error& e) {
      if (e.code() != "conflict") throw;
      // Another writer published first (typical: lease flusher vs setattr/fsync
      // on an unpublished create). Adopt the server copy when we have no
      // reapply, otherwise restated mutation below.
      if (!reapply) {
        auto fresh = st.session.get_object(ino_oid(st.volume, m.ino));
        if (fresh.exists) {
          InodeMeta next = inode_from_json(fresh.body, cas_from_attrs(fresh.attrs));
          next.ino = m.ino;
          std::lock_guard lock(st.mu);
          merge_dirty_locked(st, next);
          cache_inode_locked(st, next);
          m = std::move(next);
          return;
        }
        m.cas = 0;
        continue;
      }
      // Another writer changed the record. Re-applying the caller's stale copy would
      // silently discard their fields, so only operations that can restate their own
      // mutation against a fresh record may retry.
      auto fresh = st.session.get_object(ino_oid(st.volume, m.ino));
      if (!fresh.exists) {
        m.cas = 0;
        continue;
      }
      InodeMeta next = inode_from_json(fresh.body, cas_from_attrs(fresh.attrs));
      next.ino = m.ino;
      reapply(next);
      {
        std::lock_guard lock(st.mu);
        merge_dirty_locked(st, next);
      }
      m = std::move(next);
    }
  }
  throw client_error("conflict", "inode store failed");
}

void flush_dirty_inode(FsState& st, uint64_t ino) {
  flush_dirty_chunks(st, ino);
  DirtySize d;
  std::optional<std::string> path;
  {
    std::lock_guard lock(st.mu);
    auto it = st.dirty_sizes.find(ino);
    if (it == st.dirty_sizes.end()) return;
    d = it->second;
    auto cit = st.inode_cache.find(ino);
    if (cit != st.inode_cache.end() && cit->second.unpublished) path = cit->second.create_path;
  }
  InodeMeta m = load_inode(st, ino);
  if (!m.exists) {
    drop_dirty_chunks(st, ino);
    std::lock_guard lock(st.mu);
    st.dirty_sizes.erase(ino);
    return;
  }
  m.size = std::max(m.size, d.size);
  m.mtime_ns = std::max(m.mtime_ns, d.mtime_ns);
  m.ctime_ns = std::max(m.ctime_ns, d.ctime_ns);
  store_inode(st, m, path, [d](InodeMeta& next) {
    next.size = std::max(next.size, d.size);
    next.mtime_ns = std::max(next.mtime_ns, d.mtime_ns);
    next.ctime_ns = std::max(next.ctime_ns, d.ctime_ns);
  });
}

void flush_all_dirty_inodes(FsState& st,
                            std::optional<std::chrono::steady_clock::duration> min_age) {
  std::vector<uint64_t> inos;
  {
    std::lock_guard lock(st.mu);
    const auto now = std::chrono::steady_clock::now();
    inos.reserve(st.dirty_sizes.size());
    for (const auto& [ino, d] : st.dirty_sizes) {
      if (min_age && now - d.since < *min_age) continue;
      inos.push_back(ino);
    }
  }
  {
    std::lock_guard lock(st.dirty_chunk_mu);
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [ino, chunks] : st.dirty_chunks) {
      if (std::find(inos.begin(), inos.end(), ino) != inos.end()) continue;
      bool due = !min_age;
      if (min_age) {
        for (const auto& [c, d] : chunks) {
          (void)c;
          if (now - d.since >= *min_age) {
            due = true;
            break;
          }
        }
      }
      if (due) inos.push_back(ino);
    }
  }
  for (uint64_t ino : inos) {
    try {
      flush_dirty_inode(st, ino);
    } catch (...) {
    }
  }
}

void ensure_super(FsState& st) {
  {
    std::lock_guard lock(st.mu);
    if (st.super.exists &&
        std::chrono::steady_clock::now() - st.super_loaded < kInodeCacheTtl) {
      return;
    }
  }
  auto snap = st.session.get_object(super_oid(st.volume));
  if (snap.exists) {
    std::lock_guard lock(st.mu);
    st.super = super_from_json(snap.body, cas_from_attrs(snap.attrs));
    st.super_loaded = std::chrono::steady_clock::now();
    return;
  }
  SuperMeta m;
  m.next_ino = 2;
  m.stripe_unit = st.stripe_unit;
  m.stripe_width = st.stripe_width;
  m.uuid = "fs-" + st.volume;
  m.cas = 0;
  m.cas = st.session.put_bytes(super_oid(st.volume), super_to_json(m), {}, 0);
  m.exists = true;
  std::lock_guard lock(st.mu);
  st.super = m;
  st.super_loaded = std::chrono::steady_clock::now();
}

uint64_t alloc_ino(FsState& st) {
  // Reserve a batch from the super object with one CAS; unused numbers of a
  // batch are simply skipped (inode numbers need only be unique).
  for (int attempt = 0; attempt < 16; ++attempt) {
    ensure_super(st);
    SuperMeta m;
    {
      std::lock_guard lock(st.mu);
      if (st.ino_next < st.ino_end) return st.ino_next++;
      m = st.super;
      m.next_ino += kInoBatch;
    }
    try {
      m.cas = st.session.put_bytes(super_oid(st.volume), super_to_json(m), {}, m.cas);
      m.exists = true;
      std::lock_guard lock(st.mu);
      const uint64_t first = m.next_ino - kInoBatch;
      st.super = m;
      st.super_loaded = std::chrono::steady_clock::now();
      st.ino_next = first + 1;
      st.ino_end = m.next_ino;
      return first;
    } catch (const client_error& e) {
      if (e.code() != "conflict") throw;
      auto snap = st.session.get_object(super_oid(st.volume));
      if (snap.exists) {
        std::lock_guard lock(st.mu);
        st.super = super_from_json(snap.body, cas_from_attrs(snap.attrs));
        st.super_loaded = std::chrono::steady_clock::now();
      }
    }
  }
  throw client_error("conflict", "alloc_ino failed");
}

void ensure_root(FsState& st) {
  auto root = load_inode(st, kRootIno);
  if (root.exists) return;
  const uint64_t ts = now_ns();
  root.ino = kRootIno;
  root.mode = S_IFDIR | 0755;
  root.nlink = 2;
  root.uid = st.default_uid;
  root.gid = st.default_gid;
  root.parent_ino = 0;
  root.size = 0;
  root.atime_ns = root.mtime_ns = root.ctime_ns = ts;
  root.stripe_unit = st.stripe_unit;
  root.stripe_width = st.stripe_width;
  root.cas = 0;
  store_inode(st, root);
  DirTable dir = make_dir(st, kRootIno);
  dir.load(false);  // creates empty on first link
}

void drop_nlink(FsState& st, uint64_t ino) {
  bool unpublished = false;
  {
    std::lock_guard lock(st.mu);
    auto it = st.inode_cache.find(ino);
    unpublished = it != st.inode_cache.end() && it->second.unpublished;
    if (unpublished) st.unpublished_dropped.insert(ino);
  }
  if (!unpublished) flush_dirty_inode(st, ino);
  auto m = load_inode(st, ino);
  if (!m.exists) return;
  if (m.nlink > 1) {
    const uint64_t ts = now_ns();
    m.nlink -= 1;
    m.ctime_ns = ts;
    store_inode(st, m, std::nullopt, [ts](InodeMeta& next) {
      if (next.nlink > 0) next.nlink -= 1;
      next.ctime_ns = ts;
    });
    return;
  }
  if (S_ISREG(m.mode)) {
    if (unpublished) {
      delete_file_chunks(st, ino, m.size, m.stripe_unit, m.project_id, m.uid, m.gid);
    } else {
      truncate_file(st, ino, 0);
    }
  }
  if (!unpublished) {
    try {
      st.session.delete_object(ino_oid(st.volume, ino));
    } catch (...) {
    }
  }
  drop_dirty_chunks(st, ino);
  std::string flock_token;
  {
    std::lock_guard lock(st.mu);
    st.inode_cache.erase(ino);
    st.dirty_sizes.erase(ino);
    auto fit = st.flock_tokens.find(ino);
    if (fit != st.flock_tokens.end()) {
      flock_token = std::move(fit->second);
      st.flock_tokens.erase(fit);
    }
  }
  if (!flock_token.empty()) {
    try {
      st.session.lock_release(ino_oid(st.volume, ino), flock_token);
    } catch (...) {
    }
  }
}

// mtime/ctime/nlink of a directory after a namespace change. Under a lease the
// server copy is written once per flush batch and only the in-core inode is
// updated here; otherwise it is a CAS PUT with reapply.
void touch_dir_inode(FsState& st, uint64_t dir_ino, InodeMeta& pmeta, uint64_t ts,
                     int nlink_delta) {
  if (st.leases) {
    if (auto l = st.leases->authoritative(dir_ino)) {
      st.leases->touch_parent(*l, ts, nlink_delta);
      return;
    }
  }
  if (!pmeta.exists) {
    pmeta = load_inode(st, dir_ino);
    if (!pmeta.exists) return;
  }
  pmeta.mtime_ns = pmeta.ctime_ns = ts;
  if (nlink_delta > 0) {
    pmeta.nlink += static_cast<uint32_t>(nlink_delta);
  } else if (nlink_delta < 0 && pmeta.nlink > 2) {
    pmeta.nlink -= 1;
  }
  store_inode(st, pmeta, std::nullopt, [ts, nlink_delta](InodeMeta& next) {
    next.mtime_ns = next.ctime_ns = ts;
    if (nlink_delta > 0) {
      next.nlink += static_cast<uint32_t>(nlink_delta);
    } else if (nlink_delta < 0 && next.nlink > 2) {
      next.nlink -= 1;
    }
  });
}

void release_all_flocks(FsState& st) {
  std::unordered_map<uint64_t, std::string> held;
  {
    std::lock_guard lock(st.mu);
    held.swap(st.flock_tokens);
  }
  for (const auto& [ino, token] : held) {
    try {
      st.session.lock_release(ino_oid(st.volume, ino), token);
    } catch (...) {
    }
  }
}

int read_file(FsState& st, uint64_t ino, uint64_t offset, void* buf, size_t len, size_t* out_len) {
  if (out_len) *out_len = 0;
  auto meta = load_inode(st, ino);
  if (!meta.exists) return -ENOENT;
  if (!S_ISREG(meta.mode)) return -EISDIR;
  if (offset >= meta.size || len == 0) return 0;
  const uint64_t end = std::min(offset + static_cast<uint64_t>(len), meta.size);
  const size_t want = static_cast<size_t>(end - offset);
  if (st.qos && !st.qos->admit(meta.project_id, meta.uid, meta.gid, want)) return -EAGAIN;
  aios::note_frontend_io(st.frontend_label, false, want);
  auto* out = static_cast<uint8_t*>(buf);
  std::memset(out, 0, want);

  const uint64_t unit = meta.stripe_unit ? meta.stripe_unit : st.stripe_unit;
  uint64_t pos = offset;
  size_t written = 0;
  while (pos < end) {
    const uint64_t chunk = pos / unit;
    const uint64_t chunk_off = pos % unit;
    const uint64_t chunk_end = std::min(end, (chunk + 1) * unit);
    const size_t n = static_cast<size_t>(chunk_end - pos);
    try {
      uint64_t cas = 0;
      if (copy_dirty_range(st, ino, chunk, chunk_off, out + written, n)) {
        pos += n;
        written += n;
        continue;
      }
      ChunkCache::Body body = st.chunk_cache.lookup(ino, chunk, cas);
      if (!body) {
        auto snap = st.session.get_object(chunk_oid(st.volume, ino, chunk));
        if (snap.exists) {
          body = std::make_shared<const std::string>(std::move(snap.body));
          cas = cas_from_attrs(snap.attrs);
          st.chunk_cache.store(ino, chunk, body, cas);
        }
      }
      if (body && chunk_off < body->size()) {
        const size_t avail = static_cast<size_t>(body->size() - chunk_off);
        const size_t take = std::min(n, avail);
        std::memcpy(out + written, body->data() + chunk_off, take);
      }
    } catch (const client_error& e) {
      return map_error(e);
    }
    pos += n;
    written += n;
  }
  if (out_len) *out_len = written;
  return 0;
}

void normalize_ranges(aios_range* ranges, uint32_t* nranges) {
  if (!ranges || !nranges || *nranges < 2) return;
  const uint32_t n = *nranges;
  std::sort(ranges, ranges + n, [](const aios_range& a, const aios_range& b) {
    if (a.offset != b.offset) return a.offset < b.offset;
    return a.length < b.length;
  });
  uint32_t out = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (out == 0) {
      ranges[out++] = ranges[i];
      continue;
    }
    aios_range& prev = ranges[out - 1];
    const uint64_t prev_end = prev.offset + prev.length;
    if (ranges[i].offset <= prev_end) {
      const uint64_t cur_end = ranges[i].offset + ranges[i].length;
      if (cur_end > prev_end) prev.length = cur_end - prev.offset;
    } else {
      ranges[out++] = ranges[i];
    }
  }
  *nranges = out;
}

int prefetch_file(FsState& st, uint64_t ino, aios_range* ranges, uint32_t nranges, uint32_t flags) {
  if (!ranges) return -EINVAL;
  if (nranges == 0) return -EINVAL;
  if (nranges > AIOS_PREFETCH_MAX_RANGES) return -E2BIG;
  if (flags & ~AIOS_PREFETCH_SUPPORTED_FLAGS) return -EOPNOTSUPP;

  auto meta = load_inode(st, ino);
  if (!meta.exists) return -ENOENT;
  if (!S_ISREG(meta.mode)) return -EISDIR;

  uint64_t total = 0;
  for (uint32_t i = 0; i < nranges; ++i) {
    const uint64_t offset = ranges[i].offset;
    const uint64_t length = ranges[i].length;
    if (length == 0) return -EINVAL;
    if (offset > UINT64_MAX - length) return -EINVAL;
    const uint64_t end = offset + length;
    if (offset >= meta.size || end > meta.size) return -EINVAL;
    if (UINT64_MAX - total < length) return -EOVERFLOW;
    total += length;
    if (total > AIOS_PREFETCH_MAX_BYTES) return -E2BIG;
  }

  normalize_ranges(ranges, &nranges);
  if (nranges == 0) return -EINVAL;

  if (st.qos && !st.qos->admit(meta.project_id, meta.uid, meta.gid, total)) return -EAGAIN;
  aios::note_frontend_io(st.frontend_label, false, total);

  const uint64_t unit = meta.stripe_unit ? meta.stripe_unit : st.stripe_unit;
  std::vector<uint64_t> chunks;
  chunks.reserve(nranges);
  for (uint32_t i = 0; i < nranges; ++i) {
    const uint64_t first = ranges[i].offset / unit;
    const uint64_t last = (ranges[i].offset + ranges[i].length - 1) / unit;
    for (uint64_t c = first; c <= last; ++c) chunks.push_back(c);
  }
  std::sort(chunks.begin(), chunks.end());
  chunks.erase(std::unique(chunks.begin(), chunks.end()), chunks.end());
  if (chunks.empty()) return 0;

  std::vector<uint64_t> miss;
  miss.reserve(chunks.size());
  for (uint64_t chunk : chunks) {
    uint64_t cas = 0;
    if (has_dirty_chunk(st, ino, chunk)) continue;
    if (!st.chunk_cache.lookup(ino, chunk, cas)) miss.push_back(chunk);
  }
  if (miss.empty()) return 0;

  std::atomic<int> err{0};
  size_t nthreads = st.stripe_width ? st.stripe_width : 1;
  if (nthreads > miss.size()) nthreads = miss.size();
  if (nthreads > 8) nthreads = 8;
  std::atomic<size_t> next{0};
  auto worker = [&] {
    for (;;) {
      if (err.load() != 0) return;
      const size_t i = next.fetch_add(1);
      if (i >= miss.size()) return;
      const uint64_t chunk = miss[i];
      try {
        auto snap = st.session.get_object(chunk_oid(st.volume, ino, chunk));
        if (snap.exists) {
          st.chunk_cache.store(ino, chunk, std::move(snap.body), cas_from_attrs(snap.attrs));
        }
      } catch (const client_error& e) {
        int mapped = map_error(e);
        int expected = 0;
        err.compare_exchange_strong(expected, mapped ? mapped : -EIO);
      } catch (...) {
        int expected = 0;
        err.compare_exchange_strong(expected, -EIO);
      }
    }
  };
  if (nthreads <= 1) {
    worker();
  } else {
    std::vector<std::thread> workers;
    workers.reserve(nthreads);
    for (size_t t = 0; t < nthreads; ++t) workers.emplace_back(worker);
    for (auto& w : workers) w.join();
  }
  return err.load();
}

int write_file(FsState& st, uint64_t ino, uint64_t offset, const void* buf, size_t len,
               size_t* out_len) {
  if (out_len) *out_len = 0;
  if (len == 0) return 0;
  auto meta = load_inode(st, ino);
  if (!meta.exists) return -ENOENT;
  if (!S_ISREG(meta.mode)) return -EISDIR;
  const uint64_t new_size_pre = std::max(meta.size, offset + static_cast<uint64_t>(len));
  if (new_size_pre > meta.size && st.quota) {
    const auto grow = static_cast<std::int64_t>(new_size_pre - meta.size);
    if (!st.quota->may_grow(meta.project_id, meta.uid, meta.gid, grow)) return -EDQUOT;
  }
  if (st.qos && !st.qos->admit(meta.project_id, meta.uid, meta.gid, len)) return -EBUSY;
  aios::note_frontend_io(st.frontend_label, true, len);
  const uint64_t unit = meta.stripe_unit ? meta.stripe_unit : st.stripe_unit;
  const auto* in = static_cast<const uint8_t*>(buf);
  uint64_t pos = offset;
  size_t done = 0;

  while (done < len) {
    const uint64_t chunk = pos / unit;
    const uint64_t chunk_off = pos % unit;
    const size_t n = std::min(static_cast<size_t>(unit - chunk_off), len - done);
    const std::string oid = chunk_oid(st.volume, ino, chunk);
    int chunk_rc = -EAGAIN;
    std::lock_guard chunk_guard(st.chunk_lock(ino, chunk));
    for (int attempt = 0; attempt < kChunkWriteRetries; ++attempt) {
      if (attempt > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1 + (attempt % 4)));
      }
      try {
        bool mutated = false;
        {
          std::lock_guard dlock(st.dirty_chunk_mu);
          auto iit = st.dirty_chunks.find(ino);
          if (iit != st.dirty_chunks.end()) {
            auto cit = iit->second.find(chunk);
            if (cit != iit->second.end()) {
              auto& d = cit->second;
              if (d.body.size() < chunk_off + n) d.body.resize(chunk_off + n, '\0');
              std::memcpy(d.body.data() + chunk_off, in + done, n);
              account_dirty_locked(st, d);
              d.gen++;
              mutated = true;
            }
          }
        }
        if (!mutated) {
          std::string body;
          uint64_t cas = 0;
          if (auto cached = st.chunk_cache.lookup(ino, chunk, cas)) {
            body = *cached;
          } else {
            auto existing = st.session.get_object(oid);
            if (existing.exists) {
              body = std::move(existing.body);
              cas = cas_from_attrs(existing.attrs);
            }
          }
          if (body.size() < chunk_off + n) body.resize(chunk_off + n, '\0');
          std::memcpy(body.data() + chunk_off, in + done, n);
          std::lock_guard dlock(st.dirty_chunk_mu);
          auto& d = st.dirty_chunks[ino][chunk];
          d.body = std::move(body);
          d.cas = cas;
          d.gen = 1;
          d.since = std::chrono::steady_clock::now();
          account_dirty_locked(st, d);
        }
        chunk_rc = 0;
        break;
      } catch (const client_error& e) {
        st.chunk_cache.drop(ino, chunk);
        if (e.code() == "conflict") continue;
        return map_error(e);
      }
    }
    if (chunk_rc) return chunk_rc;
    pos += n;
    done += n;
  }

  // Concurrent writers to one inode each hold a pre-write snapshot; the shared
  // cached record and the deferred size must be merged (max), never replaced, and
  // the quota delta measured against the cached size so growth is counted once.
  const uint64_t write_end = offset + static_cast<uint64_t>(len);
  const uint64_t ts = now_ns();
  bool flush_now = false;
  std::int64_t quota_delta = 0;
  {
    std::lock_guard lock(st.mu);
    auto cit = st.inode_cache.find(ino);
    if (cit == st.inode_cache.end()) {
      cache_inode_locked(st, meta);
      cit = st.inode_cache.find(ino);
    }
    InodeMeta& c = cit != st.inode_cache.end() ? cit->second.meta : meta;
    const uint64_t before = c.size;
    const uint64_t after = std::max(before, write_end);
    c.size = after;
    c.mtime_ns = std::max(c.mtime_ns, ts);
    c.ctime_ns = std::max(c.ctime_ns, ts);
    if (cit != st.inode_cache.end()) cit->second.lru = ++st.inode_cache_clock;
    auto& d = st.dirty_sizes[ino];
    const auto now = std::chrono::steady_clock::now();
    if (d.dirty_bytes == 0) d.since = now;
    d.size = std::max(d.size, after);
    d.mtime_ns = std::max(d.mtime_ns, ts);
    d.ctime_ns = std::max(d.ctime_ns, ts);
    d.dirty_bytes += static_cast<uint64_t>(len);
    flush_now = d.dirty_bytes >= kDirtyFlushBytes || (now - d.since) >= kDirtyFlushAge;
    quota_delta = static_cast<std::int64_t>(after) - static_cast<std::int64_t>(before);
  }
  if (flush_now) {
    try {
      flush_dirty_inode(st, ino);
    } catch (const client_error& e) {
      return map_error(e);
    }
  }
  if (st.quota && quota_delta != 0) {
    st.quota->note_delta(meta.project_id, meta.uid, meta.gid, quota_delta);
  }
  if (meta.parent_ino != 0) mark_rstat_dirty(st, meta.parent_ino);
  if (out_len) *out_len = len;
  return 0;
}

int truncate_file(FsState& st, uint64_t ino, uint64_t size) {
  auto meta = load_inode(st, ino);
  if (!meta.exists) return -ENOENT;
  if (!S_ISREG(meta.mode)) return -EISDIR;
  if (size > meta.size && st.quota) {
    const auto grow = static_cast<std::int64_t>(size - meta.size);
    if (!st.quota->may_grow(meta.project_id, meta.uid, meta.gid, grow)) return -EDQUOT;
  }
  const uint64_t old_size = meta.size;
  const uint64_t unit = meta.stripe_unit ? meta.stripe_unit : st.stripe_unit;
  if (size < meta.size) {
    const uint64_t first_drop = (size + unit - 1) / unit;
    const uint64_t old_chunks = (meta.size + unit - 1) / unit;
    drop_dirty_chunks(st, ino, first_drop);
    if (size > 0) {
      const uint64_t last = (size - 1) / unit;
      const uint64_t keep = size - last * unit;
      resize_dirty_chunk(st, ino, last, static_cast<size_t>(keep));
    }
    try {
      flush_dirty_chunks(st, ino);
    } catch (const client_error& e) {
      return map_error(e);
    }
    st.chunk_cache.drop(ino);
    for (uint64_t c = first_drop; c < old_chunks; ++c) {
      try {
        st.session.delete_object(chunk_oid(st.volume, ino, c));
      } catch (...) {
      }
    }
    if (size > 0) {
      const uint64_t last = (size - 1) / unit;
      const uint64_t keep = size - last * unit;
      const auto data_layout = data_layout_for_ino(st, ino);
      for (int attempt = 0; attempt < 8; ++attempt) {
        try {
          auto snap = st.session.get_object(chunk_oid(st.volume, ino, last));
          if (!snap.exists || snap.body.size() <= keep) break;
          std::string body = snap.body;
          body.resize(static_cast<size_t>(keep));
          const uint64_t cas = cas_from_attrs(snap.attrs);
          st.session.put_bytes(chunk_oid(st.volume, ino, last), body, {}, cas, std::nullopt,
                               data_layout);
          break;
        } catch (const client_error& e) {
          if (e.code() == "conflict") continue;
          return map_error(e);
        }
      }
    }
  }
  const uint64_t ts = now_ns();
  meta.size = size;
  meta.mtime_ns = meta.ctime_ns = ts;
  {
    // The truncate supersedes any deferred size still waiting to be flushed.
    std::lock_guard lock(st.mu);
    st.dirty_sizes.erase(ino);
  }
  try {
    store_inode(st, meta, unpublished_create_path(st, ino), [size, ts](InodeMeta& next) {
      next.size = size;
      next.mtime_ns = next.ctime_ns = ts;
    });
  } catch (const client_error& e) {
    return map_error(e);
  }
  if (st.quota && size != old_size) {
    st.quota->note_delta(meta.project_id, meta.uid, meta.gid,
                         static_cast<std::int64_t>(size) - static_cast<std::int64_t>(old_size));
  }
  if (meta.parent_ino != 0) mark_rstat_dirty(st, meta.parent_ino);
  return 0;
}

void publish_inodes(FsState& st, const std::vector<uint64_t>& inos) {
  std::vector<uint64_t> want;
  want.reserve(inos.size());
  {
    std::lock_guard lock(st.mu);
    for (uint64_t ino : inos) {
      if (st.unpublished_dropped.count(ino)) continue;
      auto it = st.inode_cache.find(ino);
      if (it != st.inode_cache.end() && it->second.unpublished) want.push_back(ino);
    }
  }
  std::sort(want.begin(), want.end());
  want.erase(std::unique(want.begin(), want.end()), want.end());
  if (want.empty()) return;

  auto one = [&st](uint64_t ino) {
    InodeMeta m;
    std::optional<std::string> path;
    {
      std::lock_guard lock(st.mu);
      if (st.unpublished_dropped.count(ino)) return;
      auto it = st.inode_cache.find(ino);
      if (it == st.inode_cache.end() || !it->second.unpublished) return;
      m = it->second.meta;
      path = it->second.create_path;
      merge_dirty_locked(st, m);
    }
    store_inode(st, m, path);
    bool drop = false;
    {
      std::lock_guard lock(st.mu);
      drop = st.unpublished_dropped.erase(ino) > 0;
      if (drop) {
        st.inode_cache.erase(ino);
        st.dirty_sizes.erase(ino);
      }
    }
    if (drop) {
      try {
        st.session.delete_object(ino_oid(st.volume, ino));
      } catch (...) {
      }
    }
  };

  if (want.size() == 1) {
    one(want[0]);
    return;
  }
  const size_t nthreads = std::min<size_t>(8, want.size());
  std::atomic<size_t> next{0};
  std::exception_ptr err;
  std::mutex err_mu;
  std::vector<std::thread> workers;
  workers.reserve(nthreads);
  for (size_t t = 0; t < nthreads; ++t) {
    workers.emplace_back([&] {
      for (;;) {
        const size_t i = next.fetch_add(1);
        if (i >= want.size()) return;
        try {
          one(want[i]);
        } catch (...) {
          std::lock_guard lock(err_mu);
          if (!err) err = std::current_exception();
        }
      }
    });
  }
  for (auto& w : workers) w.join();
  if (err) std::rethrow_exception(err);
}

int commit_new_dentry(FsState& st, DirTable& dir, const std::string& name, InodeMeta& m,
                      const std::optional<std::string>& path, InodeMeta& pmeta, uint64_t ts,
                      int nlink_delta) {
  m.exists = true;
  if (st.leases) {
    auto l = st.leases->get(dir.ino(), dir.put_layout());
    if (l) {
      {
        std::lock_guard lock(st.mu);
        cache_unpublished_locked(st, m, path);
      }
      const int rc = st.leases->queue(*l, kOpLink, {name, std::to_string(m.ino)}, true, 0);
      if (rc == 0) {
        dir.load(true);
        touch_dir_inode(st, dir.ino(), pmeta, ts, nlink_delta);
        mark_rstat_dirty(st, dir.ino());
        return 0;
      }
      {
        std::lock_guard lock(st.mu);
        cache_erase_locked(st, m.ino);
        st.dirty_sizes.erase(m.ino);
      }
      if (rc == -EEXIST) return -EEXIST;
      st.leases->drop(dir.ino());
    }
  }
  store_inode(st, m, path);
  if (!dir.link_if_absent(name, m.ino)) {
    delete_orphan_inode(st, m.ino);
    return -EEXIST;
  }
  touch_dir_inode(st, dir.ino(), pmeta, ts, nlink_delta);
  mark_rstat_dirty(st, dir.ino());
  return 0;
}

}  // namespace posix
}  // namespace aios

using aios::posix::FsState;
using aios::posix::kRootIno;

struct aios_posix_fs {
  std::unique_ptr<FsState> st;
};

namespace {

struct CallerSlot {
  bool set{false};
  uint32_t uid{0};
  uint32_t gid{0};
};

thread_local std::unordered_map<const aios_posix_fs*, CallerSlot> g_tls_callers;

aios_posix_cred effective_caller(const aios_posix_fs* fs) {
  aios_posix_cred c{};
  if (!fs || !fs->st) return c;
  c.uid = fs->st->default_uid;
  c.gid = fs->st->default_gid;
  auto it = g_tls_callers.find(fs);
  if (it != g_tls_callers.end() && it->second.set) {
    c.uid = it->second.uid;
    c.gid = it->second.gid;
  }
  return c;
}

constexpr int kWantR = 4;
constexpr int kWantW = 2;
constexpr int kWantX = 1;

int ensure_not_frozen(aios_posix_fs* fs) {
  if (!fs || !fs->st) return -EINVAL;
  aios::posix::ensure_super(*fs->st);
  {
    std::lock_guard lock(fs->st->mu);
    if (fs->st->super.frozen) return -EBUSY;
  }
  return 0;
}

int link_existing_ino(aios_posix_fs* fs, uint64_t ino, uint64_t new_parent, const char* new_name) {
  const auto cred = effective_caller(fs);
  auto m = aios::posix::load_inode(*fs->st, ino);
  if (!m.exists) return -ENOENT;
  if (S_ISDIR(m.mode)) return -EPERM;

  auto new_dir = aios::posix::make_dir(*fs->st, new_parent);
  new_dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, new_parent));
  new_dir.load();
  if (new_dir.entries().count(new_name)) return -EEXIST;
  auto np = aios::posix::load_inode(*fs->st, new_parent);
  if (!np.exists) return -ENOENT;
  if (!S_ISDIR(np.mode)) return -ENOTDIR;
  if (int ac = aios::posix::check_access(cred, np, kWantW | kWantX)) return ac;

  // Bump nlink before creating the new dentry (safer on crash).
  // Primary parent_ino is unchanged; only the destination dir is dirty for rstat.
  const uint64_t ts = aios::posix::now_ns();
  m.nlink += 1;
  m.ctime_ns = ts;
  const auto path = aios::posix::unpublished_create_path(*fs->st, ino);
  aios::posix::store_inode(*fs->st, m, path, [ts](aios::posix::InodeMeta& next) {
    next.nlink += 1;
    next.ctime_ns = ts;
  });
  if (!new_dir.link_if_absent(new_name, ino)) {
    aios::posix::store_inode(*fs->st, m, path, [ts](aios::posix::InodeMeta& next) {
      if (next.nlink > 0) next.nlink -= 1;
      next.ctime_ns = ts;
    });
    return -EEXIST;
  }
  aios::posix::touch_dir_inode(*fs->st, new_parent, np, ts, 0);
  aios::posix::mark_rstat_dirty(*fs->st, new_parent);
  return 0;
}

}  // namespace

extern "C" {

void aios_posix_set_caller(aios_posix_fs* fs, uint32_t uid, uint32_t gid) {
  if (!fs) return;
  auto& slot = g_tls_callers[fs];
  slot.set = true;
  slot.uid = uid;
  slot.gid = gid;
}

void aios_posix_clear_caller(aios_posix_fs* fs) {
  if (!fs) return;
  g_tls_callers.erase(fs);
}

aios_posix_cred aios_posix_get_caller(const aios_posix_fs* fs) { return effective_caller(fs); }

int aios_posix_access(aios_posix_fs* fs, uint64_t ino, int amode) {
  if (!fs) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (amode == F_OK) return 0;
    int want = 0;
    if (amode & R_OK) want |= kWantR;
    if (amode & W_OK) want |= kWantW;
    if (amode & X_OK) want |= kWantX;
    return aios::posix::check_access(effective_caller(fs), m, want);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

aios_posix_fs* aios_posix_mount(const aios_posix_config* cfg, int* err_out) {
  if (err_out) *err_out = 0;
  if (!cfg || !cfg->endpoint || !cfg->cluster_key) {
    if (err_out) *err_out = EINVAL;
    return nullptr;
  }
  try {
    aios::SessionConfig sc;
    sc.endpoint = cfg->endpoint;
    sc.cluster_key = cfg->cluster_key;
    if (cfg->tls_ca && cfg->tls_ca[0]) sc.tls_ca = cfg->tls_ca;
    sc.tls_insecure = (cfg->flags & AIOS_POSIX_F_TLS_INSECURE) != 0;
    // Default frontend label "fs" so FUSE/posix object OPS and logical IO are separated from S3/VBD.
    if (cfg->app_label && cfg->app_label[0]) sc.app_label = cfg->app_label;
    else sc.app_label = aios::kFrontendFs;
    auto fs = new aios_posix_fs;
    fs->st = std::make_unique<FsState>(std::move(sc));
    fs->st->volume = (cfg->volume && cfg->volume[0]) ? cfg->volume : "default";
    fs->st->stripe_unit = cfg->stripe_unit ? cfg->stripe_unit : aios::posix::kDefaultStripeUnit;
    fs->st->stripe_width = cfg->stripe_width ? cfg->stripe_width : aios::posix::kDefaultStripeWidth;
    fs->st->default_uid = cfg->uid;
    fs->st->default_gid = cfg->gid;
    fs->st->frontend_label = fs->st->session.app_label().empty() ? aios::kFrontendFs
                                                                 : fs->st->session.app_label();
    fs->st->quota = std::make_unique<aios::posix::QuotaLedger>(fs->st->session, fs->st->volume);
    fs->st->qos = std::make_unique<aios::posix::QosController>(fs->st->session, fs->st->volume);
    fs->st->rstat_interval_ms = cfg->rstat_interval_ms;
    fs->st->no_lease = (cfg->flags & AIOS_POSIX_F_NOLEASE) != 0;
    if (const char* env = std::getenv("AIOS_POSIX_NOLEASE")) {
      if (env[0] && env[0] != '0') fs->st->no_lease = true;
    }
    aios::posix::ensure_super(*fs->st);
    aios::posix::ensure_root(*fs->st);
    if (!fs->st->no_lease) {
      fs->st->leases = std::make_unique<aios::posix::DirLeaseManager>(*fs->st);
      fs->st->leases->start();
    }
    aios::posix::start_rstat_thread(*fs->st);
    return fs;
  } catch (const aios::client_error& e) {
    if (err_out) *err_out = -aios::posix::map_error(e);
    return nullptr;
  } catch (...) {
    if (err_out) *err_out = EIO;
    return nullptr;
  }
}

void aios_posix_unmount(aios_posix_fs* fs) {
  if (!fs) return;
  g_tls_callers.erase(fs);
  if (fs->st) {
    // Queued directory records first: they must be on the server before the
    // session goes away, and this hands every lease back.
    if (fs->st->leases) fs->st->leases->stop();
    aios::posix::stop_rstat_thread(*fs->st);
    aios::posix::flush_all_dirty_inodes(*fs->st);
    if (fs->st->quota) fs->st->quota->flush();
    aios::posix::release_all_flocks(*fs->st);
  }
  delete fs;
}

int aios_posix_fsyncdir(aios_posix_fs* fs, uint64_t dir_ino) {
  if (!fs || !fs->st) return -EINVAL;
  if (!fs->st->leases) return 0;
  try {
    return fs->st->leases->fsync(dir_ino);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_sync(aios_posix_fs* fs) {
  if (!fs || !fs->st) return -EINVAL;
  try {
    aios::posix::flush_all_dirty_inodes(*fs->st);
    if (!fs->st->leases) return 0;
    return fs->st->leases->sync_all(false);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

uint64_t aios_posix_stripe_unit(const aios_posix_fs* fs) {
  return (fs && fs->st) ? fs->st->stripe_unit : 0;
}

void aios_posix_flush_rstats(aios_posix_fs* fs) {
  if (!fs || !fs->st) return;
  try {
    aios::posix::flush_rstats(*fs->st);
  } catch (...) {
  }
}

int aios_posix_lookup(aios_posix_fs* fs, uint64_t parent, const char* name,
                      aios_posix_stat* st_out) {
  if (!fs || !name || !st_out) return -EINVAL;
  try {
    if (std::strcmp(name, ".") == 0) return aios_posix_getattr(fs, parent, st_out);
    if (std::strcmp(name, "..") == 0) {
      // v1: parent of root is root; otherwise unknown → ENOENT for ..
      if (parent == kRootIno) return aios_posix_getattr(fs, kRootIno, st_out);
      return -ENOENT;
    }
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_access(effective_caller(fs), pmeta, kWantX)) return ac;
    auto dir = aios::posix::make_dir(*fs->st, parent);
    dir.load();
    auto it = dir.entries().find(name);
    if (it == dir.entries().end()) return -ENOENT;
    return aios_posix_getattr(fs, it->second, st_out);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_getattr(aios_posix_fs* fs, uint64_t ino, aios_posix_stat* st_out) {
  if (!fs || !st_out) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    aios::posix::fill_stat(m, st_out);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_readdir(aios_posix_fs* fs, uint64_t ino, uint64_t* offset,
                       aios_posix_dirent* buf, size_t max_entries) {
  if (!fs || !offset || !buf || max_entries == 0) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (!S_ISDIR(m.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantR)) return ac;
    auto dir = aios::posix::make_dir(*fs->st, ino);
    dir.load();
    std::vector<std::pair<std::string, uint64_t>> items;
    items.emplace_back(".", ino);
    items.emplace_back("..", ino == kRootIno ? kRootIno : ino);
    for (const auto& [n, i] : dir.entries()) items.emplace_back(n, i);
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    // d_type needs each child's mode; fetch the uncached ones of this batch in
    // parallel instead of one GET after another.
    {
      std::vector<uint64_t> want;
      const size_t end = std::min(items.size(), static_cast<size_t>(*offset) + max_entries);
      for (size_t i = static_cast<size_t>(*offset); i < end; ++i) want.push_back(items[i].second);
      aios::posix::prefetch_inodes(*fs->st, want);
    }
    size_t nwrite = 0;
    while (*offset < items.size() && nwrite < max_entries) {
      const auto& [name, child] = items[static_cast<size_t>(*offset)];
      ++*offset;
      if (name.size() >= sizeof(buf[0].name)) continue;
      auto cm = aios::posix::load_inode(*fs->st, child);
      aios_posix_dirent& d = buf[nwrite];
      std::memset(&d, 0, sizeof(d));
      d.ino = child;
      d.mode = cm.exists ? (cm.mode & S_IFMT) : S_IFREG;
      std::snprintf(d.name, sizeof(d.name), "%s", name.c_str());
      ++nwrite;
    }
    return static_cast<int>(nwrite);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_mkdir(aios_posix_fs* fs, uint64_t parent, const char* name, uint32_t mode,
                     aios_posix_stat* st_out) {
  if (!fs || !name) return -EINVAL;
  if (int rc = aios::posix::validate_dentry_name(name)) return rc;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    const auto cred = effective_caller(fs);
    if (int ac = aios::posix::check_access(cred, pmeta, kWantW | kWantX)) return ac;
    auto dir = aios::posix::make_dir(*fs->st, parent);
    dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, parent));
    dir.load();
    if (dir.entries().count(name)) return -EEXIST;
    const uint64_t ino = aios::posix::alloc_ino(*fs->st);
    const uint64_t ts = aios::posix::now_ns();
    aios::posix::InodeMeta m;
    m.ino = ino;
    m.mode = S_IFDIR | (mode & 0777);
    m.nlink = 2;
    m.uid = cred.uid;
    m.gid = cred.gid;
    m.project_id = pmeta.project_id;
    m.parent_ino = parent;
    m.atime_ns = m.mtime_ns = m.ctime_ns = ts;
    m.stripe_unit = fs->st->stripe_unit;
    m.stripe_width = fs->st->stripe_width;
    if (int rc = aios::posix::commit_new_dentry(
            *fs->st, dir, name, m, aios::posix::child_path_for_layout(*fs->st, parent, name), pmeta,
            ts, 1))
      return rc;
    if (st_out) aios::posix::fill_stat(m, st_out);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_create(aios_posix_fs* fs, uint64_t parent, const char* name, uint32_t mode,
                      aios_posix_stat* st_out) {
  if (!fs || !name) return -EINVAL;
  if (int rc = aios::posix::validate_dentry_name(name)) return rc;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    const auto cred = effective_caller(fs);
    if (int ac = aios::posix::check_access(cred, pmeta, kWantW | kWantX)) return ac;
    auto dir = aios::posix::make_dir(*fs->st, parent);
    dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, parent));
    dir.load();
    if (dir.entries().count(name)) return -EEXIST;
    const uint64_t ino = aios::posix::alloc_ino(*fs->st);
    const uint64_t ts = aios::posix::now_ns();
    aios::posix::InodeMeta m;
    m.ino = ino;
    m.mode = S_IFREG | (mode & 0777);
    m.nlink = 1;
    m.uid = cred.uid;
    m.gid = cred.gid;
    m.project_id = pmeta.project_id;
    m.parent_ino = parent;
    m.atime_ns = m.mtime_ns = m.ctime_ns = ts;
    m.stripe_unit = fs->st->stripe_unit;
    m.stripe_width = fs->st->stripe_width;
    if (int rc = aios::posix::commit_new_dentry(
            *fs->st, dir, name, m, aios::posix::child_path_for_layout(*fs->st, parent, name), pmeta,
            ts, 0))
      return rc;
    if (st_out) aios::posix::fill_stat(m, st_out);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_symlink(aios_posix_fs* fs, uint64_t parent, const char* name, const char* target,
                       aios_posix_stat* st_out) {
  if (!fs || !name || !target) return -EINVAL;
  if (int rc = aios::posix::validate_dentry_name(name)) return rc;
  const size_t tlen = std::strlen(target);
  if (tlen == 0 || tlen > aios::posix::kMaxSymlinkBytes) return -ENAMETOOLONG;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    const auto cred = effective_caller(fs);
    if (int ac = aios::posix::check_access(cred, pmeta, kWantW | kWantX)) return ac;
    auto dir = aios::posix::make_dir(*fs->st, parent);
    dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, parent));
    dir.load();
    if (dir.entries().count(name)) return -EEXIST;
    const uint64_t ino = aios::posix::alloc_ino(*fs->st);
    const uint64_t ts = aios::posix::now_ns();
    aios::posix::InodeMeta m;
    m.ino = ino;
    m.mode = S_IFLNK | 0777;
    m.nlink = 1;
    m.uid = cred.uid;
    m.gid = cred.gid;
    m.project_id = pmeta.project_id;
    m.parent_ino = parent;
    m.size = static_cast<uint64_t>(tlen);
    m.symlink.assign(target, tlen);
    m.atime_ns = m.mtime_ns = m.ctime_ns = ts;
    m.stripe_unit = fs->st->stripe_unit;
    m.stripe_width = fs->st->stripe_width;
    if (int rc = aios::posix::commit_new_dentry(
            *fs->st, dir, name, m, aios::posix::child_path_for_layout(*fs->st, parent, name), pmeta,
            ts, 0))
      return rc;
    if (st_out) aios::posix::fill_stat(m, st_out);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_readlink(aios_posix_fs* fs, uint64_t ino, char* buf, size_t size) {
  if (!fs) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (!S_ISLNK(m.mode)) return -EINVAL;
    const size_t need = m.symlink.size() + 1;
    if (size == 0) return static_cast<int>(need);
    if (!buf) return -EINVAL;
    if (size < need) return -ERANGE;
    std::memcpy(buf, m.symlink.data(), m.symlink.size());
    buf[m.symlink.size()] = '\0';
    return static_cast<int>(m.symlink.size());
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_unlink(aios_posix_fs* fs, uint64_t parent, const char* name) {
  if (!fs || !name) return -EINVAL;
  if (int rc = aios::posix::validate_dentry_name(name)) return rc;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    const auto cred = effective_caller(fs);
    if (int ac = aios::posix::check_access(cred, pmeta, kWantW | kWantX)) return ac;
    auto dir = aios::posix::make_dir(*fs->st, parent);
    dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, parent));
    dir.load();
    auto it = dir.entries().find(name);
    if (it == dir.entries().end()) return -ENOENT;
    const uint64_t ino = it->second;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (m.exists && S_ISDIR(m.mode)) return -EISDIR;
    if (m.exists) {
      if (int ac = aios::posix::check_sticky_unlink(cred, pmeta, m)) return ac;
    }
    // Remove the dentry only while it still points at the inode we inspected; a
    // peer that unlinked and recreated the name in between must not lose its file.
    if (int rc = dir.unlink_if(name, ino)) return rc;
    if (m.exists) aios::posix::drop_nlink(*fs->st, ino);
    aios::posix::mark_rstat_dirty(*fs->st, parent);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_link(aios_posix_fs* fs, uint64_t old_parent, const char* old_name,
                    uint64_t new_parent, const char* new_name) {
  if (!fs || !old_name || !new_name) return -EINVAL;
  if (int rc = aios::posix::validate_dentry_name(new_name)) return rc;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    const auto cred = effective_caller(fs);
    auto op = aios::posix::load_inode(*fs->st, old_parent);
    if (!op.exists) return -ENOENT;
    if (!S_ISDIR(op.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_access(cred, op, kWantX)) return ac;
    auto old_dir = aios::posix::make_dir(*fs->st, old_parent);
    old_dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, old_parent));
    old_dir.load();
    auto it = old_dir.entries().find(old_name);
    if (it == old_dir.entries().end()) return -ENOENT;
    return link_existing_ino(fs, it->second, new_parent, new_name);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_link_ino(aios_posix_fs* fs, uint64_t ino, uint64_t new_parent, const char* new_name) {
  if (!fs || !new_name) return -EINVAL;
  if (int rc = aios::posix::validate_dentry_name(new_name)) return rc;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    return link_existing_ino(fs, ino, new_parent, new_name);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_rmdir(aios_posix_fs* fs, uint64_t parent, const char* name) {
  if (!fs || !name) return -EINVAL;
  if (int rc = aios::posix::validate_dentry_name(name)) return rc;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    const auto cred = effective_caller(fs);
    if (int ac = aios::posix::check_access(cred, pmeta, kWantW | kWantX)) return ac;
    auto dir = aios::posix::make_dir(*fs->st, parent);
    dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, parent));
    dir.load();
    auto it = dir.entries().find(name);
    if (it == dir.entries().end()) return -ENOENT;
    const uint64_t ino = it->second;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists || !S_ISDIR(m.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_sticky_unlink(cred, pmeta, m)) return ac;
    auto child = aios::posix::make_dir(*fs->st, ino);
    child.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, ino));
    child.load();
    if (!child.entries().empty()) return -ENOTEMPTY;
    // Our own lease on the child would refuse the locks taken below.
    if (fs->st->leases) fs->st->leases->drop(ino);
    // Re-check emptiness with the child's tip locked so a concurrent create in it
    // cannot slip between the check and the removal.
    int rc = dir.unlink_if(name, ino, {child.meta_oid(), child.log_oid(), child.snap_oid()},
                           [&child]() -> int {
                             child.load(false);
                             return child.entries().empty() ? 0 : -ENOTEMPTY;
                           });
    if (rc) return rc;
    {
      std::lock_guard lock(fs->st->mu);
      auto cit = fs->st->inode_cache.find(ino);
      if (cit != fs->st->inode_cache.end() && cit->second.unpublished) {
        fs->st->unpublished_dropped.insert(ino);
      }
    }
    fs->st->session.delete_object(aios::posix::ino_oid(fs->st->volume, ino));
    aios::posix::touch_dir_inode(*fs->st, parent, pmeta, aios::posix::now_ns(), -1);
    {
      std::lock_guard lock(fs->st->mu);
      fs->st->inode_cache.erase(ino);
      fs->st->dir_cache.erase(ino);
    }
    aios::posix::mark_rstat_dirty(*fs->st, parent);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_rename(aios_posix_fs* fs, uint64_t old_parent, const char* old_name,
                      uint64_t new_parent, const char* new_name) {
  return aios_posix_rename2(fs, old_parent, old_name, new_parent, new_name, 0);
}

int aios_posix_rename2(aios_posix_fs* fs, uint64_t old_parent, const char* old_name,
                       uint64_t new_parent, const char* new_name, unsigned flags) {
  if (!fs || !old_name || !new_name) return -EINVAL;
  if (flags & (AIOS_POSIX_RENAME_EXCHANGE | AIOS_POSIX_RENAME_WHITEOUT)) return -EINVAL;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    const auto cred = effective_caller(fs);
    auto op_meta = aios::posix::load_inode(*fs->st, old_parent);
    if (!op_meta.exists) return -ENOENT;
    if (!S_ISDIR(op_meta.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_access(cred, op_meta, kWantW | kWantX)) return ac;
    auto np_meta = aios::posix::load_inode(*fs->st, new_parent);
    if (!np_meta.exists) return -ENOENT;
    if (!S_ISDIR(np_meta.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_access(cred, np_meta, kWantW | kWantX)) return ac;

    // Cross layout-rule rename is not in-place; clients should copy (EXDEV).
    if (aios::posix::layout_rules_present(*fs->st)) {
      auto join_path = [](const std::string& dir, const char* name) {
        if (dir == "/") return std::string("/") + name;
        return dir + "/" + name;
      };
      const std::string src =
          join_path(aios::posix::path_of_ino(*fs->st, old_parent), old_name);
      const std::string dst =
          join_path(aios::posix::path_of_ino(*fs->st, new_parent), new_name);
      if (aios::posix::layout_domains_differ(*fs->st, src, dst)) return -EXDEV;
    }

    if (old_parent == new_parent) {
      auto dir = aios::posix::make_dir(*fs->st, old_parent);
      dir.load();
      auto oit = dir.entries().find(old_name);
      if (oit == dir.entries().end()) return -ENOENT;
      auto src = aios::posix::load_inode(*fs->st, oit->second);
      if (src.exists) {
        if (int ac = aios::posix::check_sticky_unlink(cred, op_meta, src)) return ac;
      }
      if (dir.entries().count(new_name) && std::strcmp(old_name, new_name) != 0) {
        if (flags & AIOS_POSIX_RENAME_NOREPLACE) return -EEXIST;
        auto tit = dir.entries().find(new_name);
        auto tm = aios::posix::load_inode(*fs->st, tit->second);
        if (tm.exists) {
          if (int ac = aios::posix::check_sticky_unlink(cred, op_meta, tm)) return ac;
        }
      }
      return aios::posix::rename_same_dir(*fs->st, old_parent, old_name, new_name);
    }
    // Cross-dir: sticky checks on source and optional victim.
    {
      auto dir = aios::posix::make_dir(*fs->st, old_parent);
      dir.load();
      auto oit = dir.entries().find(old_name);
      if (oit == dir.entries().end()) return -ENOENT;
      auto src = aios::posix::load_inode(*fs->st, oit->second);
      if (src.exists) {
        if (int ac = aios::posix::check_sticky_unlink(cred, op_meta, src)) return ac;
      }
      auto ndir = aios::posix::make_dir(*fs->st, new_parent);
      ndir.load();
      auto tit = ndir.entries().find(new_name);
      if (tit != ndir.entries().end()) {
        if (flags & AIOS_POSIX_RENAME_NOREPLACE) return -EEXIST;
        auto tm = aios::posix::load_inode(*fs->st, tit->second);
        if (tm.exists) {
          if (int ac = aios::posix::check_sticky_unlink(cred, np_meta, tm)) return ac;
        }
      }
    }
    return aios::posix::rename_cross_dir(*fs->st, old_parent, old_name, new_parent, new_name);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_read(aios_posix_fs* fs, uint64_t ino, uint64_t offset, void* buf, size_t len,
                    size_t* out_len) {
  if (!fs || !buf) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantR)) return ac;
    return aios::posix::read_file(*fs->st, ino, offset, buf, len, out_len);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_prefetchv(aios_posix_fs* fs, uint64_t ino, const struct aios_prefetchv* req) {
  if (!fs || !req) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantR)) return ac;
    struct aios_prefetchv copy = *req;
    return aios::posix::prefetch_file(*fs->st, ino, copy.ranges, copy.nranges, copy.flags);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_write(aios_posix_fs* fs, uint64_t ino, uint64_t offset, const void* buf,
                     size_t len, size_t* out_len) {
  if (!fs || !buf) return -EINVAL;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantW)) return ac;
    return aios::posix::write_file(*fs->st, ino, offset, buf, len, out_len);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_truncate(aios_posix_fs* fs, uint64_t ino, uint64_t size) {
  if (!fs) return -EINVAL;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantW)) return ac;
    return aios::posix::truncate_file(*fs->st, ino, size);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_setattr(aios_posix_fs* fs, uint64_t ino, const aios_posix_stat* st,
                       uint32_t to_set) {
  if (!fs || !st) return -EINVAL;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    const auto cred = effective_caller(fs);
    if (to_set & (AIOS_POSIX_SET_UID | AIOS_POSIX_SET_GID)) {
      if (cred.uid != 0) return -EPERM;
    }
    if (to_set & AIOS_POSIX_SET_MODE) {
      if (cred.uid != 0 && cred.uid != m.uid) return -EPERM;
    }
    if (to_set & (AIOS_POSIX_SET_SIZE | AIOS_POSIX_SET_MTIME | AIOS_POSIX_SET_ATIME)) {
      if (int ac = aios::posix::check_access(cred, m, kWantW)) return ac;
    }
    const auto old_uid = m.uid;
    const auto old_gid = m.gid;
    const uint64_t ts = aios::posix::now_ns();
    const uint32_t new_mode = st->mode;
    const uint32_t new_uid = st->uid;
    const uint32_t new_gid = st->gid;
    const uint64_t new_mtime = st->mtime_ns;
    const uint64_t new_atime = st->atime_ns;
    if (to_set & AIOS_POSIX_SET_MODE) m.mode = (m.mode & S_IFMT) | (st->mode & 07777);
    if (to_set & AIOS_POSIX_SET_UID) m.uid = st->uid;
    if (to_set & AIOS_POSIX_SET_GID) m.gid = st->gid;
    if (to_set & AIOS_POSIX_SET_MTIME) m.mtime_ns = st->mtime_ns;
    if (to_set & AIOS_POSIX_SET_ATIME) m.atime_ns = st->atime_ns;
    m.ctime_ns = ts;
    aios::posix::store_inode(*fs->st, m, aios::posix::unpublished_create_path(*fs->st, ino),
                             [to_set, new_mode, new_uid, new_gid, new_mtime, new_atime,
                              ts](aios::posix::InodeMeta& next) {
                               if (to_set & AIOS_POSIX_SET_MODE) {
                                 next.mode = (next.mode & S_IFMT) | (new_mode & 07777);
                               }
                               if (to_set & AIOS_POSIX_SET_UID) next.uid = new_uid;
                               if (to_set & AIOS_POSIX_SET_GID) next.gid = new_gid;
                               if (to_set & AIOS_POSIX_SET_MTIME) next.mtime_ns = new_mtime;
                               if (to_set & AIOS_POSIX_SET_ATIME) next.atime_ns = new_atime;
                               next.ctime_ns = ts;
                             });
    if (fs->st->quota && (to_set & (AIOS_POSIX_SET_UID | AIOS_POSIX_SET_GID)) &&
        (old_uid != m.uid || old_gid != m.gid)) {
      fs->st->quota->note_chown(m.project_id, old_uid, old_gid, m.uid, m.gid, m.size);
    }
    if (to_set & AIOS_POSIX_SET_SIZE) {
      int rc = aios::posix::truncate_file(*fs->st, ino, st->size);
      if (rc) return rc;
    }
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_fsync(aios_posix_fs* fs, uint64_t ino) {
  if (!fs) return -EINVAL;
  try {
    aios::posix::flush_dirty_inode(*fs->st, ino);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  } catch (...) {
    return -EIO;
  }
}

int aios_posix_statfs(aios_posix_fs* fs, aios_posix_statvfs* st_out) {
  if (!fs || !st_out) return -EINVAL;
  std::memset(st_out, 0, sizeof(*st_out));
  st_out->bsize = fs->st ? static_cast<uint32_t>(fs->st->stripe_unit ? fs->st->stripe_unit : 4096)
                         : 4096;
  st_out->blocks = 1ull << 40;
  st_out->bfree = st_out->blocks / 2;
  st_out->bavail = st_out->bfree;
  st_out->files = 1ull << 32;
  st_out->ffree = st_out->files / 2;
  st_out->namemax = 255;
  return 0;
}

int aios_posix_setxattr(aios_posix_fs* fs, uint64_t ino, const char* name, const void* value,
                        size_t size, int flags) {
  if (!fs || !aios::posix::valid_xattr_name(name)) return -EINVAL;
  if (aios::posix::is_rstat_xattr(name)) return -EPERM;
  if (size > aios::posix::kMaxXattrValue) return -E2BIG;
  if (size > 0 && !value) return -EINVAL;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    const auto cred = effective_caller(fs);
    if (cred.uid != 0 && cred.uid != m.uid) {
      if (int ac = aios::posix::check_access(cred, m, kWantW)) return ac;
    }
    const bool present = m.xattrs.count(name) != 0;
    if ((flags & AIOS_POSIX_XATTR_CREATE) && present) return -EEXIST;
    if ((flags & AIOS_POSIX_XATTR_REPLACE) && !present) return -aios::posix::kXattrMissing;
    if (!present && m.xattrs.size() >= aios::posix::kMaxXattrCount) return -ENOSPC;
    const std::string xname = name;
    const std::string xval(static_cast<const char*>(value), size);
    const uint64_t ts = aios::posix::now_ns();
    m.xattrs[xname] = xval;
    m.ctime_ns = ts;
    aios::posix::store_inode(*fs->st, m, aios::posix::unpublished_create_path(*fs->st, ino),
                             [xname, xval, ts](aios::posix::InodeMeta& next) {
                               next.xattrs[xname] = xval;
                               next.ctime_ns = ts;
                             });
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_getxattr(aios_posix_fs* fs, uint64_t ino, const char* name, void* value,
                        size_t size) {
  if (!fs || !aios::posix::valid_xattr_name(name)) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantR)) return ac;
    if (aios::posix::is_rstat_xattr(name)) {
      return aios::posix::get_rstat_xattr(m, name, value, size);
    }
    auto it = m.xattrs.find(name);
    if (it == m.xattrs.end()) return -aios::posix::kXattrMissing;
    if (size == 0) return static_cast<int>(it->second.size());
    if (size < it->second.size()) return -ERANGE;
    if (value && !it->second.empty()) {
      std::memcpy(value, it->second.data(), it->second.size());
    }
    return static_cast<int>(it->second.size());
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_listxattr(aios_posix_fs* fs, uint64_t ino, char* list, size_t size) {
  if (!fs) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantR)) return ac;
    static const char* kVirt[] = {aios::posix::kRstatXattrRbytes, aios::posix::kRstatXattrRfiles,
                                  aios::posix::kRstatXattrRdirs, aios::posix::kRstatXattrRtime};
    size_t need = 0;
    for (const auto& [k, _] : m.xattrs) need += k.size() + 1;
    if (S_ISDIR(m.mode)) {
      for (const char* v : kVirt) need += std::strlen(v) + 1;
    }
    if (size == 0) return static_cast<int>(need);
    if (size < need) return -ERANGE;
    if (!list && need > 0) return -EINVAL;
    size_t off = 0;
    for (const auto& [k, _] : m.xattrs) {
      std::memcpy(list + off, k.data(), k.size());
      off += k.size();
      list[off++] = '\0';
    }
    if (S_ISDIR(m.mode)) {
      for (const char* v : kVirt) {
        const size_t n = std::strlen(v);
        std::memcpy(list + off, v, n);
        off += n;
        list[off++] = '\0';
      }
    }
    return static_cast<int>(need);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_removexattr(aios_posix_fs* fs, uint64_t ino, const char* name) {
  if (!fs || !aios::posix::valid_xattr_name(name)) return -EINVAL;
  if (aios::posix::is_rstat_xattr(name)) return -EPERM;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    const auto cred = effective_caller(fs);
    if (cred.uid != 0 && cred.uid != m.uid) {
      if (int ac = aios::posix::check_access(cred, m, kWantW)) return ac;
    }
    if (!m.xattrs.erase(name)) return -aios::posix::kXattrMissing;
    const std::string xname = name;
    const uint64_t ts = aios::posix::now_ns();
    m.ctime_ns = ts;
    aios::posix::store_inode(*fs->st, m, aios::posix::unpublished_create_path(*fs->st, ino),
                             [xname, ts](aios::posix::InodeMeta& next) {
      next.xattrs.erase(xname);
      next.ctime_ns = ts;
    });
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

}  // extern "C"

namespace {

std::string normalize_snap_path(const char* path) {
  if (!path || !*path) return "/";
  std::string p(path);
  if (p.front() != '/') p = "/" + p;
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

int resolve_path_ino_local(aios_posix_fs* fs, const std::string& norm, uint64_t* out_ino) {
  uint64_t ino = aios::posix::kRootIno;
  if (norm == "/") {
    *out_ino = ino;
    return 0;
  }
  std::string rest = norm.substr(1);
  while (!rest.empty()) {
    const auto slash = rest.find('/');
    const std::string comp = slash == std::string::npos ? rest : rest.substr(0, slash);
    rest = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    if (comp.empty() || comp == ".") continue;
    if (comp == "..") return -EINVAL;
    aios_posix_stat st{};
    int rc = aios_posix_lookup(fs, ino, comp.c_str(), &st);
    if (rc) return rc;
    ino = st.ino;
  }
  *out_ino = ino;
  return 0;
}

void collect_subtree_oids_session(aios::posix::FsState& st, uint64_t root_ino,
                                  std::vector<std::string>& oids) {
  oids.clear();
  oids.push_back(aios::posix::super_oid(st.volume));
  std::queue<uint64_t> q;
  std::unordered_set<uint64_t> seen;
  q.push(root_ino);
  seen.insert(root_ino);
  while (!q.empty()) {
    const uint64_t ino = q.front();
    q.pop();
    auto m = aios::posix::load_inode(st, ino);
    if (!m.exists) continue;
    oids.push_back(aios::posix::ino_oid(st.volume, ino));
    if (S_ISDIR(m.mode)) {
      oids.push_back(aios::posix::dir_meta_oid(st.volume, ino));
      oids.push_back(aios::posix::dir_log_oid(st.volume, ino));
      oids.push_back(aios::posix::dir_snap_oid(st.volume, ino));
      auto dt = aios::posix::make_dir(st, ino);
      dt.load();
      for (const auto& [name, child] : dt.entries()) {
        (void)name;
        if (seen.insert(child).second) q.push(child);
      }
    } else if (S_ISREG(m.mode)) {
      const uint64_t stripe = m.stripe_unit ? m.stripe_unit : aios::posix::kDefaultStripeUnit;
      const uint64_t nchunk = m.size == 0 ? 0 : (m.size + stripe - 1) / stripe;
      for (uint64_t c = 0; c < nchunk; ++c) {
        oids.push_back(aios::posix::chunk_oid(st.volume, ino, c));
      }
    }
  }
}

}  // namespace

extern "C" {

int aios_posix_snapshot(aios_posix_fs* fs, char* snap_id_out, size_t snap_id_len) {
  return aios_posix_snapshot_at(fs, "/", snap_id_out, snap_id_len);
}

int aios_posix_snapshot_at(aios_posix_fs* fs, const char* path, char* snap_id_out,
                           size_t snap_id_len) {
  if (!fs || !fs->st || !snap_id_out || snap_id_len < 17) return -EINVAL;
  try {
    aios::posix::ensure_super(*fs->st);
    auto& st = *fs->st;
    aios::posix::flush_all_dirty_inodes(st);
    const std::string norm = normalize_snap_path(path);
    uint64_t root_ino = aios::posix::kRootIno;
    if (norm != "/") {
      if (int rc = resolve_path_ino_local(fs, norm, &root_ino)) return rc;
    }
    {
      aios::posix::SuperMeta m = st.super;
      m.frozen = true;
      m.cas = st.session.put_bytes(aios::posix::super_oid(st.volume), aios::posix::super_to_json(m),
                                   {}, m.cas);
      m.exists = true;
      st.super = m;
    }
    // Directory records queued under leases must be on the server before the
    // tips are read; new ones are refused while frozen.
    if (st.leases) st.leases->sync_all(false);
    const auto sid = [&]() {
      static thread_local std::mt19937_64 rng{
          static_cast<std::uint64_t>(
              std::chrono::steady_clock::now().time_since_epoch().count())};
      std::ostringstream oss;
      oss << std::hex << rng() << rng();
      return oss.str();
    }();
    const std::string live_prefix = "posix/" + st.volume + "/";
    const std::string snap_prefix = live_prefix + ".snap/" + sid + "/";
    const std::string snap_marker = live_prefix + ".snap/";
    std::size_t copied = 0;
    try {
      std::vector<std::string> src_oids;
      if (norm == "/") {
        std::string cursor;
        for (;;) {
          auto page = st.session.list_prefix(live_prefix, 256, cursor);
          for (const auto& e : page.objects) {
            if (e.oid.rfind(snap_marker, 0) == 0) continue;
            src_oids.push_back(e.oid);
          }
          if (page.next_cursor.empty()) break;
          cursor = page.next_cursor;
        }
      } else {
        collect_subtree_oids_session(st, root_ino, src_oids);
      }
      for (const auto& oid : src_oids) {
        if (oid.size() < live_prefix.size()) continue;
        const std::string dst = snap_prefix + oid.substr(live_prefix.size());
        auto snap = st.session.get_object(oid);
        if (!snap.exists) continue;
        st.session.put_bytes(dst, snap.body, snap.attrs, std::nullopt);
        ++copied;
      }
      using clock = std::chrono::system_clock;
      const auto created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  clock::now().time_since_epoch())
                                  .count();
      nlohmann::json man{{"aios_backup_manifest", 1},
                         {"kind", "posix"},
                         {"volume", st.volume},
                         {"snap_id", sid},
                         {"path", norm},
                         {"root_ino", root_ino},
                         {"created_ms", created_ms},
                         {"oids", copied}};
      st.session.put_bytes(snap_prefix + "manifest", man.dump(), {}, std::nullopt);
    } catch (...) {
      aios::posix::SuperMeta m = st.super;
      m.frozen = false;
      try {
        m.cas = st.session.put_bytes(aios::posix::super_oid(st.volume),
                                     aios::posix::super_to_json(m), {}, m.cas);
        st.super = m;
      } catch (...) {
      }
      throw;
    }
    {
      aios::posix::SuperMeta m = st.super;
      m.frozen = false;
      m.cas = st.session.put_bytes(aios::posix::super_oid(st.volume), aios::posix::super_to_json(m),
                                   {}, m.cas);
      m.exists = true;
      st.super = m;
    }
    std::snprintf(snap_id_out, snap_id_len, "%s", sid.c_str());
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_flock(aios_posix_fs* fs, uint64_t ino, int op) {
  if (!fs) return -EINVAL;
  const int cmd = op & (LOCK_SH | LOCK_EX | LOCK_UN);
  const bool nonblock = (op & LOCK_NB) != 0;
  if (cmd != LOCK_SH && cmd != LOCK_EX && cmd != LOCK_UN) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (cmd != LOCK_UN) {
      const int want = (cmd == LOCK_EX) ? kWantW : kWantR;
      if (int ac = aios::posix::check_access(effective_caller(fs), m, want)) return ac;
      // The cluster lock is on the inode oid and blocks PUTs without the token.
      // Publish first so the lease flusher (and a peer GET) are not fenced out.
      aios::posix::publish_inodes(*fs->st, {ino});
    }
    const std::string oid = aios::posix::ino_oid(fs->st->volume, ino);

    if (cmd == LOCK_UN) {
      std::string token;
      {
        std::lock_guard lock(fs->st->mu);
        auto it = fs->st->flock_tokens.find(ino);
        if (it == fs->st->flock_tokens.end()) return 0;
        token = it->second;
        fs->st->flock_tokens.erase(it);
      }
      try {
        fs->st->session.lock_release(oid, token);
      } catch (const aios::client_error& e) {
        if (e.code() != "not_found") return aios::posix::map_error(e);
      }
      return 0;
    }

    // LOCK_SH and LOCK_EX both use exclusive cluster locks.
    std::string held;
    {
      std::lock_guard lock(fs->st->mu);
      auto it = fs->st->flock_tokens.find(ino);
      if (it != fs->st->flock_tokens.end()) held = it->second;
    }
    if (!held.empty()) {
      try {
        fs->st->session.lock_renew(oid, held, aios::posix::kFlockTtlMs);
        return 0;
      } catch (const aios::client_error&) {
        std::lock_guard lock(fs->st->mu);
        auto it = fs->st->flock_tokens.find(ino);
        if (it != fs->st->flock_tokens.end() && it->second == held) {
          fs->st->flock_tokens.erase(it);
        }
      }
    }

    std::string token;
    if (nonblock) {
      if (!fs->st->session.lock_try_acquire(oid, token, aios::posix::kFlockTtlMs)) {
        return -EWOULDBLOCK;
      }
    } else {
      // Blocking: poll try_acquire.
      for (int i = 0; i < 300; ++i) {
        if (fs->st->session.lock_try_acquire(oid, token, aios::posix::kFlockTtlMs)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        token.clear();
      }
      if (token.empty()) return -EAGAIN;
    }
    std::lock_guard lock(fs->st->mu);
    fs->st->flock_tokens[ino] = std::move(token);
    return 0;
  } catch (const aios::client_error& e) {
    if (e.code() == "lock_held") return -EWOULDBLOCK;
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

}  // extern "C"
