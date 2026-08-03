#include "client/set.hpp"

#include "client/wire.hpp"

namespace aios {
namespace {

void apply_set_op(std::set<std::string>& local, const changelog::Record& r) {
  using changelog::Op;
  switch (r.op) {
    case Op::Insert:
      if (!r.args.empty()) local.insert(r.args[0]);
      break;
    case Op::Erase:
      if (!r.args.empty()) local.erase(r.args[0]);
      break;
    case Op::Clear:
      local.clear();
      break;
    default:
      break;
  }
}

}  // namespace

struct set::Impl {
  changelog::Log log;
  std::uint64_t applied_op{0};
  std::vector<changelog::Record> pending;
  explicit Impl(Session& s, const std::string& type, const std::string& name)
      : log(s, type, name) {}
};

set::set(Session& session, std::string name, sync_mode mode, bool flush_on_destroy)
    : StlBase(session, "set", std::move(name), mode, flush_on_destroy),
      impl_(std::make_unique<Impl>(session, "set", this->name())) {}

set::~set() {
  if (flush_on_destroy() && mode() == sync_mode::async && dirty()) {
    try {
      flush();
    } catch (...) {
    }
  }
}

void set::maybe_compact() {
  try {
    auto m = impl_->log.load_meta();
    if (m.log_bytes > changelog::kAutoCompactBytes) compact();
  } catch (...) {
  }
}

void set::ensure_fresh_read() const {
  if (mode() == sync_mode::async && local_valid_) return;
  const_cast<set*>(this)->pull();
}

void set::pull() {
  auto meta = impl_->log.pull(
      &impl_->applied_op,
      [this](const std::string& snap) { local_ = wire::parse_set_doc(snap); },
      [this](const changelog::Record& r) { apply_set_op(local_, r); });
  (void)meta;
  local_valid_ = true;
}

void set::persist_op(changelog::Op op, std::vector<std::string> args) {
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

void set::load() {
  if (mode() == sync_mode::async && dirty()) {
    throw client_error("bad_request", "flush or discard before load");
  }
  impl_->pending.clear();
  impl_->applied_op = 0;
  local_.clear();
  pull();
  clear_dirty();
}

void set::flush() {
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

void set::compact() {
  ensure_fresh_read();
  if (mode() == sync_mode::async && dirty()) flush();
  const auto body = wire::make_set_doc(local_, mode()).dump();
  impl_->log.compact(body, mode(), impl_->applied_op);
  auto m = impl_->log.load_meta();
  impl_->applied_op = m.snapshot_op;
}

void set::clear() {
  if (mode() == sync_mode::async && !local_valid_) {
    local_.clear();
    local_valid_ = true;
  } else {
    ensure_fresh_read();
    local_.clear();
  }
  persist_op(changelog::Op::Clear, {});
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
  if (inserted) persist_op(changelog::Op::Insert, {key});
  return inserted;
}

std::size_t set::erase(const std::string& key) {
  ensure_fresh_read();
  const auto n = local_.erase(key);
  if (n) persist_op(changelog::Op::Erase, {key});
  return n;
}

std::set<std::string> set::snapshot() const {
  ensure_fresh_read();
  return local_;
}

}  // namespace aios
