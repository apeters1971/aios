#include "client/mutex.hpp"

#include "client/error.hpp"
#include "util/log.hpp"

#include <chrono>
#include <thread>

namespace aios {

mutex::mutex(Session& session, std::string name, int ttl_ms)
    : session_(&session),
      name_(std::move(name)),
      oid_(Session::stl_oid("mutex", name_)),
      ttl_ms_(ttl_ms) {}

mutex::~mutex() {
  unlock();
}

void mutex::maybe_renew() {
  if (token_.empty() || expires_ms_ <= 0) return;
  const auto now = now_ms();
  const auto renew_at = expires_ms_ - static_cast<std::int64_t>(ttl_ms_ / 3);
  if (now < renew_at) return;
  try {
    session_->lock_renew(oid_, token_, ttl_ms_, &expires_ms_);
  } catch (...) {
  }
}

bool mutex::owns_lock() const {
  if (token_.empty()) return false;
  if (expires_ms_ > 0 && now_ms() >= expires_ms_) return false;
  return true;
}

void mutex::lock(std::optional<int> timeout_ms) {
  if (owns_lock()) throw client_error("bad_request", "mutex already locked by this handle");
  const int limit_ms = timeout_ms.value_or(ttl_ms_);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limit_ms);
  for (;;) {
    if (try_lock()) return;
    if (std::chrono::steady_clock::now() >= deadline) {
      throw client_error("timeout", "mutex lock timed out");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool mutex::try_lock() {
  if (owns_lock()) return false;
  std::string tok;
  std::int64_t exp = 0;
  if (!session_->lock_try_acquire(oid_, tok, ttl_ms_, &exp)) return false;
  token_ = std::move(tok);
  expires_ms_ = exp > 0 ? exp : now_ms() + ttl_ms_;
  return true;
}

void mutex::unlock() noexcept {
  if (token_.empty()) return;
  const auto tok = std::move(token_);
  token_.clear();
  expires_ms_ = 0;
  try {
    session_->lock_release(oid_, tok);
  } catch (...) {
  }
}

void mutex::renew() {
  if (!owns_lock()) throw client_error("bad_request", "mutex not owned");
  session_->lock_renew(oid_, token_, ttl_ms_, &expires_ms_);
}

}  // namespace aios
