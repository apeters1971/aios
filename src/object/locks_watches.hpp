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
//
// A lease is exclusive for mutations: while it is active, a PUT/append/DELETE
// without the matching token fails with "lock_held". Two properties make it
// usable as a *delegation* (a client batching directory updates locally and
// flushing them later under the lease):
//
//  * Fencing. A token stays known after its lease expired or was released
//    (for kFenceRetainMs) and is refused with "lock_expired" even when nobody
//    else holds the object. A holder that lost its lease therefore cannot
//    land a late write on top of whatever a successor did; it has to re-sync.
//  * Break. A waiter may request the lease back. The remaining lifetime is cut
//    to at most the grace period, renewals cannot extend past it, and the
//    holder sees break_requested on its next renew / stat so it can flush and
//    release early instead of running to the deadline.
class LockTable {
 public:
  static constexpr int kDefaultTtlMs = 30000;
  static constexpr int kMaxTtlMs = 300000;
  static constexpr int kDefaultBreakGraceMs = 5000;
  static constexpr std::int64_t kFenceRetainMs = 10 * 60 * 1000;
  static constexpr std::size_t kFenceMaxEntries = 65536;

  struct Status {
    std::int64_t expires_ms{0};
    bool break_requested{false};
  };

  // Returns nullopt if mutate allowed; otherwise "lock_held" / "lock_expired".
  std::optional<std::string> check_mutate(const std::string& oid,
                                          const std::optional<std::string>& token) const;

  // Acquire: ok + token/expires, or error "lock_held" / "bad_request".
  bool acquire(const std::string& oid, int ttl_ms, std::string& token_out,
               std::int64_t& expires_ms_out, std::string& err);
  bool renew(const std::string& oid, const std::string& token, int ttl_ms, Status& out,
             std::string& err);
  bool release(const std::string& oid, const std::string& token, std::string& err);
  // true if held (unexpired). Does not expose token.
  bool stat(const std::string& oid, Status& out) const;
  // Ask the holder to give the lease back within grace_ms. false with err
  // "not held" when nothing is held (the caller can acquire right away).
  bool request_break(const std::string& oid, int grace_ms, Status& out, std::string& err);

 private:
  struct Entry {
    std::string token;
    std::int64_t expires_ms{0};
    bool break_requested{false};
    // After expiry / release the entry lingers as a fence for the token.
    std::int64_t forget_ms{0};
  };

  bool active_locked(const Entry& e, std::int64_t now) const;
  void purge_expired_locked(std::int64_t now);
  void fence_locked(Entry& e, std::int64_t now);

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

  // Wake every waiter and refuse new waits. Long polls block for up to two minutes,
  // which is far too long to hold up shutdown.
  void shutdown();

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
  bool stopped_{false};
};

}  // namespace aios
