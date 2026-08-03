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

bool LockTable::active_locked(const Entry& e, std::int64_t now) const {
  return e.expires_ms > now;
}

void LockTable::purge_expired_locked(std::int64_t now) {
  for (auto it = locks_.begin(); it != locks_.end();) {
    if (!active_locked(it->second, now)) it = locks_.erase(it);
    else
      ++it;
  }
}

std::optional<std::string> LockTable::check_mutate(
    const std::string& oid, const std::optional<std::string>& token) const {
  std::lock_guard lock(mu_);
  const auto now = now_ms();
  auto it = locks_.find(oid);
  if (it == locks_.end() || !active_locked(it->second, now)) return std::nullopt;
  if (token && *token == it->second.token) return std::nullopt;
  return std::string("lock_held");
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
  Entry e;
  e.token = random_token_hex();
  e.expires_ms = now + clamp_ttl(ttl_ms);
  locks_[oid] = e;
  token_out = e.token;
  expires_ms_out = e.expires_ms;
  return true;
}

bool LockTable::renew(const std::string& oid, const std::string& token, int ttl_ms,
                      std::int64_t& expires_ms_out, std::string& err) {
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
  it->second.expires_ms = now + clamp_ttl(ttl_ms);
  expires_ms_out = it->second.expires_ms;
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
  locks_.erase(it);
  return true;
}

bool LockTable::stat(const std::string& oid, std::int64_t& expires_ms_out) const {
  std::lock_guard lock(mu_);
  const auto now = now_ms();
  auto it = locks_.find(oid);
  if (it == locks_.end() || !active_locked(it->second, now)) return false;
  expires_ms_out = it->second.expires_ms;
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

bool WatchHub::wait_prefix(const std::string& prefix, int timeout_ms,
                           std::vector<WatchEvent>& out) {
  auto waiter = std::make_shared<Waiter>();
  waiter->kind = Waiter::Kind::Prefix;
  waiter->prefix = prefix;

  std::unique_lock lock(mu_);
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
