#include "client/deque.hpp"

#include "client/wire.hpp"

namespace aios {

deque::deque(Session& session, std::string name, sync_mode mode, bool flush_on_destroy)
    : StlBase(session, "deque", std::move(name), mode, flush_on_destroy) {}

deque::~deque() {
  if (flush_on_destroy() && mode() == sync_mode::async && dirty()) {
    try {
      flush();
    } catch (...) {
    }
  }
}

void deque::ensure_fresh_read() const {
  if (mode() == sync_mode::async && local_valid_) return;
  auto snap = const_cast<deque*>(this)->fetch();
  if (!snap.exists) {
    local_.clear();
    const_cast<deque*>(this)->set_cas(0);
  } else {
    local_ = wire::parse_list_doc(snap.body, "deque");
    const_cast<deque*>(this)->set_cas(snap.cas);
  }
  local_valid_ = true;
}

void deque::persist_if_sync() {
  if (mode() != sync_mode::sync) {
    mark_dirty();
    return;
  }
  put_body(wire::make_list_doc(local_, mode(), "deque").dump());
  local_valid_ = true;
}

void deque::load() {
  if (mode() == sync_mode::async && dirty()) {
    throw client_error("bad_request", "flush or discard before load");
  }
  auto snap = fetch();
  if (!snap.exists) {
    local_.clear();
    set_cas(0);
  } else {
    local_ = wire::parse_list_doc(snap.body, "deque");
    set_cas(snap.cas);
  }
  local_valid_ = true;
  clear_dirty();
}

void deque::flush() {
  if (mode() != sync_mode::async) {
    throw client_error("bad_request", "flush only valid in async mode");
  }
  if (!local_valid_) load();
  put_body(wire::make_list_doc(local_, mode(), "deque").dump());
  local_valid_ = true;
}

void deque::clear() {
  if (mode() == sync_mode::async && !local_valid_) {
    local_.clear();
    local_valid_ = true;
  } else {
    ensure_fresh_read();
    local_.clear();
  }
  persist_if_sync();
}

std::size_t deque::size() const {
  ensure_fresh_read();
  return local_.size();
}

void deque::push_back(const std::string& v) {
  ensure_fresh_read();
  local_.push_back(v);
  persist_if_sync();
}

void deque::push_front(const std::string& v) {
  ensure_fresh_read();
  local_.insert(local_.begin(), v);
  persist_if_sync();
}

void deque::pop_back() {
  ensure_fresh_read();
  if (local_.empty()) throw client_error("bad_request", "pop_back on empty deque");
  local_.pop_back();
  persist_if_sync();
}

void deque::pop_front() {
  ensure_fresh_read();
  if (local_.empty()) throw client_error("bad_request", "pop_front on empty deque");
  local_.erase(local_.begin());
  persist_if_sync();
}

std::string& deque::operator[](std::size_t i) {
  ensure_fresh_read();
  if (mode() == sync_mode::async) mark_dirty();
  return local_.at(i);
}

const std::string& deque::operator[](std::size_t i) const {
  ensure_fresh_read();
  return local_.at(i);
}

std::string deque::at(std::size_t i) const {
  ensure_fresh_read();
  return local_.at(i);
}

void deque::set_at(std::size_t i, const std::string& v) {
  ensure_fresh_read();
  local_.at(i) = v;
  persist_if_sync();
}

void deque::insert(std::size_t index, const std::string& v) {
  ensure_fresh_read();
  if (index > local_.size()) throw client_error("bad_request", "insert index out of range");
  local_.insert(local_.begin() + static_cast<std::ptrdiff_t>(index), v);
  persist_if_sync();
}

void deque::erase(std::size_t index) {
  ensure_fresh_read();
  if (index >= local_.size()) throw client_error("bad_request", "erase index out of range");
  local_.erase(local_.begin() + static_cast<std::ptrdiff_t>(index));
  persist_if_sync();
}

std::vector<std::string> deque::snapshot() const {
  ensure_fresh_read();
  return local_;
}

}  // namespace aios
