#pragma once

#include "client/session.hpp"

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

  void lock();
  bool try_lock();
  void unlock();
  void renew();
  bool owns_lock() const { return !token_.empty(); }

  const std::string& name() const { return name_; }
  const std::string& oid() const { return oid_; }

 private:
  Session* session_;
  std::string name_;
  std::string oid_;
  int ttl_ms_;
  std::string token_;
};

}  // namespace aios
