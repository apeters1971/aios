#pragma once

#include "client/changelog.hpp"
#include "client/mode.hpp"
#include "client/session.hpp"
#include "client/stl_base.hpp"
#include "client/stl_codec.hpp"
#include "client/wire.hpp"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace aios {

template <class Key>
class basic_set : public detail::StlBase {
 public:
  using key_type = Key;
  using value_type = Key;

  basic_set(Session& session, std::string name, sync_mode mode = sync_mode::async,
            bool flush_on_destroy = true)
      : StlBase(session, "set", std::move(name), mode, flush_on_destroy),
        impl_(std::make_unique<Impl>(session, "set", this->name())) {}

  ~basic_set() {
    if (flush_on_destroy() && mode() == sync_mode::async && dirty()) {
      try {
        flush();
      } catch (...) {
      }
    }
  }

  basic_set(const basic_set&) = delete;
  basic_set& operator=(const basic_set&) = delete;

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
      impl_->log.append_ops(impl_->pending, mode());
      auto m = impl_->log.load_meta();
      if (m.next_op > 1) impl_->applied_op = m.next_op - 1;
      impl_->pending.clear();
    }
    clear_dirty();
    maybe_compact();
  }

  void compact() {
    ensure_fresh_read();
    if (mode() == sync_mode::async && dirty()) flush();
    const auto body = wire::make_set_doc(to_wire(local_), mode()).dump();
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

  bool insert(const Key& key) {
    ensure_fresh_read();
    const auto [_, inserted] = local_.insert(key);
    if (inserted) persist_op(changelog::Op::Insert, {stl_codec<Key>::to_string(key)});
    return inserted;
  }

  std::size_t erase(const Key& key) {
    ensure_fresh_read();
    const auto n = local_.erase(key);
    if (n) persist_op(changelog::Op::Erase, {stl_codec<Key>::to_string(key)});
    return n;
  }

  std::set<Key> snapshot() const {
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

  static std::set<std::string> to_wire(const std::set<Key>& in) {
    std::set<std::string> out;
    for (const auto& k : in) out.insert(stl_codec<Key>::to_string(k));
    return out;
  }

  static std::set<Key> from_wire(const std::set<std::string>& in) {
    std::set<Key> out;
    for (const auto& k : in) out.insert(stl_codec<Key>::from_string(k));
    return out;
  }

  static void apply_op(std::set<Key>& local, const changelog::Record& r) {
    using changelog::Op;
    switch (r.op) {
      case Op::Insert:
        if (!r.args.empty()) local.insert(stl_codec<Key>::from_string(r.args[0]));
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
    const_cast<basic_set*>(this)->pull();
  }

  void pull() {
    impl_->log.pull(
        &impl_->applied_op,
        [this](const std::string& snap) { local_ = from_wire(wire::parse_set_doc(snap)); },
        [this](const changelog::Record& r) { apply_op(local_, r); });
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
  mutable std::set<Key> local_;
  mutable bool local_valid_{false};
};

using set = basic_set<std::string>;

}  // namespace aios
