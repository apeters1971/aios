#include "client/unordered_map.hpp"

#include "client/wire.hpp"

namespace aios {

unordered_map::unordered_map(Session& session, std::string name, sync_mode mode,
                             bool flush_on_destroy)
    : StlBase(session, "unordered_map", std::move(name), mode, flush_on_destroy) {}

unordered_map::~unordered_map() {
  if (flush_on_destroy() && mode() == sync_mode::async && dirty()) {
    try {
      flush();
    } catch (...) {
    }
  }
}

void unordered_map::ensure_fresh_read() const {
  if (mode() == sync_mode::async && local_valid_) return;
  auto snap = const_cast<unordered_map*>(this)->fetch();
  if (!snap.exists) {
    local_.clear();
    const_cast<unordered_map*>(this)->set_cas(0);
  } else {
    local_ = wire::parse_unordered_map_doc(snap.body);
    const_cast<unordered_map*>(this)->set_cas(snap.cas);
  }
  local_valid_ = true;
}

void unordered_map::persist_if_sync() {
  if (mode() != sync_mode::sync) {
    mark_dirty();
    return;
  }
  put_body(wire::make_unordered_map_doc(local_, mode()).dump());
  local_valid_ = true;
}

void unordered_map::load() {
  if (mode() == sync_mode::async && dirty()) {
    throw client_error("bad_request", "flush or discard before load");
  }
  auto snap = fetch();
  if (!snap.exists) {
    local_.clear();
    set_cas(0);
  } else {
    local_ = wire::parse_unordered_map_doc(snap.body);
    set_cas(snap.cas);
  }
  local_valid_ = true;
  clear_dirty();
}

void unordered_map::flush() {
  if (mode() != sync_mode::async) {
    throw client_error("bad_request", "flush only valid in async mode");
  }
  if (!local_valid_) load();
  put_body(wire::make_unordered_map_doc(local_, mode()).dump());
  local_valid_ = true;
}

void unordered_map::clear() {
  if (mode() == sync_mode::async && !local_valid_) {
    local_.clear();
    local_valid_ = true;
  } else {
    ensure_fresh_read();
    local_.clear();
  }
  persist_if_sync();
}

std::size_t unordered_map::size() const {
  ensure_fresh_read();
  return local_.size();
}

bool unordered_map::contains(const std::string& key) const {
  ensure_fresh_read();
  return local_.find(key) != local_.end();
}

std::string unordered_map::at(const std::string& key) const {
  ensure_fresh_read();
  auto it = local_.find(key);
  if (it == local_.end()) throw client_error("not_found", "key not found");
  return it->second;
}

unordered_map::reference unordered_map::operator[](const std::string& key) {
  return reference(*this, key);
}

void unordered_map::insert_or_assign(const std::string& key, const std::string& value) {
  ensure_fresh_read();
  local_[key] = value;
  persist_if_sync();
}

std::size_t unordered_map::erase(const std::string& key) {
  ensure_fresh_read();
  const auto n = local_.erase(key);
  if (n) persist_if_sync();
  return n;
}

std::unordered_map<std::string, std::string> unordered_map::snapshot() const {
  ensure_fresh_read();
  return local_;
}

}  // namespace aios
