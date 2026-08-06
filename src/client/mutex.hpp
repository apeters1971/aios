#pragma once

#include "client/session.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace aios {

// Cluster-shared mutex via HTTP object locks on oid stl/mutex/{name}.
// Models BasicLockable for std::lock_guard / std::unique_lock.
class mutex {
 public:
  mutex(Session& session, std::string name, int ttl_ms = 30000);
  ~mutex();

  mutex(const mutex&) = delete;
  mutex& operator=(const mutex&) = delete;

  // Blocks until acquired or timeout_ms elapses (defaults to ttl_ms_).
  void lock(std::optional<int> timeout_ms = std::nullopt);
  bool try_lock();
  void unlock() noexcept;
  void renew();
  bool owns_lock() const;

  const std::string& name() const { return name_; }
  const std::string& oid() const { return oid_; }

 private:
  void maybe_renew();

  Session* session_;
  std::string name_;
  std::string oid_;
  int ttl_ms_;
  std::string token_;
  std::int64_t expires_ms_{0};
};

}  // namespace aios
