// Directory leases for libaios_posix: see the comment on struct DirLease in
// posix_internal.hpp. The protocol (server lock on the directory's meta object,
// batched changelog appends under the lock, break/renew) is the same one the
// kernel client uses, so both kinds of mount interoperate on one volume.

#include "posix/posix_internal.hpp"

#include "client/changelog.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>

namespace aios {
namespace posix {

namespace {

using clock_t_ = std::chrono::steady_clock;

std::string dir_meta_json(uint64_t next_op, uint64_t log_bytes, uint64_t snapshot_op,
                          const std::string& snap_oid) {
  return nlohmann::json{{"aios_posix_dir", 1},
                        {"next_op", next_op},
                        {"log_bytes", log_bytes},
                        {"snapshot_op", snapshot_op},
                        {"snapshot_oid", snap_oid}}
      .dump();
}

void apply_op(std::unordered_map<std::string, uint64_t>& entries, const DirLeaseOp& op) {
  if (op.op == kOpLink && op.args.size() >= 2) {
    entries[op.args[0]] = static_cast<uint64_t>(std::stoull(op.args[1]));
  } else if (op.op == kOpUnlink && !op.args.empty()) {
    entries.erase(op.args[0]);
  } else if (op.op == kOpRename && op.args.size() >= 2) {
    auto it = entries.find(op.args[0]);
    if (it != entries.end()) {
      const uint64_t child = it->second;
      entries.erase(it);
      entries[op.args[1]] = child;
    }
  }
}

// lock_held / lock_expired on our own meta PUT, or a CAS conflict there, mean
// somebody else owns the directory now.
bool is_lost_error(const client_error& e) {
  return e.code() == "lock_held" || e.code() == "lock_expired" || e.code() == "conflict" ||
         e.code() == "not_found";
}

}  // namespace

DirLeaseManager::DirLeaseManager(FsState& st) : st_(st) {}

DirLeaseManager::~DirLeaseManager() { stop(); }

void DirLeaseManager::start() {
  if (th_.joinable()) return;
  stop_.store(false);
  th_ = std::thread([this] { run(); });
}

void DirLeaseManager::stop() {
  if (!th_.joinable()) return;
  try {
    sync_all(true);
  } catch (...) {
  }
  stop_.store(true);
  kick();
  th_.join();
}

void DirLeaseManager::kick() {
  {
    std::lock_guard lock(wake_mu_);
    wake_ = true;
  }
  wake_cv_.notify_all();
}

std::shared_ptr<DirLease> DirLeaseManager::slot(uint64_t ino) {
  std::lock_guard lock(mu_);
  auto it = leases_.find(ino);
  if (it != leases_.end()) return it->second;
  if (leases_.size() >= kLeaseMax) {
    // Recycle the least recently used idle slot; one with an unreported error
    // is kept so fsync can still return it.
    std::shared_ptr<DirLease> victim;
    for (auto& [i, l] : leases_) {
      std::lock_guard ll(l->mu);
      if (l->busy_locked() || l->err) continue;
      if (!victim || l->last_use < victim->last_use) victim = l;
    }
    if (!victim) return nullptr;
    leases_.erase(victim->ino);
  }
  auto l = std::make_shared<DirLease>(ino, dir_meta_oid(st_.volume, ino),
                                      dir_log_oid(st_.volume, ino), dir_snap_oid(st_.volume, ino));
  leases_[ino] = l;
  return l;
}

std::shared_ptr<DirLease> DirLeaseManager::authoritative(uint64_t ino) {
  std::shared_ptr<DirLease> l;
  {
    std::lock_guard lock(mu_);
    auto it = leases_.find(ino);
    if (it == leases_.end()) return nullptr;
    l = it->second;
  }
  std::lock_guard ll(l->mu);
  if (l->owns_locked(clock_t_::now()) || !l->pending.empty()) return l;
  return nullptr;
}

std::shared_ptr<DirLease> DirLeaseManager::get(uint64_t ino, const PutLayout& layout) {
  if (st_.no_lease || stop_.load()) return nullptr;
  auto l = slot(ino);
  if (!l) return nullptr;

  std::lock_guard acq(l->acquire_mu);
  {
    std::unique_lock lock(l->mu);
    const auto now = clock_t_::now();
    if (l->active_locked(now)) {
      l->last_use = now;
      return l;
    }
    if (l->owns_locked(now)) {
      // Break requested / release wanted / about to expire: hand it back so
      // the synchronous path can take the lock itself.
      lock.unlock();
      drop(ino);
      return nullptr;
    }
    // Lost with records still queued: they must reach the server before
    // anything else is committed to this directory.
    if (!l->flushed_locked()) {
      kick();
      l->cv.wait(lock, [&] { return l->flushed_locked(); });
    }
    if (l->next_try != clock_t_::time_point{} && now < l->next_try) return nullptr;
  }
  // Unmount may have drained everything while we waited; no flusher is left to
  // commit or release a lease taken now.
  if (stop_.load()) return nullptr;

  std::string token;
  try {
    token = st_.session.lock_acquire(l->meta_oid, kLeaseTtlMs).token;
  } catch (const client_error& e) {
    if (e.code() == "lock_held") {
      try {
        st_.session.lock_break(l->meta_oid, kLeaseBreakGraceMs);
      } catch (...) {
      }
    }
    std::lock_guard lock(l->mu);
    l->next_try = clock_t_::now() + kLeaseRetry;
    return nullptr;
  }
  // We own the directory now: load the tip once, it stays authoritative.
  DirTable tip(st_.session, st_.volume, ino, nullptr);
  try {
    tip.load(false);
  } catch (...) {
    try {
      st_.session.lock_release(l->meta_oid, token);
    } catch (...) {
    }
    std::lock_guard lock(l->mu);
    l->next_try = clock_t_::now() + kLeaseRetry;
    return nullptr;
  }
  if (stop_.load()) {
    try {
      st_.session.lock_release(l->meta_oid, token);
    } catch (...) {
    }
    return nullptr;
  }
  {
    std::lock_guard lock(l->mu);
    const auto now = clock_t_::now();
    l->token = std::move(token);
    l->put_layout = layout;
    l->held = true;
    l->break_requested = false;
    l->release_wanted = false;
    l->expires = now + std::chrono::milliseconds(kLeaseTtlMs);
    l->last_renew = l->last_use = now;
    l->entries = tip.entries();
    l->next_op = tip.next_op();
    l->log_bytes = tip.log_bytes();
    l->snapshot_op = tip.snapshot_op();
    l->meta_cas = tip.meta_cas();
    l->err = 0;
  }
  {
    // The plain directory cache may hold a pre-lease snapshot.
    std::lock_guard lock(st_.mu);
    st_.dir_cache.erase(ino);
  }
  kick();
  return l;
}

int DirLeaseManager::queue(DirLease& l, uint32_t op, std::vector<std::string> args,
                           bool must_be_absent, uint64_t expected_ino) {
  std::unique_lock lock(l.mu);
  if (!l.owns_locked(clock_t_::now())) return -EAGAIN;
  if (args.empty()) return -EINVAL;
  if (op == kOpLink && must_be_absent && l.entries.count(args[0])) return -EEXIST;
  if (op == kOpUnlink || op == kOpRename) {
    auto it = l.entries.find(args[0]);
    if (it == l.entries.end()) return -ENOENT;
    if (expected_ino && it->second != expected_ino) return -ENOENT;
  }
  // Bound the queue: a flood of creates waits for the flusher.
  while (l.pending.size() >= kLeaseMaxPending) {
    kick();
    l.cv.wait(lock);
    if (!l.owns_locked(clock_t_::now())) return -EAGAIN;
  }
  DirLeaseOp rec{op, std::move(args)};
  apply_op(l.entries, rec);
  l.pending.push_back(std::move(rec));
  l.last_use = clock_t_::now();
  lock.unlock();
  kick();
  return 0;
}

int DirLeaseManager::peek(DirLease& l, const std::string& name, uint64_t& ino_out) {
  std::lock_guard lock(l.mu);
  if (!l.owns_locked(clock_t_::now())) return -EAGAIN;
  auto it = l.entries.find(name);
  ino_out = it == l.entries.end() ? 0 : it->second;
  return 0;
}

int DirLeaseManager::rename(DirLease& l, const std::string& old_name, const std::string& new_name,
                            uint64_t moved, uint64_t victim) {
  std::unique_lock lock(l.mu);
  if (!l.owns_locked(clock_t_::now())) return -EAGAIN;
  auto oit = l.entries.find(old_name);
  if (oit == l.entries.end()) return -ENOENT;
  if (oit->second != moved) return -EAGAIN;
  auto nit = l.entries.find(new_name);
  const uint64_t cur_victim = nit == l.entries.end() ? 0 : nit->second;
  if (cur_victim != victim) return -EAGAIN;
  while (l.pending.size() >= kLeaseMaxPending) {
    kick();
    l.cv.wait(lock);
    if (!l.owns_locked(clock_t_::now())) return -EAGAIN;
  }
  DirLeaseOp rec{kOpRename, {old_name, new_name}};
  apply_op(l.entries, rec);
  l.pending.push_back(std::move(rec));
  l.last_use = clock_t_::now();
  lock.unlock();
  kick();
  return 0;
}

void DirLeaseManager::touch_parent(DirLease& l, uint64_t ts, int nlink_delta) {
  {
    std::lock_guard lock(l.mu);
    l.parent_dirty = true;
    l.parent_mtime_ns = std::max(l.parent_mtime_ns, ts);
    l.nlink_delta += nlink_delta;
    l.last_use = clock_t_::now();
  }
  {
    std::lock_guard lock(st_.mu);
    auto it = st_.inode_cache.find(l.ino);
    if (it != st_.inode_cache.end()) {
      InodeMeta& m = it->second.meta;
      m.mtime_ns = std::max(m.mtime_ns, ts);
      m.ctime_ns = std::max(m.ctime_ns, ts);
      if (nlink_delta > 0) {
        m.nlink += static_cast<uint32_t>(nlink_delta);
      } else if (nlink_delta < 0 && m.nlink > 2) {
        m.nlink -= 1;
      }
    }
  }
  kick();
}

void DirLeaseManager::drop(uint64_t ino) {
  std::shared_ptr<DirLease> l;
  {
    std::lock_guard lock(mu_);
    auto it = leases_.find(ino);
    if (it == leases_.end()) return;
    l = it->second;
  }
  std::unique_lock lock(l->mu);
  if (!l->busy_locked()) return;
  l->release_wanted = true;
  l->next_try = clock_t_::now() + kLeaseRetry;
  kick();
  l->cv.wait(lock, [&] { return !l->busy_locked() || stop_.load(); });
  l->release_wanted = false;
}

int DirLeaseManager::fsync(uint64_t ino) {
  std::shared_ptr<DirLease> l;
  {
    std::lock_guard lock(mu_);
    auto it = leases_.find(ino);
    if (it == leases_.end()) return 0;
    l = it->second;
  }
  std::unique_lock lock(l->mu);
  if (!l->flushed_locked()) {
    kick();
    l->cv.wait(lock, [&] { return l->flushed_locked() || stop_.load(); });
  }
  const int err = l->err;
  l->err = 0;
  return err;
}

int DirLeaseManager::sync_all(bool release) {
  std::vector<std::shared_ptr<DirLease>> all;
  {
    std::lock_guard lock(mu_);
    all.reserve(leases_.size());
    for (auto& [i, l] : leases_) all.push_back(l);
  }
  int err = 0;
  for (auto& l : all) {
    if (release) {
      std::unique_lock lock(l->mu);
      if (l->busy_locked()) {
        l->release_wanted = true;
        kick();
        l->cv.wait(lock, [&] { return !l->busy_locked(); });
        l->release_wanted = false;
      }
      if (l->err && !err) err = l->err;
      l->err = 0;
    } else {
      const int e = fsync(l->ino);
      if (e && !err) err = e;
    }
  }
  return err;
}

// ---------------------------------------------------------------------------
// Flusher thread.
// ---------------------------------------------------------------------------

void DirLeaseManager::run() {
  while (!stop_.load()) {
    std::vector<std::shared_ptr<DirLease>> all;
    {
      std::lock_guard lock(mu_);
      all.reserve(leases_.size());
      for (auto& [i, l] : leases_) all.push_back(l);
    }
    bool again = false;
    for (auto& l : all) {
      try {
        service(*l);
      } catch (...) {
      }
      std::lock_guard lock(l->mu);
      if (!l->flushed_locked()) again = true;
    }
    std::unique_lock lock(wake_mu_);
    // Transient failures retry soon; otherwise wake for the next renew or a kick.
    const auto wait = again ? std::chrono::milliseconds(100) : kLeaseRenew;
    wake_cv_.wait_for(lock, wait, [&] { return wake_ || stop_.load(); });
    wake_ = false;
  }
}

void DirLeaseManager::release_locked_token(DirLease& l, const std::string& token) {
  // Caller holds l.mu and has already cleared held; the network call is made
  // without the lock so waiters are not stalled behind it.
  l.mu.unlock();
  try {
    st_.session.lock_release(l.meta_oid, token);
  } catch (...) {
  }
  l.mu.lock();
}

void DirLeaseManager::service(DirLease& l) {
  const auto now = clock_t_::now();
  bool owns;
  bool lost = false;
  std::string token;
  {
    std::lock_guard lock(l.mu);
    owns = l.owns_locked(now);
    token = l.token;
    if (l.held && !owns) {
      // Expired without a successful renew.
      l.held = false;
      lost = true;
    }
  }

  if (owns) {
    const int rc = flush(l);
    if (rc == -ESTALE) {
      std::unique_lock lock(l.mu);
      // Only touch the lease if nobody re-acquired it meanwhile. Give the lock
      // back in case we still have it (the conflict may have been on the log
      // object), so the replay's own acquire does not have to break our lease.
      if (l.held && l.token == token) {
        l.held = false;
        l.break_requested = false;
        release_locked_token(l, token);
        lost = true;
      }
    }
  }
  {
    std::lock_guard lock(l.mu);
    if (!l.held && !l.flushed_locked()) lost = true;
  }
  if (lost) {
    {
      std::lock_guard lock(st_.mu);
      st_.dir_cache.erase(l.ino);
    }
    replay(l);
  }

  {
    std::unique_lock lock(l.mu);
    const auto now2 = clock_t_::now();
    if (l.owns_locked(now2)) {
      const bool idle = l.flushed_locked() && now2 - l.last_use > kLeaseIdle;
      if (l.release_wanted || l.break_requested || idle) {
        // Anything still queued (a flush just failed) is re-committed
        // synchronously by the next run.
        const std::string tok = l.token;
        l.held = false;
        l.break_requested = false;
        release_locked_token(l, tok);
      } else if (now2 - l.last_renew >= kLeaseRenew) {
        const std::string tok = l.token;
        bool brk = false;
        bool renew_lost = false;
        bool renewed = false;
        lock.unlock();
        try {
          st_.session.lock_renew(l.meta_oid, tok, kLeaseTtlMs, nullptr, &brk);
          renewed = true;
        } catch (const client_error& e) {
          renew_lost = is_lost_error(e);
        } catch (...) {
        }
        lock.lock();
        if (l.held && l.token == tok) {
          if (renew_lost) {
            l.held = false;
          } else if (renewed) {
            l.last_renew = clock_t_::now();
            if (brk) {
              // The server shortened us to the grace period.
              l.break_requested = true;
              l.expires = std::min(l.expires, clock_t_::now() +
                                                  std::chrono::milliseconds(kLeaseBreakGraceMs));
              kick();
            } else {
              l.expires = clock_t_::now() + std::chrono::milliseconds(kLeaseTtlMs);
            }
          }
          // Other errors: keep going on the current expiry.
        }
      }
    }
  }
  l.cv.notify_all();
}

int DirLeaseManager::flush(DirLease& l) {
  int conflicts = 0;
  for (;;) {
    std::string batch;
    size_t n = 0;
    bool pd;
    uint64_t pts;
    int pdelta;
    std::string token;
    uint64_t next_op, log_bytes, snapshot_op, meta_cas;
    PutLayout layout;
    {
      std::lock_guard lock(l.mu);
      for (const auto& op : l.pending) {
        changelog::Record r;
        r.op_id = l.next_op + n;
        r.op = static_cast<changelog::Op>(op.op);
        r.args = op.args;
        const std::string enc = changelog::encode_record(r);
        if (!batch.empty() && batch.size() + enc.size() > kLeaseBatchBytes) break;
        batch += enc;
        ++n;
      }
      pd = l.parent_dirty;
      pts = l.parent_mtime_ns;
      pdelta = l.nlink_delta;
      l.parent_dirty = false;
      l.nlink_delta = 0;
      token = l.token;
      next_op = l.next_op;
      log_bytes = l.log_bytes;
      snapshot_op = l.snapshot_op;
      meta_cas = l.meta_cas;
      layout = l.put_layout;
    }
    if (n == 0 && !pd) return 0;

    try {
      if (n) {
        auto ar = st_.session.append(l.log_oid, batch, token);
        if (ar.offset != log_bytes || ar.size >= changelog::kAutoCompactBytes) {
          // Garbage past the committed log_bytes (a peer's failed append, or a
          // duplicate of ours after a lost reply) or a long log: rewrite the tip.
          const int rc = compact(l, n);
          if (rc) return rc;
        } else {
          const uint64_t cas =
              st_.session.put_bytes(l.meta_oid, dir_meta_json(next_op + n, ar.size, snapshot_op,
                                                              l.snap_oid),
                                    {}, meta_cas, token, layout);
          std::lock_guard lock(l.mu);
          l.meta_cas = cas;
          l.log_bytes = ar.size;
          l.next_op += n;
        }
        std::lock_guard lock(l.mu);
        for (size_t i = 0; i < n && !l.pending.empty(); ++i) l.pending.pop_front();
      }
      if (pd) {
        try {
          touch_parent_now(l, pts, pdelta);
        } catch (const client_error& e) {
          std::lock_guard lock(l.mu);
          if (!l.err) l.err = map_error(e);
        } catch (...) {
          std::lock_guard lock(l.mu);
          if (!l.err) l.err = -EIO;
        }
      }
      l.cv.notify_all();
    } catch (const client_error& e) {
      {
        std::lock_guard lock(l.mu);
        if (pd) {
          l.parent_dirty = true;
          l.nlink_delta += pdelta;
          l.parent_mtime_ns = std::max(l.parent_mtime_ns, pts);
        }
      }
      // lock_held on the *log* object is a peer's cross-directory rename that
      // locked the log first and now waits for our meta lock: it has asked for
      // a break, which the renew path honours. A conflict on our own meta PUT
      // (our reply was lost but applied) shows as garbage next time. Give both
      // a few tries before declaring the lease lost.
      if ((e.code() == "lock_held" || e.code() == "conflict") && ++conflicts < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20 * conflicts));
        continue;
      }
      if (e.code() == "lock_held") return -EBUSY;  // keep the lease; retried after the renew
      if (is_lost_error(e)) return -ESTALE;
      return -EIO;
    } catch (...) {
      std::lock_guard lock(l.mu);
      if (pd) {
        l.parent_dirty = true;
        l.nlink_delta += pdelta;
        l.parent_mtime_ns = std::max(l.parent_mtime_ns, pts);
      }
      return -EIO;
    }
  }
}

int DirLeaseManager::compact(DirLease& l, size_t n) {
  // Table = server tip + the n records just appended; log and snapshot need
  // their own locks (meta is ours), committed together through /txn.
  DirTable tip(st_.session, st_.volume, l.ino, nullptr);
  tip.load(false);
  std::string token;
  PutLayout layout;
  {
    std::lock_guard lock(l.mu);
    size_t i = 0;
    for (const auto& op : l.pending) {
      if (i++ == n) break;
      tip.apply_record(0, op.op, op.args);
    }
    token = l.token;
    layout = l.put_layout;
  }
  tip.set_put_layout(layout);
  HeldLocks locks;
  locks.session = &st_.session;
  locks.acquire_sorted({l.log_oid, l.snap_oid});
  std::string txn_id;
  try {
    txn_id = st_.session.txn_begin();
    {
      // Bodies with the op counter advanced past the n records just appended
      // (the raw load returned the server's pre-append next_op).
      std::string meta, snap, log;
      const uint64_t next = tip.next_op() + n;
      nlohmann::json entries = nlohmann::json::object();
      for (const auto& [name, ino] : tip.entries()) entries[name] = ino;
      snap = nlohmann::json{{"entries", entries}}.dump();
      meta = dir_meta_json(next, 0, next - 1, l.snap_oid);
      st_.session.txn_prepare_put(txn_id, l.snap_oid, snap, std::nullopt,
                                  locks.token_for(l.snap_oid));
      st_.session.txn_prepare_put(txn_id, l.log_oid, log, std::nullopt,
                                  locks.token_for(l.log_oid));
      st_.session.txn_prepare_put(txn_id, l.meta_oid, meta, tip.meta_cas(), token);
      st_.session.txn_commit(txn_id);
      txn_id.clear();
      std::lock_guard lock(l.mu);
      l.next_op = next;
      l.log_bytes = 0;
      l.snapshot_op = next - 1;
      l.meta_cas = tip.meta_cas() + 1;
    }
  } catch (const client_error& e) {
    if (!txn_id.empty()) {
      try {
        st_.session.txn_abort(txn_id);
      } catch (...) {
      }
    }
    if (is_lost_error(e)) return -ESTALE;
    return -EIO;
  } catch (...) {
    return -EIO;
  }
  return 0;
}

void DirLeaseManager::replay(DirLease& l) {
  // The lease is gone with records still queued: commit them one by one with
  // the synchronous protocol (fresh locks per record). What cannot be committed
  // is reported through fsync of the directory.
  DirTable raw(st_.session, st_.volume, l.ino, nullptr);
  for (;;) {
    DirLeaseOp op;
    {
      std::lock_guard lock(l.mu);
      if (l.pending.empty()) break;
      op = l.pending.front();
    }
    try {
      raw.append_ops({{op.op, op.args}});
    } catch (const client_error& e) {
      AIOS_LOG_WARN("posix: dir ", l.ino, ": lost lease, op ", op.op, " on \"",
                    op.args.empty() ? std::string() : op.args[0], "\" not committed: ", e.what());
      std::lock_guard lock(l.mu);
      if (!l.err) l.err = map_error(e);
    } catch (...) {
      std::lock_guard lock(l.mu);
      if (!l.err) l.err = -EIO;
    }
    {
      std::lock_guard lock(l.mu);
      if (!l.pending.empty()) l.pending.pop_front();
    }
    l.cv.notify_all();
  }
  bool pd;
  uint64_t pts;
  int pdelta;
  {
    std::lock_guard lock(l.mu);
    pd = l.parent_dirty;
    pts = l.parent_mtime_ns;
    pdelta = l.nlink_delta;
    l.parent_dirty = false;
    l.nlink_delta = 0;
  }
  if (pd) {
    try {
      touch_parent_now(l, pts, pdelta);
    } catch (const client_error& e) {
      std::lock_guard lock(l.mu);
      if (!l.err) l.err = map_error(e);
    } catch (...) {
      std::lock_guard lock(l.mu);
      if (!l.err) l.err = -EIO;
    }
  }
  {
    std::lock_guard lock(st_.mu);
    st_.dir_cache.erase(l.ino);
  }
  l.cv.notify_all();
}

void DirLeaseManager::touch_parent_now(DirLease& l, uint64_t ts, int delta) {
  // The in-core copy already carries this delta (touch_parent); apply it to the
  // server's record, which the store then republishes in-core.
  auto snap = st_.session.get_object(ino_oid(st_.volume, l.ino));
  if (!snap.exists) return;  // directory removed meanwhile
  InodeMeta m = inode_from_json(snap.body, cas_from_attrs(snap.attrs));
  auto apply = [ts, delta](InodeMeta& next) {
    next.mtime_ns = std::max(next.mtime_ns, ts);
    next.ctime_ns = std::max(next.ctime_ns, ts);
    if (delta > 0) {
      next.nlink += static_cast<uint32_t>(delta);
    } else if (delta < 0) {
      const uint32_t d = static_cast<uint32_t>(-delta);
      next.nlink = next.nlink > 2 + d ? next.nlink - d : 2;
    }
  };
  apply(m);
  store_inode(st_, m, std::nullopt, apply);
}

}  // namespace posix
}  // namespace aios
