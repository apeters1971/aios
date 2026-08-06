#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

enum class DeliveryMode { Ephemeral, Buffered, Durable };

inline const char* delivery_mode_name(DeliveryMode m) {
  switch (m) {
    case DeliveryMode::Ephemeral:
      return "ephemeral";
    case DeliveryMode::Buffered:
      return "buffered";
    case DeliveryMode::Durable:
      return "durable";
  }
  return "buffered";
}

inline std::optional<DeliveryMode> parse_delivery_mode(const std::string& s) {
  if (s == "ephemeral") return DeliveryMode::Ephemeral;
  if (s == "buffered") return DeliveryMode::Buffered;
  if (s == "durable") return DeliveryMode::Durable;
  return std::nullopt;
}

struct PubMessage {
  std::uint64_t id{0};
  std::int64_t ts_ms{0};
  std::string content_type;
  std::vector<std::uint8_t> data;
};

struct TopicStat {
  bool exists{false};
  DeliveryMode delivery{DeliveryMode::Buffered};
  std::uint64_t next_id{1};  // next id to assign; tip = next_id - 1
  std::size_t buffered{0};
  std::size_t capacity{0};
};

// Primary-local topic hub. Ephemeral/buffered state is in-memory only.
// Durable persistence is owned by ObjectService; this hub tracks cursor + waiters.
class TopicHub {
 public:
  static constexpr std::size_t kDefaultCapacity = 256;
  static constexpr std::size_t kMaxCapacity = 4096;
  static constexpr std::size_t kMaxMessageBytes = 1u << 20;  // 1 MiB

  // Create or ensure topic. Fails with mode_mismatch if mode conflicts.
  bool create(const std::string& topic, DeliveryMode mode, std::size_t capacity,
              std::string& err_code, std::string& err);

  // Load durable cursor into memory (idempotent if already present with same mode).
  bool ensure_durable(const std::string& topic, std::uint64_t next_id, std::string& err_code,
                      std::string& err);

  bool stat(const std::string& topic, TopicStat& out) const;

  // Publish for ephemeral/buffered (allocates id, buffers if needed, wakes waiters).
  bool publish_memory(const std::string& topic, std::optional<DeliveryMode> mode_if_new,
                      std::size_t capacity_if_new, const std::uint8_t* data, std::size_t len,
                      const std::string& content_type, PubMessage& out, DeliveryMode& mode_out,
                      std::string& err_code, std::string& err);

  // Reserve next id for durable publish (does not wake waiters).
  bool reserve_durable(const std::string& topic, const std::uint8_t* data, std::size_t len,
                       const std::string& content_type, PubMessage& out, std::string& err_code,
                       std::string& err);

  // After durable objects are persisted, advance tip and wake waiters.
  void commit_durable(const std::string& topic, const PubMessage& msg);

  // Abort a reserved durable id (topic next_id unchanged only if msg.id == next_id-1
  // after reserve — we bump next_id on reserve, so abort leaves a hole; acceptable for v1).
  // Actually: reserve bumps next_id; on failure we leave the hole. No abort API needed.

  // Return buffered messages with id > after_id (buffered mode only).
  std::vector<PubMessage> buffered_since(const std::string& topic, std::uint64_t after_id) const;

  // Block until messages with id > after_id arrive, or timeout.
  // For buffered: returns immediately if ring has matching messages.
  // For ephemeral/durable: only waits for future publishes (caller handles durable catch-up).
  bool subscribe(const std::string& topic, std::uint64_t after_id, int timeout_ms,
                 std::vector<PubMessage>& out);

  // Wake every subscriber and refuse new waits, so shutdown does not block on a
  // long poll.
  void shutdown();

  std::uint64_t tip_id(const std::string& topic) const;  // 0 if none / unknown

 private:
  struct Topic {
    DeliveryMode delivery{DeliveryMode::Buffered};
    std::uint64_t next_id{1};
    std::size_t capacity{kDefaultCapacity};
    std::deque<PubMessage> ring;
  };

  struct Waiter {
    std::string topic;
    std::uint64_t after_id{0};
    bool done{false};
    std::vector<PubMessage> messages;
  };

  Topic* get_or_create_locked(const std::string& topic, DeliveryMode mode,
                              std::size_t capacity, std::string& err_code, std::string& err);
  void notify_locked(const std::string& topic, const PubMessage& msg);
  static std::size_t clamp_capacity(std::size_t capacity);

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::unordered_map<std::string, Topic> topics_;
  std::list<std::shared_ptr<Waiter>> waiters_;
  bool stopped_{false};
};

inline std::string pubsub_meta_oid(const std::string& topic) { return "pubsub/" + topic; }

inline std::string pubsub_msg_oid(const std::string& topic, std::uint64_t id) {
  return "pubsub/" + topic + "/m/" + std::to_string(id);
}

}  // namespace aios
