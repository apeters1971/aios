#pragma once

#include <condition_variable>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

struct WatchEvent {
  std::string oid;
  std::uint64_t seq{0};
  std::string op;  // "put" | "del"
  std::int64_t ts_ms{0};
};

// Primary-local enforced leases (not durable across restart).
class LockTable {
 public:
  static constexpr int kDefaultTtlMs = 30000;
  static constexpr int kMaxTtlMs = 300000;

  // Returns nullopt if mutate allowed; otherwise error code ("lock_held").
  std::optional<std::string> check_mutate(const std::string& oid,
                                          const std::optional<std::string>& token) const;

  // Acquire: ok + token/expires, or error "lock_held" / "bad_request".
  bool acquire(const std::string& oid, int ttl_ms, std::string& token_out,
               std::int64_t& expires_ms_out, std::string& err);
  bool renew(const std::string& oid, const std::string& token, int ttl_ms,
             std::int64_t& expires_ms_out, std::string& err);
  bool release(const std::string& oid, const std::string& token, std::string& err);
  // true if held (unexpired). Does not expose token.
  bool stat(const std::string& oid, std::int64_t& expires_ms_out) const;

 private:
  struct Entry {
    std::string token;
    std::int64_t expires_ms{0};
  };

  bool active_locked(const Entry& e, std::int64_t now) const;
  void purge_expired_locked(std::int64_t now);

  mutable std::mutex mu_;
  std::unordered_map<std::string, Entry> locks_;
};

// In-memory watch fanout for long-poll waiters.
class WatchHub {
 public:
  void notify(WatchEvent ev);

  // Block until oid tip event with seq > after_seq, or timeout. Returns true if event.
  bool wait_oid(const std::string& oid, std::uint64_t after_seq, int timeout_ms,
                WatchEvent& out);

  // Block until an event for an oid under prefix, or timeout.
  bool wait_prefix(const std::string& prefix, int timeout_ms, std::vector<WatchEvent>& out);

 private:
  struct Waiter {
    enum class Kind { Oid, Prefix } kind{Kind::Oid};
    std::string oid;
    std::string prefix;
    std::uint64_t after_seq{0};
    bool done{false};
    std::vector<WatchEvent> events;
  };

  std::mutex mu_;
  std::condition_variable cv_;
  std::list<std::shared_ptr<Waiter>> waiters_;
};

}  // namespace aios
