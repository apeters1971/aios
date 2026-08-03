#include "client/list.hpp"

#include "client/wire.hpp"

namespace aios {

list::list(Session& session, std::string name, sync_mode mode, bool flush_on_destroy)
    : StlBase(session, "list", std::move(name), mode, flush_on_destroy) {}

list::~list() {
  if (flush_on_destroy() && mode() == sync_mode::async && dirty()) {
    try {
      flush();
    } catch (...) {
    }
  }
}

void list::ensure_fresh_read() const {
  if (mode() == sync_mode::async && local_valid_) return;
  auto snap = const_cast<list*>(this)->fetch();
  if (!snap.exists) {
    local_.clear();
    const_cast<list*>(this)->set_cas(0);
  } else {
    local_ = wire::parse_list_doc(snap.body, "list");
    const_cast<list*>(this)->set_cas(snap.cas);
  }
  local_valid_ = true;
}

void list::persist_if_sync() {
  if (mode() != sync_mode::sync) {
    mark_dirty();
    return;
  }
  put_body(wire::make_list_doc(local_, mode(), "list").dump());
  local_valid_ = true;
}

void list::load() {
  if (mode() == sync_mode::async && dirty()) {
    throw client_error("bad_request", "flush or discard before load");
  }
  auto snap = fetch();
  if (!snap.exists) {
    local_.clear();
    set_cas(0);
  } else {
    local_ = wire::parse_list_doc(snap.body, "list");
    set_cas(snap.cas);
  }
  local_valid_ = true;
  clear_dirty();
}

void list::flush() {
  if (mode() != sync_mode::async) {
    throw client_error("bad_request", "flush only valid in async mode");
  }
  if (!local_valid_) load();
  put_body(wire::make_list_doc(local_, mode(), "list").dump());
  local_valid_ = true;
}

void list::clear() {
  if (mode() == sync_mode::async && !local_valid_) {
    local_.clear();
    local_valid_ = true;
  } else {
    ensure_fresh_read();
    local_.clear();
  }
  persist_if_sync();
}

std::size_t list::size() const {
  ensure_fresh_read();
  return local_.size();
}

void list::push_back(const std::string& v) {
  ensure_fresh_read();
  local_.push_back(v);
  persist_if_sync();
}

void list::push_front(const std::string& v) {
  ensure_fresh_read();
  local_.insert(local_.begin(), v);
  persist_if_sync();
}

void list::pop_back() {
  ensure_fresh_read();
  if (local_.empty()) throw client_error("bad_request", "pop_back on empty list");
  local_.pop_back();
  persist_if_sync();
}

void list::pop_front() {
  ensure_fresh_read();
  if (local_.empty()) throw client_error("bad_request", "pop_front on empty list");
  local_.erase(local_.begin());
  persist_if_sync();
}

std::string& list::operator[](std::size_t i) {
  ensure_fresh_read();
  if (mode() == sync_mode::async) mark_dirty();
  return local_.at(i);
}

const std::string& list::operator[](std::size_t i) const {
  ensure_fresh_read();
  return local_.at(i);
}

std::string list::at(std::size_t i) const {
  ensure_fresh_read();
  return local_.at(i);
}

void list::insert(std::size_t index, const std::string& v) {
  ensure_fresh_read();
  if (index > local_.size()) throw client_error("bad_request", "insert index out of range");
  local_.insert(local_.begin() + static_cast<std::ptrdiff_t>(index), v);
  persist_if_sync();
}

void list::erase(std::size_t index) {
  ensure_fresh_read();
  if (index >= local_.size()) throw client_error("bad_request", "erase index out of range");
  local_.erase(local_.begin() + static_cast<std::ptrdiff_t>(index));
  persist_if_sync();
}

std::vector<std::string> list::snapshot() const {
  ensure_fresh_read();
  return local_;
}

}  // namespace aios
