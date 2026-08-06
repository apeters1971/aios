#pragma once

#include "client/changelog.hpp"
#include "client/mode.hpp"
#include "client/session.hpp"
#include "client/stl_base.hpp"
#include "client/stl_codec.hpp"
#include "client/wire.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aios {

template <class T = std::string>
class basic_deque : public detail::StlBase {
 public:
  using value_type = T;

  basic_deque(Session& session, std::string name, sync_mode mode = sync_mode::async,
              bool flush_on_destroy = true)
      : StlBase(session, "deque", std::move(name), mode, flush_on_destroy),
        impl_(std::make_unique<Impl>(session, "deque", this->name())) {}

  ~basic_deque() {
    if (flush_on_destroy() && mode() == sync_mode::async && dirty()) {
      try {
        flush();
      } catch (...) {
      }
    }
  }

  basic_deque(const basic_deque&) = delete;
  basic_deque& operator=(const basic_deque&) = delete;

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
    if (mode() == sync_mode::async && dirty()) flush();
    impl_->log.compact(mode(), [this] {
      impl_->applied_op = 0;
      local_.clear();
      pull();
      local_valid_ = true;
      return std::make_pair(wire::make_list_doc(to_wire(local_), mode(), "deque").dump(),
                            impl_->applied_op);
    });
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

  void push_back(const T& v) {
    ensure_fresh_read();
    local_.push_back(v);
    persist_op(changelog::Op::PushBack, {stl_codec<T>::to_string(v)});
  }

  void push_front(const T& v) {
    ensure_fresh_read();
    local_.insert(local_.begin(), v);
    persist_op(changelog::Op::PushFront, {stl_codec<T>::to_string(v)});
  }

  void pop_back() {
    ensure_fresh_read();
    if (local_.empty()) throw client_error("bad_request", "pop_back on empty deque");
    local_.pop_back();
    persist_op(changelog::Op::PopBack, {});
  }

  void pop_front() {
    ensure_fresh_read();
    if (local_.empty()) throw client_error("bad_request", "pop_front on empty deque");
    local_.erase(local_.begin());
    persist_op(changelog::Op::PopFront, {});
  }

  const T& operator[](std::size_t i) const {
    ensure_fresh_read();
    return local_.at(i);
  }
  T at(std::size_t i) const {
    ensure_fresh_read();
    return local_.at(i);
  }

  void set_at(std::size_t i, const T& v) {
    ensure_fresh_read();
    local_.at(i) = v;
    persist_op(changelog::Op::SetAt, {std::to_string(i), stl_codec<T>::to_string(v)});
  }

  void insert(std::size_t index, const T& v) {
    ensure_fresh_read();
    if (index > local_.size()) throw client_error("bad_request", "insert index out of range");
    local_.insert(local_.begin() + static_cast<std::ptrdiff_t>(index), v);
    persist_op(changelog::Op::Insert, {std::to_string(index), stl_codec<T>::to_string(v)});
  }

  void erase(std::size_t index) {
    ensure_fresh_read();
    if (index >= local_.size()) throw client_error("bad_request", "erase index out of range");
    local_.erase(local_.begin() + static_cast<std::ptrdiff_t>(index));
    persist_op(changelog::Op::EraseAt, {std::to_string(index)});
  }

  std::vector<T> snapshot() const {
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

  static std::size_t parse_index(const std::string& s) {
    return static_cast<std::size_t>(stl_codec<std::uint64_t>::from_string(s));
  }

  static std::vector<std::string> to_wire(const std::vector<T>& in) {
    std::vector<std::string> out;
    out.reserve(in.size());
    for (const auto& v : in) out.push_back(stl_codec<T>::to_string(v));
    return out;
  }

  static std::vector<T> from_wire(const std::vector<std::string>& in) {
    std::vector<T> out;
    out.reserve(in.size());
    for (const auto& v : in) out.push_back(stl_codec<T>::from_string(v));
    return out;
  }

  static void apply_op(std::vector<T>& local, const changelog::Record& r) {
    using changelog::Op;
    switch (r.op) {
      case Op::PushBack:
        if (!r.args.empty()) local.push_back(stl_codec<T>::from_string(r.args[0]));
        break;
      case Op::PushFront:
        if (!r.args.empty()) local.insert(local.begin(), stl_codec<T>::from_string(r.args[0]));
        break;
      case Op::PopBack:
        if (!local.empty()) local.pop_back();
        break;
      case Op::PopFront:
        if (!local.empty()) local.erase(local.begin());
        break;
      case Op::Insert:
        if (r.args.size() >= 2) {
          const auto idx = parse_index(r.args[0]);
          if (idx <= local.size()) {
            local.insert(local.begin() + static_cast<std::ptrdiff_t>(idx),
                         stl_codec<T>::from_string(r.args[1]));
          }
        }
        break;
      case Op::EraseAt:
        if (!r.args.empty()) {
          const auto idx = parse_index(r.args[0]);
          if (idx < local.size()) {
            local.erase(local.begin() + static_cast<std::ptrdiff_t>(idx));
          }
        }
        break;
      case Op::SetAt:
        if (r.args.size() >= 2) {
          const auto idx = parse_index(r.args[0]);
          if (idx < local.size()) local[idx] = stl_codec<T>::from_string(r.args[1]);
        }
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
    const_cast<basic_deque*>(this)->pull();
  }

  void pull() {
    impl_->log.pull(
        &impl_->applied_op,
        [this](const std::string& snap) {
          local_ = from_wire(wire::parse_list_doc(snap, "deque"));
        },
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
  mutable std::vector<T> local_;
  mutable bool local_valid_{false};
};

using deque = basic_deque<std::string>;

}  // namespace aios
