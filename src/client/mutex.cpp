#include "client/mutex.hpp"

#include "client/error.hpp"

#include <chrono>
#include <thread>

namespace aios {

mutex::mutex(Session& session, std::string name, int ttl_ms)
    : session_(&session),
      name_(std::move(name)),
      oid_(Session::stl_oid("mutex", name_)),
      ttl_ms_(ttl_ms) {}

mutex::~mutex() {
  if (!token_.empty()) {
    try {
      session_->lock_release(oid_, token_);
    } catch (...) {
    }
    token_.clear();
  }
}

void mutex::lock() {
  if (!token_.empty()) throw client_error("bad_request", "mutex already locked by this handle");
  for (;;) {
    if (try_lock()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool mutex::try_lock() {
  if (!token_.empty()) return false;
  std::string tok;
  if (!session_->lock_try_acquire(oid_, tok, ttl_ms_)) return false;
  token_ = std::move(tok);
  return true;
}

void mutex::unlock() {
  if (token_.empty()) throw client_error("bad_request", "mutex not owned");
  session_->lock_release(oid_, token_);
  token_.clear();
}

void mutex::renew() {
  if (token_.empty()) throw client_error("bad_request", "mutex not owned");
  session_->lock_renew(oid_, token_, ttl_ms_);
}

}  // namespace aios
