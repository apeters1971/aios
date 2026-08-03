#include "client/set.hpp"

#include "client/wire.hpp"

namespace aios {

set::set(Session& session, std::string name, sync_mode mode, bool flush_on_destroy)
    : StlBase(session, "set", std::move(name), mode, flush_on_destroy) {}

set::~set() {
  if (flush_on_destroy() && mode() == sync_mode::async && dirty()) {
    try {
      flush();
    } catch (...) {
    }
  }
}

void set::ensure_fresh_read() const {
  if (mode() == sync_mode::async && local_valid_) return;
  auto snap = const_cast<set*>(this)->fetch();
  if (!snap.exists) {
    local_.clear();
    const_cast<set*>(this)->set_cas(0);
  } else {
    local_ = wire::parse_set_doc(snap.body);
    const_cast<set*>(this)->set_cas(snap.cas);
  }
  local_valid_ = true;
}

void set::persist_if_sync() {
  if (mode() != sync_mode::sync) {
    mark_dirty();
    return;
  }
  put_body(wire::make_set_doc(local_, mode()).dump());
  local_valid_ = true;
}

void set::load() {
  if (mode() == sync_mode::async && dirty()) {
    throw client_error("bad_request", "flush or discard before load");
  }
  auto snap = fetch();
  if (!snap.exists) {
    local_.clear();
    set_cas(0);
  } else {
    local_ = wire::parse_set_doc(snap.body);
    set_cas(snap.cas);
  }
  local_valid_ = true;
  clear_dirty();
}

void set::flush() {
  if (mode() != sync_mode::async) {
    throw client_error("bad_request", "flush only valid in async mode");
  }
  if (!local_valid_) load();
  put_body(wire::make_set_doc(local_, mode()).dump());
  local_valid_ = true;
}

void set::clear() {
  if (mode() == sync_mode::async && !local_valid_) {
    local_.clear();
    local_valid_ = true;
  } else {
    ensure_fresh_read();
    local_.clear();
  }
  persist_if_sync();
}

std::size_t set::size() const {
  ensure_fresh_read();
  return local_.size();
}

bool set::contains(const std::string& key) const {
  ensure_fresh_read();
  return local_.find(key) != local_.end();
}

bool set::insert(const std::string& key) {
  ensure_fresh_read();
  const auto [_, inserted] = local_.insert(key);
  if (inserted) persist_if_sync();
  return inserted;
}

std::size_t set::erase(const std::string& key) {
  ensure_fresh_read();
  const auto n = local_.erase(key);
  if (n) persist_if_sync();
  return n;
}

std::set<std::string> set::snapshot() const {
  ensure_fresh_read();
  return local_;
}

}  // namespace aios
