#include "client/string.hpp"

#include "client/wire.hpp"

namespace aios {

string::string(Session& session, std::string name, sync_mode mode, bool flush_on_destroy)
    : StlBase(session, "string", std::move(name), mode, flush_on_destroy) {}

string::~string() {
  if (flush_on_destroy() && mode() == sync_mode::async && dirty()) {
    try {
      flush();
    } catch (...) {
    }
  }
}

void string::ensure_fresh_read() const {
  if (mode() == sync_mode::async && local_valid_) return;
  auto snap = const_cast<string*>(this)->fetch();
  if (!snap.exists) {
    local_.clear();
    const_cast<string*>(this)->set_cas(0);
  } else {
    local_ = wire::parse_string_doc(snap.body);
    const_cast<string*>(this)->set_cas(snap.cas);
  }
  local_valid_ = true;
}

void string::persist_if_sync() {
  if (mode() != sync_mode::sync) {
    mark_dirty();
    return;
  }
  put_body(wire::make_string_doc(local_, mode()).dump());
  local_valid_ = true;
}

void string::load() {
  if (mode() == sync_mode::async && dirty()) {
    throw client_error("bad_request", "flush or discard before load");
  }
  auto snap = fetch();
  if (!snap.exists) {
    local_.clear();
    set_cas(0);
  } else {
    local_ = wire::parse_string_doc(snap.body);
    set_cas(snap.cas);
  }
  local_valid_ = true;
  clear_dirty();
}

void string::flush() {
  if (mode() != sync_mode::async) {
    throw client_error("bad_request", "flush only valid in async mode");
  }
  if (!local_valid_) load();
  put_body(wire::make_string_doc(local_, mode()).dump());
  local_valid_ = true;
}

string& string::assign(std::string v) {
  if (mode() == sync_mode::sync) ensure_fresh_read();
  else if (!local_valid_) {
    local_.clear();
    local_valid_ = true;
  }
  local_ = std::move(v);
  persist_if_sync();
  return *this;
}

string& string::append(const std::string& v) {
  ensure_fresh_read();
  local_ += v;
  persist_if_sync();
  return *this;
}

void string::clear() {
  if (mode() == sync_mode::async && !local_valid_) {
    local_.clear();
    local_valid_ = true;
  } else {
    ensure_fresh_read();
    local_.clear();
  }
  persist_if_sync();
}

std::size_t string::size() const {
  ensure_fresh_read();
  return local_.size();
}

const std::string& string::str() const {
  ensure_fresh_read();
  return local_;
}

}  // namespace aios
