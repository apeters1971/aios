#include "client/list.hpp"

#include "client/wire.hpp"

namespace aios {
namespace {

std::size_t parse_index(const std::string& s) {
  return static_cast<std::size_t>(std::stoull(s));
}

void apply_seq_op(std::vector<std::string>& local, const changelog::Record& r) {
  using changelog::Op;
  switch (r.op) {
    case Op::PushBack:
      if (!r.args.empty()) local.push_back(r.args[0]);
      break;
    case Op::PushFront:
      if (!r.args.empty()) local.insert(local.begin(), r.args[0]);
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
          local.insert(local.begin() + static_cast<std::ptrdiff_t>(idx), r.args[1]);
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
        if (idx < local.size()) local[idx] = r.args[1];
      }
      break;
    case Op::Clear:
      local.clear();
      break;
    default:
      break;
  }
}

}  // namespace

struct list::Impl {
  changelog::Log log;
  std::uint64_t applied_op{0};
  std::vector<changelog::Record> pending;
  explicit Impl(Session& s, const std::string& type, const std::string& name)
      : log(s, type, name) {}
};

list::list(Session& session, std::string name, sync_mode mode, bool flush_on_destroy)
    : StlBase(session, "list", std::move(name), mode, flush_on_destroy),
      impl_(std::make_unique<Impl>(session, "list", this->name())) {}

list::~list() {
  if (flush_on_destroy() && mode() == sync_mode::async && dirty()) {
    try {
      flush();
    } catch (...) {
    }
  }
}

void list::maybe_compact() {
  try {
    auto m = impl_->log.load_meta();
    if (m.log_bytes > changelog::kAutoCompactBytes) compact();
  } catch (...) {
  }
}

void list::ensure_fresh_read() const {
  if (mode() == sync_mode::async && local_valid_) return;
  const_cast<list*>(this)->pull();
}

void list::pull() {
  auto meta = impl_->log.pull(
      &impl_->applied_op,
      [this](const std::string& snap) { local_ = wire::parse_list_doc(snap, "list"); },
      [this](const changelog::Record& r) { apply_seq_op(local_, r); });
  (void)meta;
  local_valid_ = true;
}

void list::persist_op(changelog::Op op, std::vector<std::string> args) {
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

void list::load() {
  if (mode() == sync_mode::async && dirty()) {
    throw client_error("bad_request", "flush or discard before load");
  }
  impl_->pending.clear();
  impl_->applied_op = 0;
  local_.clear();
  pull();
  clear_dirty();
}

void list::flush() {
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

void list::compact() {
  ensure_fresh_read();
  if (mode() == sync_mode::async && dirty()) flush();
  const auto body = wire::make_list_doc(local_, mode(), "list").dump();
  impl_->log.compact(body, mode(), impl_->applied_op);
  auto m = impl_->log.load_meta();
  impl_->applied_op = m.snapshot_op;
}

void list::clear() {
  if (mode() == sync_mode::async && !local_valid_) {
    local_.clear();
    local_valid_ = true;
  } else {
    ensure_fresh_read();
    local_.clear();
  }
  persist_op(changelog::Op::Clear, {});
}

std::size_t list::size() const {
  ensure_fresh_read();
  return local_.size();
}

void list::push_back(const std::string& v) {
  ensure_fresh_read();
  local_.push_back(v);
  persist_op(changelog::Op::PushBack, {v});
}

void list::push_front(const std::string& v) {
  ensure_fresh_read();
  local_.insert(local_.begin(), v);
  persist_op(changelog::Op::PushFront, {v});
}

void list::pop_back() {
  ensure_fresh_read();
  if (local_.empty()) throw client_error("bad_request", "pop_back on empty list");
  local_.pop_back();
  persist_op(changelog::Op::PopBack, {});
}

void list::pop_front() {
  ensure_fresh_read();
  if (local_.empty()) throw client_error("bad_request", "pop_front on empty list");
  local_.erase(local_.begin());
  persist_op(changelog::Op::PopFront, {});
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
  persist_op(changelog::Op::Insert, {std::to_string(index), v});
}

void list::erase(std::size_t index) {
  ensure_fresh_read();
  if (index >= local_.size()) throw client_error("bad_request", "erase index out of range");
  local_.erase(local_.begin() + static_cast<std::ptrdiff_t>(index));
  persist_op(changelog::Op::EraseAt, {std::to_string(index)});
}

std::vector<std::string> list::snapshot() const {
  ensure_fresh_read();
  return local_;
}

}  // namespace aios
