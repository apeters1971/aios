#include "client/deque.hpp"

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

struct deque::Impl {
  changelog::Log log;
  std::uint64_t applied_op{0};
  std::vector<changelog::Record> pending;
  explicit Impl(Session& s, const std::string& type, const std::string& name)
      : log(s, type, name) {}
};

deque::deque(Session& session, std::string name, sync_mode mode, bool flush_on_destroy)
    : StlBase(session, "deque", std::move(name), mode, flush_on_destroy),
      impl_(std::make_unique<Impl>(session, "deque", this->name())) {}

deque::~deque() {
  if (flush_on_destroy() && mode() == sync_mode::async && dirty()) {
    try {
      flush();
    } catch (...) {
    }
  }
}

void deque::maybe_compact() {
  try {
    auto m = impl_->log.load_meta();
    if (m.log_bytes > changelog::kAutoCompactBytes) compact();
  } catch (...) {
  }
}

void deque::ensure_fresh_read() const {
  if (mode() == sync_mode::async && local_valid_) return;
  const_cast<deque*>(this)->pull();
}

void deque::pull() {
  auto meta = impl_->log.pull(
      &impl_->applied_op,
      [this](const std::string& snap) { local_ = wire::parse_list_doc(snap, "deque"); },
      [this](const changelog::Record& r) { apply_seq_op(local_, r); });
  (void)meta;
  local_valid_ = true;
}

void deque::persist_op(changelog::Op op, std::vector<std::string> args) {
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

void deque::load() {
  if (mode() == sync_mode::async && dirty()) {
    throw client_error("bad_request", "flush or discard before load");
  }
  impl_->pending.clear();
  impl_->applied_op = 0;
  local_.clear();
  pull();
  clear_dirty();
}

void deque::flush() {
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

void deque::compact() {
  ensure_fresh_read();
  if (mode() == sync_mode::async && dirty()) flush();
  const auto body = wire::make_list_doc(local_, mode(), "deque").dump();
  impl_->log.compact(body, mode(), impl_->applied_op);
  auto m = impl_->log.load_meta();
  impl_->applied_op = m.snapshot_op;
}

void deque::clear() {
  if (mode() == sync_mode::async && !local_valid_) {
    local_.clear();
    local_valid_ = true;
  } else {
    ensure_fresh_read();
    local_.clear();
  }
  persist_op(changelog::Op::Clear, {});
}

std::size_t deque::size() const {
  ensure_fresh_read();
  return local_.size();
}

void deque::push_back(const std::string& v) {
  ensure_fresh_read();
  local_.push_back(v);
  persist_op(changelog::Op::PushBack, {v});
}

void deque::push_front(const std::string& v) {
  ensure_fresh_read();
  local_.insert(local_.begin(), v);
  persist_op(changelog::Op::PushFront, {v});
}

void deque::pop_back() {
  ensure_fresh_read();
  if (local_.empty()) throw client_error("bad_request", "pop_back on empty deque");
  local_.pop_back();
  persist_op(changelog::Op::PopBack, {});
}

void deque::pop_front() {
  ensure_fresh_read();
  if (local_.empty()) throw client_error("bad_request", "pop_front on empty deque");
  local_.erase(local_.begin());
  persist_op(changelog::Op::PopFront, {});
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
  persist_op(changelog::Op::SetAt, {std::to_string(i), v});
}

void deque::insert(std::size_t index, const std::string& v) {
  ensure_fresh_read();
  if (index > local_.size()) throw client_error("bad_request", "insert index out of range");
  local_.insert(local_.begin() + static_cast<std::ptrdiff_t>(index), v);
  persist_op(changelog::Op::Insert, {std::to_string(index), v});
}

void deque::erase(std::size_t index) {
  ensure_fresh_read();
  if (index >= local_.size()) throw client_error("bad_request", "erase index out of range");
  local_.erase(local_.begin() + static_cast<std::ptrdiff_t>(index));
  persist_op(changelog::Op::EraseAt, {std::to_string(index)});
}

std::vector<std::string> deque::snapshot() const {
  ensure_fresh_read();
  return local_;
}

}  // namespace aios
