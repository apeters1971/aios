#include "object/pubsub.hpp"

#include "util/log.hpp"

#include <algorithm>
#include <chrono>

namespace aios {

std::size_t TopicHub::clamp_capacity(std::size_t capacity) {
  if (capacity == 0) capacity = kDefaultCapacity;
  return std::min(capacity, kMaxCapacity);
}

TopicHub::Topic* TopicHub::get_or_create_locked(const std::string& topic, DeliveryMode mode,
                                                std::size_t capacity, std::string& err_code,
                                                std::string& err) {
  auto it = topics_.find(topic);
  if (it != topics_.end()) {
    if (it->second.delivery != mode) {
      err_code = "mode_mismatch";
      err = "topic delivery mode mismatch";
      return nullptr;
    }
    return &it->second;
  }
  Topic t;
  t.delivery = mode;
  t.capacity = clamp_capacity(capacity);
  t.next_id = 1;
  auto [ins, _] = topics_.emplace(topic, std::move(t));
  return &ins->second;
}

void TopicHub::notify_locked(const std::string& topic, const PubMessage& msg) {
  for (auto it = waiters_.begin(); it != waiters_.end();) {
    auto& w = *it;
    if (w->topic == topic && msg.id > w->after_id) {
      w->messages.push_back(msg);
      w->done = true;
      it = waiters_.erase(it);
    } else {
      ++it;
    }
  }
  cv_.notify_all();
}

bool TopicHub::create(const std::string& topic, DeliveryMode mode, std::size_t capacity,
                      std::string& err_code, std::string& err) {
  if (topic.empty()) {
    err_code = "bad_request";
    err = "empty topic";
    return false;
  }
  std::lock_guard lock(mu_);
  return get_or_create_locked(topic, mode, capacity, err_code, err) != nullptr;
}

bool TopicHub::ensure_durable(const std::string& topic, std::uint64_t next_id,
                              std::string& err_code, std::string& err) {
  if (topic.empty()) {
    err_code = "bad_request";
    err = "empty topic";
    return false;
  }
  std::lock_guard lock(mu_);
  auto it = topics_.find(topic);
  if (it != topics_.end()) {
    if (it->second.delivery != DeliveryMode::Durable) {
      err_code = "mode_mismatch";
      err = "topic delivery mode mismatch";
      return false;
    }
    if (next_id > it->second.next_id) it->second.next_id = next_id;
    return true;
  }
  Topic t;
  t.delivery = DeliveryMode::Durable;
  t.capacity = 0;
  t.next_id = std::max<std::uint64_t>(1, next_id);
  topics_.emplace(topic, std::move(t));
  return true;
}

bool TopicHub::stat(const std::string& topic, TopicStat& out) const {
  std::lock_guard lock(mu_);
  auto it = topics_.find(topic);
  if (it == topics_.end()) {
    out.exists = false;
    return false;
  }
  out.exists = true;
  out.delivery = it->second.delivery;
  out.next_id = it->second.next_id;
  out.buffered = it->second.ring.size();
  out.capacity = it->second.capacity;
  return true;
}

bool TopicHub::publish_memory(const std::string& topic, std::optional<DeliveryMode> mode_if_new,
                              std::size_t capacity_if_new, const std::uint8_t* data,
                              std::size_t len, const std::string& content_type, PubMessage& out,
                              DeliveryMode& mode_out, std::string& err_code, std::string& err) {
  if (topic.empty()) {
    err_code = "bad_request";
    err = "empty topic";
    return false;
  }
  if (len > kMaxMessageBytes) {
    err_code = "payload_too_large";
    err = "message exceeds 1 MiB";
    return false;
  }

  std::lock_guard lock(mu_);
  Topic* t = nullptr;
  auto it = topics_.find(topic);
  if (it == topics_.end()) {
    const DeliveryMode mode = mode_if_new.value_or(DeliveryMode::Buffered);
    if (mode == DeliveryMode::Durable) {
      err_code = "bad_request";
      err = "use durable publish path";
      return false;
    }
    t = get_or_create_locked(topic, mode, capacity_if_new, err_code, err);
    if (!t) return false;
  } else {
    t = &it->second;
    if (mode_if_new && *mode_if_new != t->delivery) {
      err_code = "mode_mismatch";
      err = "topic delivery mode mismatch";
      return false;
    }
    if (t->delivery == DeliveryMode::Durable) {
      err_code = "bad_request";
      err = "use durable publish path";
      return false;
    }
  }

  PubMessage msg;
  msg.id = t->next_id++;
  msg.ts_ms = now_ms();
  msg.content_type = content_type;
  if (data && len > 0) msg.data.assign(data, data + len);

  if (t->delivery == DeliveryMode::Buffered) {
    t->ring.push_back(msg);
    while (t->ring.size() > t->capacity) t->ring.pop_front();
  }

  notify_locked(topic, msg);
  out = msg;
  mode_out = t->delivery;
  return true;
}

bool TopicHub::reserve_durable(const std::string& topic, const std::uint8_t* data,
                               std::size_t len, const std::string& content_type, PubMessage& out,
                               std::string& err_code, std::string& err) {
  if (topic.empty()) {
    err_code = "bad_request";
    err = "empty topic";
    return false;
  }
  if (len > kMaxMessageBytes) {
    err_code = "payload_too_large";
    err = "message exceeds 1 MiB";
    return false;
  }

  std::lock_guard lock(mu_);
  auto it = topics_.find(topic);
  if (it == topics_.end()) {
    err_code = "not_found";
    err = "topic not created";
    return false;
  }
  if (it->second.delivery != DeliveryMode::Durable) {
    err_code = "mode_mismatch";
    err = "topic delivery mode mismatch";
    return false;
  }

  out.id = it->second.next_id++;
  out.ts_ms = now_ms();
  out.content_type = content_type;
  out.data.clear();
  if (data && len > 0) out.data.assign(data, data + len);
  return true;
}

void TopicHub::commit_durable(const std::string& topic, const PubMessage& msg) {
  std::lock_guard lock(mu_);
  notify_locked(topic, msg);
}

std::vector<PubMessage> TopicHub::buffered_since(const std::string& topic,
                                                 std::uint64_t after_id) const {
  std::vector<PubMessage> out;
  std::lock_guard lock(mu_);
  auto it = topics_.find(topic);
  if (it == topics_.end() || it->second.delivery != DeliveryMode::Buffered) return out;
  for (const auto& m : it->second.ring) {
    if (m.id > after_id) out.push_back(m);
  }
  return out;
}

bool TopicHub::subscribe(const std::string& topic, std::uint64_t after_id, int timeout_ms,
                         std::vector<PubMessage>& out) {
  {
    std::lock_guard lock(mu_);
    auto it = topics_.find(topic);
    if (it != topics_.end() && it->second.delivery == DeliveryMode::Buffered) {
      for (const auto& m : it->second.ring) {
        if (m.id > after_id) out.push_back(m);
      }
      if (!out.empty()) return true;
    }
  }

  auto waiter = std::make_shared<Waiter>();
  waiter->topic = topic;
  waiter->after_id = after_id;

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
  out = std::move(waiter->messages);
  return !out.empty();
}

void TopicHub::shutdown() {
  std::lock_guard lock(mu_);
  stopped_ = true;
  for (auto& w : waiters_) w->done = true;
  waiters_.clear();
  cv_.notify_all();
}

std::uint64_t TopicHub::tip_id(const std::string& topic) const {
  std::lock_guard lock(mu_);
  auto it = topics_.find(topic);
  if (it == topics_.end() || it->second.next_id == 0) return 0;
  return it->second.next_id - 1;
}

}  // namespace aios
