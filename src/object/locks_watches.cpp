#include "object/locks_watches.hpp"

#include "util/log.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <vector>

namespace aios {
namespace {

std::string random_token_hex(std::size_t bytes = 16) {
  std::vector<unsigned char> buf(bytes);
  if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
    // Fallback: time-based (tests / constrained environments).
    std::ostringstream os;
    os << std::hex << now_ms();
    return os.str();
  }
  static const char* hexd = "0123456789abcdef";
  std::string out(bytes * 2, '\0');
  for (std::size_t i = 0; i < bytes; ++i) {
    out[i * 2] = hexd[buf[i] >> 4];
    out[i * 2 + 1] = hexd[buf[i] & 0xf];
  }
  return out;
}

int clamp_ttl(int ttl_ms) {
  if (ttl_ms <= 0) ttl_ms = LockTable::kDefaultTtlMs;
  return std::min(ttl_ms, LockTable::kMaxTtlMs);
}

}  // namespace

LockTable::LockTable() : instance_(random_token_hex(kInstanceHex / 2)) {
  // The time-based fallback of random_token_hex may be shorter than asked for.
  instance_.resize(kInstanceHex, '0');
}

bool LockTable::active_locked(const Entry& e, std::int64_t now) const {
  return e.expires_ms > now;
}

// Turn an entry into a fence: the token stays refused until forget_ms.
void LockTable::fence_locked(Entry& e, std::int64_t now) {
  if (active_locked(e, now)) e.expires_ms = now;
  e.break_requested = false;
  if (e.forget_ms == 0) e.forget_ms = now + kFenceRetainMs;
}

void LockTable::purge_expired_locked(std::int64_t now) {
  for (auto it = locks_.begin(); it != locks_.end();) {
    Entry& e = it->second;
    if (active_locked(e, now)) {
      ++it;
      continue;
    }
    if (e.forget_ms == 0) fence_locked(e, now);
    if (e.forget_ms <= now) it = locks_.erase(it);
    else
      ++it;
  }
  // Bound the fence memory under a flood of short leases on distinct oids:
  // drop the oldest fences first.
  if (locks_.size() > kFenceMaxEntries) {
    std::vector<std::pair<std::int64_t, std::string>> fences;
    for (const auto& [oid, e] : locks_) {
      if (!active_locked(e, now)) fences.emplace_back(e.forget_ms, oid);
    }
    std::sort(fences.begin(), fences.end());
    std::size_t excess = locks_.size() - kFenceMaxEntries;
    for (const auto& [_, oid] : fences) {
      if (excess == 0) break;
      locks_.erase(oid);
      --excess;
    }
  }
}

std::optional<std::string> LockTable::check_mutate(
    const std::string& oid, const std::optional<std::string>& token) const {
  std::lock_guard lock(mu_);
  const auto now = now_ms();
  auto it = locks_.find(oid);
  if (it == locks_.end()) {
    // No lease on this object. A token issued by another primary (the object
    // moved with the cluster map) or by an earlier incarnation of this one (we
    // restarted) means the caller's view is not authoritative any more: refuse
    // like an expired lease, the client re-syncs and replays under a fresh one
    // (NFSv4 BAD_STATEID after a server reboot recovers the same way). A token
    // of our own for some *other* object is fine: clients carry their directory
    // lease token on the sibling log/inode writes.
    if (token && !token->empty() && !issued_here(*token)) return std::string("lock_expired");
    return std::nullopt;
  }
  const Entry& e = it->second;
  if (!active_locked(e, now)) {
    // Expired or released: anyone may write without a token, except the fenced
    // former holder and anyone with a token from elsewhere.
    if (token && !token->empty() &&
        ((*token == e.token && (e.forget_ms == 0 || e.forget_ms > now)) ||
         !issued_here(*token))) {
      return std::string("lock_expired");
    }
    return std::nullopt;
  }
  if (token && *token == e.token) return std::nullopt;
  return std::string("lock_held");
}

bool LockTable::issued_here(const std::string& token) const {
  return token.size() == kTokenHex && token.compare(0, kInstanceHex, instance_) == 0;
}

std::size_t LockTable::fence_if(const std::function<bool(const std::string&)>& moved) {
  std::lock_guard lock(mu_);
  const auto now = now_ms();
  std::size_t n = 0;
  for (auto& [oid, e] : locks_) {
    if (!active_locked(e, now)) continue;
    if (!moved(oid)) continue;
    fence_locked(e, now);
    ++n;
  }
  return n;
}

bool LockTable::acquire(const std::string& oid, int ttl_ms, std::string& token_out,
                        std::int64_t& expires_ms_out, std::string& err) {
  std::lock_guard lock(mu_);
  const auto now = now_ms();
  purge_expired_locked(now);
  auto it = locks_.find(oid);
  if (it != locks_.end() && active_locked(it->second, now)) {
    err = "lock held";
    return false;
  }
  // Overwriting a fence is fine: the old token then mismatches the new one
  // and is refused as lock_held instead of lock_expired.
  Entry e;
  e.token = instance_ + random_token_hex((kTokenHex - kInstanceHex) / 2);
  e.expires_ms = now + clamp_ttl(ttl_ms);
  locks_[oid] = e;
  token_out = e.token;
  expires_ms_out = e.expires_ms;
  return true;
}

bool LockTable::renew(const std::string& oid, const std::string& token, int ttl_ms,
                      Status& out, std::string& err) {
  std::lock_guard lock(mu_);
  const auto now = now_ms();
  auto it = locks_.find(oid);
  if (it == locks_.end() || !active_locked(it->second, now)) {
    if (it != locks_.end() && it->second.token == token) fence_locked(it->second, now);
    // Unknown token (lease moved with the cluster map, or this primary was
    // restarted) reads the same to the holder as an expired one: it is gone.
    err = "lock expired";
    return false;
  }
  Entry& e = it->second;
  if (e.token != token) {
    err = "lock token mismatch";
    return false;
  }
  // A requested break is a hard deadline: renewals may not push it out.
  if (!e.break_requested) e.expires_ms = now + clamp_ttl(ttl_ms);
  out.expires_ms = e.expires_ms;
  out.break_requested = e.break_requested;
  return true;
}

bool LockTable::release(const std::string& oid, const std::string& token, std::string& err) {
  std::lock_guard lock(mu_);
  const auto now = now_ms();
  auto it = locks_.find(oid);
  if (it == locks_.end() || !active_locked(it->second, now)) {
    err = "lock not held";
    return false;
  }
  if (it->second.token != token) {
    err = "lock token mismatch";
    return false;
  }
  fence_locked(it->second, now);
  return true;
}

bool LockTable::stat(const std::string& oid, Status& out) const {
  std::lock_guard lock(mu_);
  const auto now = now_ms();
  auto it = locks_.find(oid);
  if (it == locks_.end() || !active_locked(it->second, now)) return false;
  out.expires_ms = it->second.expires_ms;
  out.break_requested = it->second.break_requested;
  return true;
}

bool LockTable::request_break(const std::string& oid, int grace_ms, Status& out,
                              std::string& err) {
  std::lock_guard lock(mu_);
  const auto now = now_ms();
  auto it = locks_.find(oid);
  if (it == locks_.end() || !active_locked(it->second, now)) {
    err = "lock not held";
    return false;
  }
  Entry& e = it->second;
  if (grace_ms <= 0) grace_ms = kDefaultBreakGraceMs;
  grace_ms = std::min(grace_ms, kMaxTtlMs);
  e.break_requested = true;
  e.expires_ms = std::min(e.expires_ms, now + grace_ms);
  out.expires_ms = e.expires_ms;
  out.break_requested = true;
  return true;
}

void WatchHub::notify(WatchEvent ev) {
  std::lock_guard lock(mu_);
  for (auto it = waiters_.begin(); it != waiters_.end();) {
    auto& w = *it;
    bool match = false;
    if (w->kind == Waiter::Kind::Oid) {
      if (w->oid == ev.oid && ev.seq > w->after_seq) {
        w->events.push_back(ev);
        match = true;
      }
    } else if (ev.oid.rfind(w->prefix, 0) == 0) {
      w->events.push_back(ev);
      match = true;
    }
    if (match) {
      w->done = true;
      it = waiters_.erase(it);
    } else {
      ++it;
    }
  }
  cv_.notify_all();
}

bool WatchHub::wait_oid(const std::string& oid, std::uint64_t after_seq, int timeout_ms,
                        WatchEvent& out) {
  auto waiter = std::make_shared<Waiter>();
  waiter->kind = Waiter::Kind::Oid;
  waiter->oid = oid;
  waiter->after_seq = after_seq;

  std::unique_lock lock(mu_);
  if (stopped_) return false;
  waiters_.push_back(waiter);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms));
  while (!waiter->done) {
    if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) break;
  }
  if (!waiter->done) {
    waiters_.remove(waiter);
    return false;
  }
  if (waiter->events.empty()) return false;
  out = waiter->events.back();
  return true;
}

void WatchHub::shutdown() {
  std::lock_guard lock(mu_);
  stopped_ = true;
  for (auto& w : waiters_) w->done = true;
  waiters_.clear();
  cv_.notify_all();
}

bool WatchHub::wait_prefix(const std::string& prefix, int timeout_ms,
                           std::vector<WatchEvent>& out) {
  auto waiter = std::make_shared<Waiter>();
  waiter->kind = Waiter::Kind::Prefix;
  waiter->prefix = prefix;

  std::unique_lock lock(mu_);
  if (stopped_) return false;
  waiters_.push_back(waiter);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms));
  while (!waiter->done) {
    if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) break;
  }
  if (!waiter->done) {
    waiters_.remove(waiter);
    return false;
  }
  out = std::move(waiter->events);
  return !out.empty();
}

}  // namespace aios
