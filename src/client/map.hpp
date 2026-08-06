#pragma once

#include "client/changelog.hpp"
#include "client/mode.hpp"
#include "client/session.hpp"
#include "client/stl_base.hpp"
#include "client/stl_codec.hpp"
#include "client/wire.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aios {

template <class Key, class Mapped = Key>
class basic_map : public detail::StlBase {
 public:
  using key_type = Key;
  using mapped_type = Mapped;
  using value_type = std::pair<const Key, Mapped>;

  class reference {
   public:
    reference(basic_map& m, Key key) : m_(&m), key_(std::move(key)) {}
    reference& operator=(const Mapped& v) {
      m_->set(key_, v);
      return *this;
    }
    operator Mapped() const { return m_->at(key_); }

   private:
    basic_map* m_;
    Key key_;
  };

  basic_map(Session& session, std::string name, sync_mode mode = sync_mode::async,
            bool flush_on_destroy = true)
      : StlBase(session, "map", std::move(name), mode, flush_on_destroy),
        impl_(std::make_unique<Impl>(session, "map", this->name())) {}

  ~basic_map() {
    if (flush_on_destroy() && mode() == sync_mode::async && dirty()) {
      try {
        flush();
      } catch (...) {
      }
    }
  }

  basic_map(const basic_map&) = delete;
  basic_map& operator=(const basic_map&) = delete;

  void load() {
    if (mode() == sync_mode::async && dirty()) {
      throw client_error("bad_request", "flush or discard before load");
    }
    impl_->pending.clear();
    impl_->applied_op = 0;
    local_.clear();
    pull();
    clear_dirty();
  }

  void flush() {
    if (mode() != sync_mode::async) {
      throw client_error("bad_request", "flush only valid in async mode");
    }
    if (!local_valid_) load();
    if (!impl_->pending.empty()) {
      impl_->applied_op = impl_->log.append_ops(impl_->pending, mode());
      impl_->pending.clear();
    }
    pull();
    clear_dirty();
    maybe_compact();
  }

  void compact() {
    ensure_fresh_read();
    if (mode() == sync_mode::async && dirty()) flush();
    const auto body = wire::make_map_doc(to_wire(local_), mode()).dump();
    impl_->log.compact(body, mode(), impl_->applied_op);
    auto m = impl_->log.load_meta();
    impl_->applied_op = m.snapshot_op;
  }

  void clear() {
    if (mode() == sync_mode::async && !local_valid_) {
      local_.clear();
      local_valid_ = true;
    } else {
      ensure_fresh_read();
      local_.clear();
    }
    persist_op(changelog::Op::Clear, {});
  }

  std::size_t size() const {
    ensure_fresh_read();
    return local_.size();
  }
  bool empty() const { return size() == 0; }

  bool contains(const Key& key) const {
    ensure_fresh_read();
    return local_.find(key) != local_.end();
  }

  Mapped at(const Key& key) const {
    ensure_fresh_read();
    auto it = local_.find(key);
    if (it == local_.end()) throw client_error("not_found", "key not found");
    return it->second;
  }

  reference operator[](const Key& key) { return reference(*this, key); }

  void insert_or_assign(const Key& key, const Mapped& value) {
    ensure_fresh_read();
    local_[key] = value;
    persist_op(changelog::Op::Put,
               {stl_codec<Key>::to_string(key), stl_codec<Mapped>::to_string(value)});
  }
  void set(const Key& key, const Mapped& value) { insert_or_assign(key, value); }

  std::size_t erase(const Key& key) {
    ensure_fresh_read();
    const auto n = local_.erase(key);
    if (n) persist_op(changelog::Op::Erase, {stl_codec<Key>::to_string(key)});
    return n;
  }

  std::map<Key, Mapped> snapshot() const {
    ensure_fresh_read();
    return local_;
  }

 private:
  struct Impl {
    changelog::Log log;
    std::uint64_t applied_op{0};
    std::vector<changelog::Record> pending;
    Impl(Session& s, const std::string& type, const std::string& name) : log(s, type, name) {}
  };

  static std::map<std::string, std::string> to_wire(const std::map<Key, Mapped>& in) {
    std::map<std::string, std::string> out;
    for (const auto& [k, v] : in) {
      out[stl_codec<Key>::to_string(k)] = stl_codec<Mapped>::to_string(v);
    }
    return out;
  }

  static std::map<Key, Mapped> from_wire(const std::map<std::string, std::string>& in) {
    std::map<Key, Mapped> out;
    for (const auto& [k, v] : in) {
      out[stl_codec<Key>::from_string(k)] = stl_codec<Mapped>::from_string(v);
    }
    return out;
  }

  static void apply_op(std::map<Key, Mapped>& local, const changelog::Record& r) {
    using changelog::Op;
    switch (r.op) {
      case Op::Put:
        if (r.args.size() >= 2) {
          local[stl_codec<Key>::from_string(r.args[0])] = stl_codec<Mapped>::from_string(r.args[1]);
        }
        break;
      case Op::Erase:
        if (!r.args.empty()) local.erase(stl_codec<Key>::from_string(r.args[0]));
        break;
      case Op::Clear:
        local.clear();
        break;
      default:
        break;
    }
  }

  void ensure_fresh_read() const {
    if (mode() == sync_mode::async && local_valid_) return;
    const_cast<basic_map*>(this)->pull();
  }

  void pull() {
    auto meta = impl_->log.pull(
        &impl_->applied_op,
        [this](const std::string& snap) { local_ = from_wire(wire::parse_map_doc(snap)); },
        [this](const changelog::Record& r) { apply_op(local_, r); });
    if (!meta.exists && impl_->applied_op == 0) {
      local_.clear();
    }
    local_valid_ = true;
  }

  void persist_op(changelog::Op op, std::vector<std::string> args) {
    if (mode() != sync_mode::sync) {
      changelog::Record r;
      r.op = op;
      r.args = std::move(args);
      impl_->pending.push_back(std::move(r));
      mark_dirty();
      return;
    }
    const auto id = impl_->log.append_op(op, std::move(args), mode());
    impl_->applied_op = id;
    maybe_compact();
  }

  void maybe_compact() {
    try {
      auto m = impl_->log.load_meta();
      if (m.log_bytes > changelog::kAutoCompactBytes) compact();
    } catch (...) {
    }
  }

  std::unique_ptr<Impl> impl_;
  mutable std::map<Key, Mapped> local_;
  mutable bool local_valid_{false};
};

using map = basic_map<std::string, std::string>;

}  // namespace aios
