#include "posix/aios_posix.h"
#include "posix/posix_internal.hpp"
#include "posix/posix_layout.hpp"

#include "client/changelog.hpp"
#include "metrics/frontend_io.hpp"
#include "util/base64.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <memory>
#include <queue>
#include <random>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aios {
namespace posix {
namespace {

constexpr uint32_t kOpLink = 1;
constexpr uint32_t kOpUnlink = 2;
constexpr uint32_t kOpRename = 3;

uint64_t cas_from_attrs(const std::unordered_map<std::string, std::string>& attrs) {
  auto it = attrs.find(kCasAttr);
  if (it == attrs.end()) return 0;
  try {
    return static_cast<uint64_t>(std::stoull(it->second));
  } catch (...) {
    return 0;
  }
}

}  // namespace

int validate_dentry_name(const char* name) {
  if (!name || !*name) return -EINVAL;
  if (std::strchr(name, '/')) return -EINVAL;
  if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) return -EINVAL;
  if (std::strlen(name) > 255) return -ENAMETOOLONG;
  return 0;
}

bool verify_dir_link(FsState& st, uint64_t parent, const char* name, uint64_t expected_ino) {
  DirTable verify(st.session, st.volume, parent);
  verify.load();
  auto it = verify.entries().find(name);
  return it != verify.entries().end() && it->second == expected_ino;
}

void delete_orphan_inode(FsState& st, uint64_t ino) {
  try {
    st.session.delete_object(ino_oid(st.volume, ino));
  } catch (...) {
  }
  std::lock_guard lock(st.mu);
  st.inode_cache.erase(ino);
}

namespace {

void delete_file_chunks(FsState& st, uint64_t ino, uint64_t size, uint64_t stripe_unit,
                        uint32_t project_id, uint32_t uid, uint32_t gid) {
  const uint64_t unit = stripe_unit ? stripe_unit : st.stripe_unit;
  const uint64_t nchunk = size == 0 ? 0 : (size + unit - 1) / unit;
  for (uint64_t c = 0; c < nchunk; ++c) {
    try {
      st.session.delete_object(chunk_oid(st.volume, ino, c));
    } catch (...) {
    }
  }
  if (st.quota && size > 0) {
    st.quota->note_delta(project_id, uid, gid, -static_cast<std::int64_t>(size));
  }
}

#define AIOS_POSIX_CATCH_ALL \
  catch (const std::exception&) { \
    return -EIO; \
  } catch (...) { \
    return -EIO; \
  }

}  // namespace

std::string super_oid(const std::string& vol) { return "posix/" + vol + "/super"; }
std::string ino_oid(const std::string& vol, uint64_t ino) {
  return "posix/" + vol + "/ino/" + std::to_string(ino);
}
std::string dir_meta_oid(const std::string& vol, uint64_t ino) {
  return "posix/" + vol + "/dir/" + std::to_string(ino) + "/meta";
}
std::string dir_log_oid(const std::string& vol, uint64_t ino) {
  return "posix/" + vol + "/dir/" + std::to_string(ino) + "/log";
}
std::string dir_snap_oid(const std::string& vol, uint64_t ino) {
  return "posix/" + vol + "/dir/" + std::to_string(ino) + "/snap";
}
std::string chunk_oid(const std::string& vol, uint64_t ino, uint64_t chunk) {
  return "posix/" + vol + "/data/" + std::to_string(ino) + "/c/" + std::to_string(chunk);
}

uint64_t now_ns() {
  using clock = std::chrono::system_clock;
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch())
          .count());
}

int map_error(const client_error& e) {
  if (e.code() == "not_found") return -ENOENT;
  if (e.code() == "conflict") return -EAGAIN;
  if (e.code() == "lock_held") return -EAGAIN;
  if (e.code() == "bad_request") return -EINVAL;
  if (e.code() == "payload_too_large") return -EFBIG;
  if (e.code() == "no_targets") return -ENOSPC;
  return -EIO;
}

constexpr size_t kMaxXattrName = 255;
constexpr size_t kMaxXattrValue = 64 * 1024;
constexpr size_t kMaxXattrCount = 128;
constexpr int kFlockTtlMs = 120000;
#ifdef __APPLE__
constexpr int kXattrMissing = ENOATTR;
#else
constexpr int kXattrMissing = ENODATA;
#endif

bool valid_xattr_name(const char* name) {
  if (!name || !*name) return false;
  return std::strlen(name) <= kMaxXattrName;
}

void fill_stat(const InodeMeta& m, aios_posix_stat* st) {
  if (!st) return;
  std::memset(st, 0, sizeof(*st));
  st->ino = m.ino;
  st->mode = m.mode;
  st->nlink = m.nlink;
  st->uid = m.uid;
  st->gid = m.gid;
  st->size = m.size;
  st->atime_ns = m.atime_ns;
  st->mtime_ns = m.mtime_ns;
  st->ctime_ns = m.ctime_ns;
  st->stripe_unit = m.stripe_unit;
  st->stripe_width = m.stripe_width;
  st->parent_ino = m.parent_ino;
}

int check_access(const aios_posix_cred& cred, const InodeMeta& m, int want) {
  if (want == 0) return 0;
  if (cred.uid == 0) return 0;  // root
  const unsigned mode = m.mode & 0777;
  unsigned bits = 0;
  if (cred.uid == m.uid)
    bits = (mode >> 6) & 7u;
  else if (cred.gid == m.gid)
    bits = (mode >> 3) & 7u;
  else
    bits = mode & 7u;
  if ((static_cast<int>(bits) & want) == want) return 0;
  return -EACCES;
}

int check_sticky_unlink(const aios_posix_cred& cred, const InodeMeta& parent,
                        const InodeMeta& victim) {
  if (cred.uid == 0) return 0;
  if ((parent.mode & S_ISVTX) == 0) return 0;
  if (cred.uid == parent.uid || cred.uid == victim.uid) return 0;
  return -EACCES;
}

InodeMeta inode_from_json(const std::string& body, uint64_t cas_hint) {
  auto j = nlohmann::json::parse(body);
  InodeMeta m;
  m.exists = true;
  m.ino = j.value("ino", static_cast<uint64_t>(0));
  m.mode = j.value("mode", static_cast<uint32_t>(0));
  m.nlink = j.value("nlink", static_cast<uint32_t>(1));
  m.uid = j.value("uid", static_cast<uint32_t>(0));
  m.gid = j.value("gid", static_cast<uint32_t>(0));
  m.project_id = j.value("project_id", static_cast<uint32_t>(0));
  m.parent_ino = j.value("parent_ino", static_cast<uint64_t>(0));
  m.size = j.value("size", static_cast<uint64_t>(0));
  m.atime_ns = j.value("atime_ns", static_cast<uint64_t>(0));
  m.mtime_ns = j.value("mtime_ns", static_cast<uint64_t>(0));
  m.ctime_ns = j.value("ctime_ns", static_cast<uint64_t>(0));
  m.stripe_unit = j.value("stripe_unit", kDefaultStripeUnit);
  m.stripe_width = j.value("stripe_width", kDefaultStripeWidth);
  m.rbytes = j.value("rbytes", static_cast<uint64_t>(0));
  m.rfiles = j.value("rfiles", static_cast<uint64_t>(0));
  m.rdirs = j.value("rdirs", static_cast<uint64_t>(0));
  m.rtime_ns = j.value("rtime_ns", static_cast<uint64_t>(0));
  m.cas = cas_hint;
  if (j.contains("xattrs") && j["xattrs"].is_object()) {
    for (auto it = j["xattrs"].begin(); it != j["xattrs"].end(); ++it) {
      if (!it.value().is_string()) continue;
      std::vector<uint8_t> raw;
      std::string err;
      if (!base64_decode(it.value().get<std::string>(), raw, err)) continue;
      m.xattrs[it.key()] = std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
    }
  }
  return m;
}

std::string inode_to_json(const InodeMeta& m) {
  nlohmann::json j{{"aios_posix_ino", 1},
                   {"ino", m.ino},
                   {"mode", m.mode},
                   {"nlink", m.nlink},
                   {"uid", m.uid},
                   {"gid", m.gid},
                   {"project_id", m.project_id},
                   {"parent_ino", m.parent_ino},
                   {"size", m.size},
                   {"atime_ns", m.atime_ns},
                   {"mtime_ns", m.mtime_ns},
                   {"ctime_ns", m.ctime_ns},
                   {"stripe_unit", m.stripe_unit},
                   {"stripe_width", m.stripe_width},
                   {"rbytes", m.rbytes},
                   {"rfiles", m.rfiles},
                   {"rdirs", m.rdirs},
                   {"rtime_ns", m.rtime_ns}};
  if (!m.xattrs.empty()) {
    nlohmann::json xa = nlohmann::json::object();
    for (const auto& [k, v] : m.xattrs) {
      xa[k] = base64_encode(reinterpret_cast<const uint8_t*>(v.data()), v.size());
    }
    j["xattrs"] = std::move(xa);
  }
  return j.dump();
}

SuperMeta super_from_json(const std::string& body, uint64_t cas_hint) {
  auto j = nlohmann::json::parse(body);
  SuperMeta m;
  m.exists = true;
  m.next_ino = j.value("next_ino", static_cast<uint64_t>(2));
  m.stripe_unit = j.value("stripe_unit", kDefaultStripeUnit);
  m.stripe_width = j.value("stripe_width", kDefaultStripeWidth);
  m.uuid = j.value("uuid", "");
  m.frozen = j.value("frozen", false);
  m.cas = cas_hint;
  return m;
}

std::string super_to_json(const SuperMeta& m) {
  return nlohmann::json{{"aios_posix_super", 1},
                        {"next_ino", m.next_ino},
                        {"stripe_unit", m.stripe_unit},
                        {"stripe_width", m.stripe_width},
                        {"uuid", m.uuid},
                        {"frozen", m.frozen}}
      .dump();
}

namespace {

struct HeldLocks {
  Session* session{nullptr};
  std::vector<std::pair<std::string, std::string>> held;  // oid, token

  ~HeldLocks() {
    if (!session) return;
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
      try {
        session->lock_release(it->first, it->second);
      } catch (...) {
      }
    }
  }

  void acquire_sorted(std::vector<std::string> oids, int ttl_ms = 30000) {
    std::sort(oids.begin(), oids.end());
    oids.erase(std::unique(oids.begin(), oids.end()), oids.end());
    for (const auto& oid : oids) {
      held.emplace_back(oid, session->lock_acquire(oid, ttl_ms).token);
    }
  }

  std::optional<std::string> token_for(const std::string& oid) const {
    for (const auto& [o, t] : held) {
      if (o == oid) return t;
    }
    return std::nullopt;
  }
};

void txn_put_dir(Session& session, const std::string& txn_id, DirTable& dir,
                 const HeldLocks& locks) {
  std::string meta, snap, log;
  dir.plan_compact_bodies(meta, snap, log);
  session.txn_prepare_put(txn_id, dir.snap_oid(), snap, std::nullopt,
                          locks.token_for(dir.snap_oid()));
  session.txn_prepare_put(txn_id, dir.log_oid(), log, std::nullopt,
                          locks.token_for(dir.log_oid()));
  session.txn_prepare_put(txn_id, dir.meta_oid(), meta, dir.meta_cas(),
                          locks.token_for(dir.meta_oid()));
}

}  // namespace

DirTable::DirTable(Session& session, std::string vol, uint64_t ino)
    : session_(session),
      vol_(std::move(vol)),
      ino_(ino),
      meta_oid_(dir_meta_oid(vol_, ino_)),
      log_oid_(dir_log_oid(vol_, ino_)),
      snap_oid_(dir_snap_oid(vol_, ino_)) {}

void DirTable::apply_record(uint64_t /*op_id*/, uint32_t op, const std::vector<std::string>& args) {
  if (op == kOpLink && args.size() >= 2) {
    entries_[args[0]] = static_cast<uint64_t>(std::stoull(args[1]));
  } else if (op == kOpUnlink && args.size() >= 1) {
    entries_.erase(args[0]);
  } else if (op == kOpRename && args.size() >= 2) {
    auto it = entries_.find(args[0]);
    if (it != entries_.end()) {
      const uint64_t child = it->second;
      entries_.erase(it);
      entries_[args[1]] = child;
    }
  }
}

void DirTable::load() {
  entries_.clear();
  next_op_ = 1;
  log_bytes_ = 0;
  snapshot_op_ = 0;
  meta_cas_ = 0;

  auto meta = session_.get_object(meta_oid_);
  if (!meta.exists) return;
  meta_cas_ = cas_from_attrs(meta.attrs);
  auto j = nlohmann::json::parse(meta.body);
  next_op_ = j.value("next_op", static_cast<uint64_t>(1));
  log_bytes_ = j.value("log_bytes", static_cast<uint64_t>(0));
  snapshot_op_ = j.value("snapshot_op", static_cast<uint64_t>(0));

  if (snapshot_op_ > 0) {
    auto snap = session_.get_object(snap_oid_);
    if (snap.exists && !snap.body.empty()) {
      auto sj = nlohmann::json::parse(snap.body);
      if (sj.contains("entries") && sj["entries"].is_object()) {
        for (auto it = sj["entries"].begin(); it != sj["entries"].end(); ++it) {
          entries_[it.key()] = it.value().get<uint64_t>();
        }
      }
    }
  }

  if (log_bytes_ == 0) return;
  auto log = session_.get_range(log_oid_, 0, log_bytes_ - 1);
  if (!log.exists || log.body.empty()) return;
  std::vector<changelog::Record> recs;
  changelog::decode_records(log.body, recs);
  for (const auto& r : recs) {
    if (r.op_id <= snapshot_op_) continue;
    apply_record(r.op_id, static_cast<uint32_t>(r.op), r.args);
  }
}

void DirTable::store_meta() {
  nlohmann::json j{{"aios_posix_dir", 1},
                   {"next_op", next_op_},
                   {"log_bytes", log_bytes_},
                   {"snapshot_op", snapshot_op_},
                   {"snapshot_oid", snap_oid_}};
  meta_cas_ = session_.put_bytes(meta_oid_, j.dump(), {}, meta_cas_, std::nullopt, put_layout_);
}

void DirTable::append_ops(const std::vector<std::pair<uint32_t, std::vector<std::string>>>& ops) {
  if (ops.empty()) return;
  // Reserve op ids via CAS on meta.
  for (int attempt = 0; attempt < 8; ++attempt) {
    load();
    const uint64_t start = next_op_;
    std::string batch;
    uint64_t op = start;
    for (const auto& [code, args] : ops) {
      changelog::Record r;
      r.op_id = op++;
      r.op = static_cast<changelog::Op>(code);
      r.args = args;
      batch += changelog::encode_record(r);
    }
    try {
      next_op_ = op;
      auto ar = session_.append(log_oid_, batch);
      log_bytes_ = ar.size;
      store_meta();
      for (const auto& [code, args] : ops) apply_record(0, code, args);
      compact_if_needed();
      return;
    } catch (const client_error& e) {
      if (e.code() == "conflict" || e.code() == "lock_held") continue;
      throw;
    }
  }
  throw client_error("conflict", "dir changelog append failed");
}

void DirTable::link(const std::string& name, uint64_t child) {
  append_ops({{kOpLink, {name, std::to_string(child)}}});
}

bool DirTable::link_if_absent(const std::string& name, uint64_t child) {
  // Serialize create/link against peers and against directory compaction so two
  // racers cannot both observe a missing name and both return success.
  for (int attempt = 0; attempt < 16; ++attempt) {
    HeldLocks locks;
    locks.session = &session_;
    try {
      locks.acquire_sorted({meta_oid_, log_oid_, snap_oid_});
    } catch (const client_error& e) {
      if (e.code() == "lock_held") {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      throw;
    }
    load();
    if (entries_.count(name)) return false;

    const uint64_t start = next_op_;
    changelog::Record r;
    r.op_id = start;
    r.op = static_cast<changelog::Op>(kOpLink);
    r.args = {name, std::to_string(child)};
    const std::string batch = changelog::encode_record(r);
    try {
      next_op_ = start + 1;
      auto ar = session_.append(log_oid_, batch, locks.token_for(log_oid_));
      log_bytes_ = ar.size;
      meta_cas_ = session_.put_bytes(
          meta_oid_,
          nlohmann::json{{"aios_posix_dir", 1},
                         {"next_op", next_op_},
                         {"log_bytes", log_bytes_},
                         {"snapshot_op", snapshot_op_},
                         {"snapshot_oid", snap_oid_}}
              .dump(),
          {}, meta_cas_, locks.token_for(meta_oid_), put_layout_);
      apply_record(0, kOpLink, r.args);
      // Compaction under the same locks we already hold.
      if (log_bytes_ >= changelog::kAutoCompactBytes) {
        std::string txn_id;
        try {
          txn_id = session_.txn_begin();
          txn_put_dir(session_, txn_id, *this, locks);
          session_.txn_commit(txn_id);
          txn_id.clear();
          snapshot_op_ = next_op_ > 0 ? next_op_ - 1 : 0;
          log_bytes_ = 0;
          meta_cas_ += 1;
        } catch (const client_error&) {
          if (!txn_id.empty()) {
            try {
              session_.txn_abort(txn_id);
            } catch (...) {
            }
          }
          // Link already committed; compaction is best-effort.
        }
      }
      return true;
    } catch (const client_error& e) {
      if (e.code() == "conflict" || e.code() == "lock_held") continue;
      throw;
    }
  }
  throw client_error("conflict", "dir link_if_absent exhausted retries");
}

void DirTable::unlink(const std::string& name) { append_ops({{kOpUnlink, {name}}}); }

void DirTable::rename_same(const std::string& old_name, const std::string& new_name) {
  append_ops({{kOpRename, {old_name, new_name}}});
}

void DirTable::compact_if_needed() {
  if (log_bytes_ < changelog::kAutoCompactBytes) return;
  // Snapshot, log truncation and meta must land together and must not race an
  // append from another client: hold the directory locks and commit through /txn.
  for (int attempt = 0; attempt < 4; ++attempt) {
    HeldLocks locks;
    locks.session = &session_;
    try {
      locks.acquire_sorted({meta_oid_, log_oid_, snap_oid_});
    } catch (const client_error& e) {
      if (e.code() == "lock_held") return;  // another client is compacting
      return;
    }
    // Reload under the locks so records appended during the window survive.
    load();
    if (log_bytes_ < changelog::kAutoCompactBytes) return;
    std::string txn_id;
    try {
      txn_id = session_.txn_begin();
      txn_put_dir(session_, txn_id, *this, locks);
      session_.txn_commit(txn_id);
      txn_id.clear();
      snapshot_op_ = next_op_ > 0 ? next_op_ - 1 : 0;
      log_bytes_ = 0;
      meta_cas_ += 1;
      return;
    } catch (const client_error& e) {
      if (!txn_id.empty()) {
        try {
          session_.txn_abort(txn_id);
        } catch (...) {
        }
      }
      if (e.code() == "conflict" || e.code() == "lock_held") continue;
      return;  // compaction is an optimization; never fail the caller's op
    }
  }
}

void DirTable::plan_compact_bodies(std::string& meta_out, std::string& snap_out,
                                   std::string& log_out) const {
  uint64_t next = next_op_;
  if (next < 2) next = 2;
  const uint64_t snap_op = next - 1;
  nlohmann::json entries = nlohmann::json::object();
  for (const auto& [n, i] : entries_) entries[n] = i;
  snap_out = nlohmann::json{{"entries", entries}}.dump();
  log_out.clear();
  meta_out = nlohmann::json{{"aios_posix_dir", 1},
                            {"next_op", next},
                            {"log_bytes", 0},
                            {"snapshot_op", snap_op},
                            {"snapshot_oid", snap_oid_}}
                 .dump();
}

int rename_cross_dir(FsState& st, uint64_t old_parent, const std::string& old_name,
                     uint64_t new_parent, const std::string& new_name) {
  if (int rc = validate_dentry_name(old_name.c_str())) return rc;
  if (int rc = validate_dentry_name(new_name.c_str())) return rc;

  for (int attempt = 0; attempt < 8; ++attempt) {
    DirTable old_dir(st.session, st.volume, old_parent);
    DirTable new_dir(st.session, st.volume, new_parent);
    old_dir.load();
    new_dir.load();

    auto it = old_dir.entries().find(old_name);
    if (it == old_dir.entries().end()) return -ENOENT;
    const uint64_t ino = it->second;
    if (ino == new_parent) return -EINVAL;  // would create cycle

    auto moved = load_inode(st, ino);
    if (!moved.exists) return -ENOENT;

    uint64_t victim_ino = 0;
    InodeMeta victim;
    if (new_dir.entries().count(new_name)) {
      victim_ino = new_dir.entries().at(new_name);
      if (victim_ino == ino) {
        // Same inode already linked at destination name: just drop source name.
      } else {
        victim = load_inode(st, victim_ino);
        if (victim.exists && S_ISDIR(victim.mode)) return -EISDIR;
        if (moved.exists && S_ISDIR(moved.mode) && victim.exists && S_ISREG(victim.mode)) {
          return -ENOTDIR;
        }
      }
    }

    auto old_p = load_inode(st, old_parent);
    auto new_p = load_inode(st, new_parent);
    if (!old_p.exists || !new_p.exists) return -ENOENT;
    if (!S_ISDIR(old_p.mode) || !S_ISDIR(new_p.mode)) return -ENOTDIR;

    HeldLocks locks;
    locks.session = &st.session;
    std::vector<std::string> lock_oids = {old_dir.meta_oid(), old_dir.log_oid(),
                                          new_dir.meta_oid(), new_dir.log_oid()};
    try {
      locks.acquire_sorted(std::move(lock_oids));
    } catch (const client_error& e) {
      if (e.code() == "lock_held") {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      throw;
    }

    // Reload under locks.
    old_dir.load();
    new_dir.load();
    it = old_dir.entries().find(old_name);
    if (it == old_dir.entries().end()) return -ENOENT;
    if (it->second != ino) continue;  // raced
    if (new_dir.entries().count(new_name)) {
      const uint64_t cur_victim = new_dir.entries().at(new_name);
      if (cur_victim != victim_ino && cur_victim != ino) continue;
    } else if (victim_ino != 0 && victim_ino != ino) {
      continue;  // victim disappeared; replan
    }

    old_p = load_inode(st, old_parent);
    new_p = load_inode(st, new_parent);
    moved = load_inode(st, ino);
    if (victim_ino && victim_ino != ino) victim = load_inode(st, victim_ino);

    // Apply dentry mutations in memory.
    old_dir.mutable_entries().erase(old_name);
    if (victim_ino && victim_ino != ino) new_dir.mutable_entries().erase(new_name);
    new_dir.mutable_entries()[new_name] = ino;

    const uint64_t ts = now_ns();
    old_p.mtime_ns = old_p.ctime_ns = ts;
    new_p.mtime_ns = new_p.ctime_ns = ts;
    if (S_ISDIR(moved.mode)) {
      if (old_p.nlink > 2) old_p.nlink -= 1;
      new_p.nlink += 1;
    }

    std::string txn_id;
    try {
      txn_id = st.session.txn_begin();
      txn_put_dir(st.session, txn_id, old_dir, locks);
      txn_put_dir(st.session, txn_id, new_dir, locks);
      st.session.txn_prepare_put(txn_id, ino_oid(st.volume, old_parent), inode_to_json(old_p),
                                 old_p.cas);
      st.session.txn_prepare_put(txn_id, ino_oid(st.volume, new_parent), inode_to_json(new_p),
                                 new_p.cas);

      bool delete_victim = false;
      uint64_t gc_size = 0;
      uint64_t gc_stripe_unit = 0;
      uint32_t gc_uid = 0;
      uint32_t gc_gid = 0;
      uint32_t gc_project_id = 0;
      bool gc_chunks = false;
      if (victim_ino && victim_ino != ino && victim.exists) {
        if (victim.nlink > 1) {
          victim.nlink -= 1;
          victim.ctime_ns = ts;
          st.session.txn_prepare_put(txn_id, ino_oid(st.volume, victim_ino),
                                     inode_to_json(victim), victim.cas);
        } else {
          gc_size = victim.size;
          gc_stripe_unit = victim.stripe_unit ? victim.stripe_unit : st.stripe_unit;
          gc_uid = victim.uid;
          gc_gid = victim.gid;
          gc_project_id = victim.project_id;
          gc_chunks = S_ISREG(victim.mode);
          st.session.txn_prepare_delete(txn_id, ino_oid(st.volume, victim_ino));
          delete_victim = true;
        }
      }

      st.session.txn_commit(txn_id);
      txn_id.clear();

      old_p.cas += 1;
      new_p.cas += 1;
      old_p.exists = true;
      new_p.exists = true;
      if (victim_ino && victim_ino != ino && victim.exists && !delete_victim) {
        victim.cas += 1;
      }
      {
        std::lock_guard lock(st.mu);
        st.inode_cache[old_parent] = old_p;
        st.inode_cache[new_parent] = new_p;
        if (victim_ino && victim_ino != ino) {
          if (delete_victim) {
            st.inode_cache.erase(victim_ino);
          } else if (victim.exists) {
            st.inode_cache[victim_ino] = victim;
          }
        }
      }
      if (delete_victim) {
        if (gc_chunks) {
          delete_file_chunks(st, victim_ino, gc_size, gc_stripe_unit, gc_project_id, gc_uid,
                             gc_gid);
        }
      }
      // Primary parent follows the destination directory; reproject if needed.
      const auto old_proj = moved.project_id;
      const bool reproject = moved.project_id != new_p.project_id;
      if (moved.parent_ino != new_parent || reproject) {
        moved.parent_ino = new_parent;
        if (reproject) moved.project_id = new_p.project_id;
        moved.ctime_ns = ts;
        try {
          const uint64_t reproject_ts = ts;
          const uint64_t new_parent_ino = new_parent;
          const uint32_t new_project_id = new_p.project_id;
          store_inode(st, moved, std::nullopt, [reproject_ts, new_parent_ino, reproject,
                                                new_project_id](InodeMeta& next) {
            next.parent_ino = new_parent_ino;
            if (reproject) next.project_id = new_project_id;
            next.ctime_ns = reproject_ts;
          });
          if (reproject && st.quota) {
            st.quota->note_reproject(old_proj, moved.project_id, moved.uid, moved.gid, moved.size);
          }
        } catch (...) {
        }
      }
      mark_rstat_dirty(st, old_parent);
      mark_rstat_dirty(st, new_parent);
      return 0;
    } catch (const client_error& e) {
      if (!txn_id.empty()) {
        try {
          st.session.txn_abort(txn_id);
        } catch (...) {
        }
      }
      if (e.code() == "conflict" || e.code() == "lock_held") {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      return map_error(e);
    }
  }
  return -EAGAIN;
}

int rename_same_dir(FsState& st, uint64_t parent, const std::string& old_name,
                    const std::string& new_name) {
  if (int rc = validate_dentry_name(old_name.c_str())) return rc;
  if (int rc = validate_dentry_name(new_name.c_str())) return rc;
  if (old_name == new_name) return 0;

  for (int attempt = 0; attempt < 8; ++attempt) {
    DirTable dir(st.session, st.volume, parent);
    dir.load();

    auto oit = dir.entries().find(old_name);
    if (oit == dir.entries().end()) return -ENOENT;
    const uint64_t ino = oit->second;

    uint64_t victim_ino = 0;
    InodeMeta victim;
    if (dir.entries().count(new_name)) {
      victim_ino = dir.entries().at(new_name);
      if (victim_ino != ino) {
        victim = load_inode(st, victim_ino);
        if (victim.exists && S_ISDIR(victim.mode)) return -EISDIR;
        auto moved = load_inode(st, ino);
        if (moved.exists && S_ISDIR(moved.mode) && victim.exists && S_ISREG(victim.mode)) {
          return -ENOTDIR;
        }
      }
    }

    auto pmeta = load_inode(st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;

    HeldLocks locks;
    locks.session = &st.session;
    try {
      locks.acquire_sorted({dir.meta_oid(), dir.log_oid()});
    } catch (const client_error& e) {
      if (e.code() == "lock_held") {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      throw;
    }

    dir.load();
    oit = dir.entries().find(old_name);
    if (oit == dir.entries().end()) return -ENOENT;
    if (oit->second != ino) continue;
    if (dir.entries().count(new_name)) {
      const uint64_t cur_victim = dir.entries().at(new_name);
      if (cur_victim != victim_ino && cur_victim != ino) continue;
    } else if (victim_ino != 0 && victim_ino != ino) {
      continue;
    }

    pmeta = load_inode(st, parent);
    if (victim_ino && victim_ino != ino) victim = load_inode(st, victim_ino);

    dir.mutable_entries().erase(old_name);
    if (victim_ino && victim_ino != ino) dir.mutable_entries().erase(new_name);
    dir.mutable_entries()[new_name] = ino;

    const uint64_t ts = now_ns();
    pmeta.mtime_ns = pmeta.ctime_ns = ts;

    std::string txn_id;
    try {
      txn_id = st.session.txn_begin();
      txn_put_dir(st.session, txn_id, dir, locks);
      st.session.txn_prepare_put(txn_id, ino_oid(st.volume, parent), inode_to_json(pmeta),
                                 pmeta.cas);

      bool delete_victim = false;
      uint64_t gc_size = 0;
      uint64_t gc_stripe_unit = 0;
      uint32_t gc_uid = 0;
      uint32_t gc_gid = 0;
      uint32_t gc_project_id = 0;
      bool gc_chunks = false;
      if (victim_ino && victim_ino != ino && victim.exists) {
        if (victim.nlink > 1) {
          victim.nlink -= 1;
          victim.ctime_ns = ts;
          st.session.txn_prepare_put(txn_id, ino_oid(st.volume, victim_ino),
                                     inode_to_json(victim), victim.cas);
        } else {
          gc_size = victim.size;
          gc_stripe_unit = victim.stripe_unit ? victim.stripe_unit : st.stripe_unit;
          gc_uid = victim.uid;
          gc_gid = victim.gid;
          gc_project_id = victim.project_id;
          gc_chunks = S_ISREG(victim.mode);
          st.session.txn_prepare_delete(txn_id, ino_oid(st.volume, victim_ino));
          delete_victim = true;
        }
      }

      st.session.txn_commit(txn_id);
      txn_id.clear();

      pmeta.cas += 1;
      pmeta.exists = true;
      if (victim_ino && victim_ino != ino && victim.exists && !delete_victim) {
        victim.cas += 1;
      }
      {
        std::lock_guard lock(st.mu);
        st.inode_cache[parent] = pmeta;
        if (victim_ino && victim_ino != ino) {
          if (delete_victim) {
            st.inode_cache.erase(victim_ino);
          } else if (victim.exists) {
            st.inode_cache[victim_ino] = victim;
          }
        }
      }
      if (delete_victim && gc_chunks) {
        delete_file_chunks(st, victim_ino, gc_size, gc_stripe_unit, gc_project_id, gc_uid,
                           gc_gid);
      }
      mark_rstat_dirty(st, parent);
      return 0;
    } catch (const client_error& e) {
      if (!txn_id.empty()) {
        try {
          st.session.txn_abort(txn_id);
        } catch (...) {
        }
      }
      if (e.code() == "conflict" || e.code() == "lock_held") {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      return map_error(e);
    }
  }
  return -EAGAIN;
}

InodeMeta load_inode(FsState& st, uint64_t ino) {
  {
    std::lock_guard lock(st.mu);
    auto it = st.inode_cache.find(ino);
    if (it != st.inode_cache.end()) return it->second;
  }
  auto snap = st.session.get_object(ino_oid(st.volume, ino));
  if (!snap.exists) {
    InodeMeta m;
    m.ino = ino;
    return m;
  }
  auto m = inode_from_json(snap.body, cas_from_attrs(snap.attrs));
  std::lock_guard lock(st.mu);
  st.inode_cache[ino] = m;
  return m;
}

void store_inode(FsState& st, InodeMeta& m, const std::optional<std::string>& path_for_layout,
                 const InodeReapply& reapply) {
  const auto layout =
      path_for_layout ? meta_layout_for_path(st, *path_for_layout) : meta_layout_for_ino(st, m.ino);
  for (int attempt = 0; attempt < 8; ++attempt) {
    try {
      m.cas = st.session.put_bytes(ino_oid(st.volume, m.ino), inode_to_json(m), {}, m.cas,
                                   std::nullopt, layout);
      m.exists = true;
      std::lock_guard lock(st.mu);
      st.inode_cache[m.ino] = m;
      return;
    } catch (const client_error& e) {
      if (e.code() != "conflict") throw;
      // Another writer changed the record. Re-applying the caller's stale copy would
      // silently discard their fields, so only operations that can restate their own
      // mutation against a fresh record may retry.
      if (!reapply) throw;
      auto fresh = st.session.get_object(ino_oid(st.volume, m.ino));
      if (!fresh.exists) {
        m.cas = 0;
        continue;
      }
      InodeMeta next = inode_from_json(fresh.body, cas_from_attrs(fresh.attrs));
      next.ino = m.ino;
      reapply(next);
      m = std::move(next);
    }
  }
  throw client_error("conflict", "inode store failed");
}

void ensure_super(FsState& st) {
  {
    std::lock_guard lock(st.mu);
    if (st.super.exists) return;
  }
  auto snap = st.session.get_object(super_oid(st.volume));
  if (snap.exists) {
    std::lock_guard lock(st.mu);
    st.super = super_from_json(snap.body, cas_from_attrs(snap.attrs));
    return;
  }
  SuperMeta m;
  m.next_ino = 2;
  m.stripe_unit = st.stripe_unit;
  m.stripe_width = st.stripe_width;
  m.uuid = "fs-" + st.volume;
  m.cas = 0;
  m.cas = st.session.put_bytes(super_oid(st.volume), super_to_json(m), {}, 0);
  m.exists = true;
  std::lock_guard lock(st.mu);
  st.super = m;
}

uint64_t alloc_ino(FsState& st) {
  for (int attempt = 0; attempt < 16; ++attempt) {
    ensure_super(st);
    uint64_t id = 0;
    SuperMeta m;
    {
      std::lock_guard lock(st.mu);
      m = st.super;
      id = m.next_ino++;
    }
    try {
      m.cas = st.session.put_bytes(super_oid(st.volume), super_to_json(m), {}, m.cas);
      m.exists = true;
      std::lock_guard lock(st.mu);
      st.super = m;
      return id;
    } catch (const client_error& e) {
      if (e.code() != "conflict") throw;
      auto snap = st.session.get_object(super_oid(st.volume));
      if (snap.exists) {
        std::lock_guard lock(st.mu);
        st.super = super_from_json(snap.body, cas_from_attrs(snap.attrs));
      }
    }
  }
  throw client_error("conflict", "alloc_ino failed");
}

void ensure_root(FsState& st) {
  auto root = load_inode(st, kRootIno);
  if (root.exists) return;
  const uint64_t ts = now_ns();
  root.ino = kRootIno;
  root.mode = S_IFDIR | 0755;
  root.nlink = 2;
  root.uid = st.default_uid;
  root.gid = st.default_gid;
  root.parent_ino = 0;
  root.size = 0;
  root.atime_ns = root.mtime_ns = root.ctime_ns = ts;
  root.stripe_unit = st.stripe_unit;
  root.stripe_width = st.stripe_width;
  root.cas = 0;
  store_inode(st, root);
  DirTable dir(st.session, st.volume, kRootIno);
  dir.load();  // creates empty on first link
}

void drop_nlink(FsState& st, uint64_t ino) {
  auto m = load_inode(st, ino);
  if (!m.exists) return;
  if (m.nlink > 1) {
    const uint64_t ts = now_ns();
    m.nlink -= 1;
    m.ctime_ns = ts;
    store_inode(st, m, std::nullopt, [ts](InodeMeta& next) {
      if (next.nlink > 0) next.nlink -= 1;
      next.ctime_ns = ts;
    });
    return;
  }
  if (S_ISREG(m.mode)) {
    truncate_file(st, ino, 0);
  }
  st.session.delete_object(ino_oid(st.volume, ino));
  std::string flock_token;
  {
    std::lock_guard lock(st.mu);
    st.inode_cache.erase(ino);
    auto fit = st.flock_tokens.find(ino);
    if (fit != st.flock_tokens.end()) {
      flock_token = std::move(fit->second);
      st.flock_tokens.erase(fit);
    }
  }
  if (!flock_token.empty()) {
    try {
      st.session.lock_release(ino_oid(st.volume, ino), flock_token);
    } catch (...) {
    }
  }
}

void release_all_flocks(FsState& st) {
  std::unordered_map<uint64_t, std::string> held;
  {
    std::lock_guard lock(st.mu);
    held.swap(st.flock_tokens);
  }
  for (const auto& [ino, token] : held) {
    try {
      st.session.lock_release(ino_oid(st.volume, ino), token);
    } catch (...) {
    }
  }
}

int read_file(FsState& st, uint64_t ino, uint64_t offset, void* buf, size_t len, size_t* out_len) {
  if (out_len) *out_len = 0;
  auto meta = load_inode(st, ino);
  if (!meta.exists) return -ENOENT;
  if (!S_ISREG(meta.mode)) return -EISDIR;
  if (offset >= meta.size || len == 0) return 0;
  const uint64_t end = std::min(offset + static_cast<uint64_t>(len), meta.size);
  const size_t want = static_cast<size_t>(end - offset);
  if (st.qos && !st.qos->admit(meta.project_id, meta.uid, meta.gid, want)) return -EAGAIN;
  aios::note_frontend_io(st.frontend_label, false, want);
  auto* out = static_cast<uint8_t*>(buf);
  std::memset(out, 0, want);

  const uint64_t unit = meta.stripe_unit ? meta.stripe_unit : st.stripe_unit;
  uint64_t pos = offset;
  size_t written = 0;
  while (pos < end) {
    const uint64_t chunk = pos / unit;
    const uint64_t chunk_off = pos % unit;
    const uint64_t chunk_end = std::min(end, (chunk + 1) * unit);
    const size_t n = static_cast<size_t>(chunk_end - pos);
    try {
      auto snap = st.session.get_object(chunk_oid(st.volume, ino, chunk));
      if (snap.exists && chunk_off < snap.body.size()) {
        const size_t avail = static_cast<size_t>(snap.body.size() - chunk_off);
        const size_t take = std::min(n, avail);
        std::memcpy(out + written, snap.body.data() + chunk_off, take);
      }
    } catch (const client_error& e) {
      return map_error(e);
    }
    pos += n;
    written += n;
  }
  if (out_len) *out_len = written;
  return 0;
}

int write_file(FsState& st, uint64_t ino, uint64_t offset, const void* buf, size_t len,
               size_t* out_len) {
  if (out_len) *out_len = 0;
  if (len == 0) return 0;
  auto meta = load_inode(st, ino);
  if (!meta.exists) return -ENOENT;
  if (!S_ISREG(meta.mode)) return -EISDIR;
  const uint64_t new_size_pre = std::max(meta.size, offset + static_cast<uint64_t>(len));
  if (new_size_pre > meta.size && st.quota) {
    const auto grow = static_cast<std::int64_t>(new_size_pre - meta.size);
    if (!st.quota->may_grow(meta.project_id, meta.uid, meta.gid, grow)) return -EDQUOT;
  }
  if (st.qos && !st.qos->admit(meta.project_id, meta.uid, meta.gid, len)) return -EBUSY;
  aios::note_frontend_io(st.frontend_label, true, len);
  const uint64_t unit = meta.stripe_unit ? meta.stripe_unit : st.stripe_unit;
  const auto* in = static_cast<const uint8_t*>(buf);
  uint64_t pos = offset;
  size_t done = 0;
  const uint32_t width = meta.stripe_width ? meta.stripe_width : st.stripe_width;
  const auto data_layout = data_layout_for_ino(st, ino);

  while (done < len) {
    struct Job {
      uint64_t chunk;
      uint64_t chunk_off;
      size_t n;
      const uint8_t* data;
    };
    std::vector<Job> jobs;
    while (done < len && jobs.size() < width) {
      const uint64_t chunk = pos / unit;
      const uint64_t chunk_off = pos % unit;
      const size_t n = std::min(static_cast<size_t>(unit - chunk_off), len - done);
      jobs.push_back(Job{chunk, chunk_off, n, in + done});
      pos += n;
      done += n;
    }
    std::vector<std::thread> threads;
    std::atomic<int> err{0};
    try {
      for (const auto& job : jobs) {
        threads.emplace_back([&st, ino, job, &err, data_layout] {
          const std::string oid = chunk_oid(st.volume, ino, job.chunk);
          for (int attempt = 0; attempt < 8; ++attempt) {
            try {
              std::string body;
              uint64_t cas = 0;
              auto existing = st.session.get_object(oid);
              if (existing.exists) {
                body = std::move(existing.body);
                cas = cas_from_attrs(existing.attrs);
              }
              if (body.size() < job.chunk_off + job.n) {
                body.resize(job.chunk_off + job.n, '\0');
              }
              std::memcpy(body.data() + job.chunk_off, job.data, job.n);
              st.session.put_bytes(oid, body, {}, cas, std::nullopt, data_layout);
              return;
            } catch (const client_error& e) {
              if (e.code() == "conflict") continue;
              err.store(map_error(e));
              return;
            } catch (...) {
              err.store(-EIO);
              return;
            }
          }
          err.store(-EAGAIN);
        });
      }
    } catch (...) {
      for (auto& t : threads) {
        if (t.joinable()) t.join();
      }
      return -EIO;
    }
    for (auto& t : threads) t.join();
    if (err.load() != 0) return err.load();
  }

  const uint64_t old_size = meta.size;
  const uint64_t new_size = std::max(meta.size, offset + static_cast<uint64_t>(len));
  const uint64_t write_end = offset + static_cast<uint64_t>(len);
  const uint64_t ts = now_ns();
  meta.size = new_size;
  meta.mtime_ns = meta.ctime_ns = ts;
  try {
    store_inode(st, meta, std::nullopt, [write_end, ts](InodeMeta& next) {
      next.size = std::max(next.size, write_end);
      next.mtime_ns = next.ctime_ns = ts;
    });
  } catch (const client_error& e) {
    return map_error(e);
  }
  if (st.quota && new_size != old_size) {
    st.quota->note_delta(meta.project_id, meta.uid, meta.gid,
                         static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size));
  }
  if (meta.parent_ino != 0) mark_rstat_dirty(st, meta.parent_ino);
  if (out_len) *out_len = len;
  return 0;
}

int truncate_file(FsState& st, uint64_t ino, uint64_t size) {
  auto meta = load_inode(st, ino);
  if (!meta.exists) return -ENOENT;
  if (!S_ISREG(meta.mode)) return -EISDIR;
  if (size > meta.size && st.quota) {
    const auto grow = static_cast<std::int64_t>(size - meta.size);
    if (!st.quota->may_grow(meta.project_id, meta.uid, meta.gid, grow)) return -EDQUOT;
  }
  const uint64_t old_size = meta.size;
  const uint64_t unit = meta.stripe_unit ? meta.stripe_unit : st.stripe_unit;
  if (size < meta.size) {
    const uint64_t first_drop = (size + unit - 1) / unit;
    const uint64_t old_chunks = (meta.size + unit - 1) / unit;
    for (uint64_t c = first_drop; c < old_chunks; ++c) {
      try {
        st.session.delete_object(chunk_oid(st.volume, ino, c));
      } catch (...) {
      }
    }
    if (size > 0) {
      const uint64_t last = (size - 1) / unit;
      const uint64_t keep = size - last * unit;
      const auto data_layout = data_layout_for_ino(st, ino);
      for (int attempt = 0; attempt < 8; ++attempt) {
        try {
          auto snap = st.session.get_object(chunk_oid(st.volume, ino, last));
          if (!snap.exists || snap.body.size() <= keep) break;
          std::string body = snap.body;
          body.resize(static_cast<size_t>(keep));
          const uint64_t cas = cas_from_attrs(snap.attrs);
          st.session.put_bytes(chunk_oid(st.volume, ino, last), body, {}, cas, std::nullopt,
                               data_layout);
          break;
        } catch (const client_error& e) {
          if (e.code() == "conflict") continue;
          return map_error(e);
        }
      }
    }
  }
  const uint64_t ts = now_ns();
  meta.size = size;
  meta.mtime_ns = meta.ctime_ns = ts;
  try {
    store_inode(st, meta, std::nullopt, [size, ts](InodeMeta& next) {
      next.size = size;
      next.mtime_ns = next.ctime_ns = ts;
    });
  } catch (const client_error& e) {
    return map_error(e);
  }
  if (st.quota && size != old_size) {
    st.quota->note_delta(meta.project_id, meta.uid, meta.gid,
                         static_cast<std::int64_t>(size) - static_cast<std::int64_t>(old_size));
  }
  if (meta.parent_ino != 0) mark_rstat_dirty(st, meta.parent_ino);
  return 0;
}

}  // namespace posix
}  // namespace aios

using aios::posix::FsState;
using aios::posix::kRootIno;

struct aios_posix_fs {
  std::unique_ptr<FsState> st;
};

namespace {

struct CallerSlot {
  bool set{false};
  uint32_t uid{0};
  uint32_t gid{0};
};

thread_local std::unordered_map<const aios_posix_fs*, CallerSlot> g_tls_callers;

aios_posix_cred effective_caller(const aios_posix_fs* fs) {
  aios_posix_cred c{};
  if (!fs || !fs->st) return c;
  c.uid = fs->st->default_uid;
  c.gid = fs->st->default_gid;
  auto it = g_tls_callers.find(fs);
  if (it != g_tls_callers.end() && it->second.set) {
    c.uid = it->second.uid;
    c.gid = it->second.gid;
  }
  return c;
}

constexpr int kWantR = 4;
constexpr int kWantW = 2;
constexpr int kWantX = 1;

int ensure_not_frozen(aios_posix_fs* fs) {
  if (!fs || !fs->st) return -EINVAL;
  aios::posix::ensure_super(*fs->st);
  {
    std::lock_guard lock(fs->st->mu);
    if (fs->st->super.frozen) return -EBUSY;
  }
  return 0;
}

}  // namespace

extern "C" {

void aios_posix_set_caller(aios_posix_fs* fs, uint32_t uid, uint32_t gid) {
  if (!fs) return;
  auto& slot = g_tls_callers[fs];
  slot.set = true;
  slot.uid = uid;
  slot.gid = gid;
}

void aios_posix_clear_caller(aios_posix_fs* fs) {
  if (!fs) return;
  g_tls_callers.erase(fs);
}

aios_posix_cred aios_posix_get_caller(const aios_posix_fs* fs) { return effective_caller(fs); }

int aios_posix_access(aios_posix_fs* fs, uint64_t ino, int amode) {
  if (!fs) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (amode == F_OK) return 0;
    int want = 0;
    if (amode & R_OK) want |= kWantR;
    if (amode & W_OK) want |= kWantW;
    if (amode & X_OK) want |= kWantX;
    return aios::posix::check_access(effective_caller(fs), m, want);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

aios_posix_fs* aios_posix_mount(const aios_posix_config* cfg, int* err_out) {
  if (err_out) *err_out = 0;
  if (!cfg || !cfg->endpoint || !cfg->cluster_key) {
    if (err_out) *err_out = EINVAL;
    return nullptr;
  }
  try {
    aios::SessionConfig sc;
    sc.endpoint = cfg->endpoint;
    sc.cluster_key = cfg->cluster_key;
    // Default frontend label "fs" so FUSE/posix object OPS and logical IO are separated from S3/VBD.
    if (cfg->app_label && cfg->app_label[0]) sc.app_label = cfg->app_label;
    else sc.app_label = aios::kFrontendFs;
    auto fs = new aios_posix_fs;
    fs->st = std::make_unique<FsState>(std::move(sc));
    fs->st->volume = (cfg->volume && cfg->volume[0]) ? cfg->volume : "default";
    fs->st->stripe_unit = cfg->stripe_unit ? cfg->stripe_unit : aios::posix::kDefaultStripeUnit;
    fs->st->stripe_width = cfg->stripe_width ? cfg->stripe_width : aios::posix::kDefaultStripeWidth;
    fs->st->default_uid = cfg->uid;
    fs->st->default_gid = cfg->gid;
    fs->st->frontend_label = fs->st->session.app_label().empty() ? aios::kFrontendFs
                                                                 : fs->st->session.app_label();
    fs->st->quota = std::make_unique<aios::posix::QuotaLedger>(fs->st->session, fs->st->volume);
    fs->st->qos = std::make_unique<aios::posix::QosController>(fs->st->session, fs->st->volume);
    fs->st->rstat_interval_ms = cfg->rstat_interval_ms;
    aios::posix::ensure_super(*fs->st);
    aios::posix::ensure_root(*fs->st);
    aios::posix::start_rstat_thread(*fs->st);
    return fs;
  } catch (const aios::client_error& e) {
    if (err_out) *err_out = -aios::posix::map_error(e);
    return nullptr;
  } catch (...) {
    if (err_out) *err_out = EIO;
    return nullptr;
  }
}

void aios_posix_unmount(aios_posix_fs* fs) {
  if (!fs) return;
  g_tls_callers.erase(fs);
  if (fs->st) {
    aios::posix::stop_rstat_thread(*fs->st);
    if (fs->st->quota) fs->st->quota->flush();
    aios::posix::release_all_flocks(*fs->st);
  }
  delete fs;
}

void aios_posix_flush_rstats(aios_posix_fs* fs) {
  if (!fs || !fs->st) return;
  try {
    aios::posix::flush_rstats(*fs->st);
  } catch (...) {
  }
}

int aios_posix_lookup(aios_posix_fs* fs, uint64_t parent, const char* name,
                      aios_posix_stat* st_out) {
  if (!fs || !name || !st_out) return -EINVAL;
  try {
    if (std::strcmp(name, ".") == 0) return aios_posix_getattr(fs, parent, st_out);
    if (std::strcmp(name, "..") == 0) {
      // v1: parent of root is root; otherwise unknown → ENOENT for ..
      if (parent == kRootIno) return aios_posix_getattr(fs, kRootIno, st_out);
      return -ENOENT;
    }
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_access(effective_caller(fs), pmeta, kWantX)) return ac;
    aios::posix::DirTable dir(fs->st->session, fs->st->volume, parent);
    dir.load();
    auto it = dir.entries().find(name);
    if (it == dir.entries().end()) return -ENOENT;
    return aios_posix_getattr(fs, it->second, st_out);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_getattr(aios_posix_fs* fs, uint64_t ino, aios_posix_stat* st_out) {
  if (!fs || !st_out) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    aios::posix::fill_stat(m, st_out);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_readdir(aios_posix_fs* fs, uint64_t ino, uint64_t* offset,
                       aios_posix_dirent* buf, size_t max_entries) {
  if (!fs || !offset || !buf || max_entries == 0) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (!S_ISDIR(m.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantR)) return ac;
    aios::posix::DirTable dir(fs->st->session, fs->st->volume, ino);
    dir.load();
    std::vector<std::pair<std::string, uint64_t>> items;
    items.emplace_back(".", ino);
    items.emplace_back("..", ino == kRootIno ? kRootIno : ino);
    for (const auto& [n, i] : dir.entries()) items.emplace_back(n, i);
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t nwrite = 0;
    while (*offset < items.size() && nwrite < max_entries) {
      const auto& [name, child] = items[static_cast<size_t>(*offset)];
      ++*offset;
      if (name.size() >= sizeof(buf[0].name)) continue;
      auto cm = aios::posix::load_inode(*fs->st, child);
      aios_posix_dirent& d = buf[nwrite];
      std::memset(&d, 0, sizeof(d));
      d.ino = child;
      d.mode = cm.exists ? (cm.mode & S_IFMT) : S_IFREG;
      std::snprintf(d.name, sizeof(d.name), "%s", name.c_str());
      ++nwrite;
    }
    return static_cast<int>(nwrite);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_mkdir(aios_posix_fs* fs, uint64_t parent, const char* name, uint32_t mode,
                     aios_posix_stat* st_out) {
  if (!fs || !name) return -EINVAL;
  if (int rc = aios::posix::validate_dentry_name(name)) return rc;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    const auto cred = effective_caller(fs);
    if (int ac = aios::posix::check_access(cred, pmeta, kWantW | kWantX)) return ac;
    aios::posix::DirTable dir(fs->st->session, fs->st->volume, parent);
    dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, parent));
    dir.load();
    if (dir.entries().count(name)) return -EEXIST;
    const uint64_t ino = aios::posix::alloc_ino(*fs->st);
    const uint64_t ts = aios::posix::now_ns();
    aios::posix::InodeMeta m;
    m.ino = ino;
    m.mode = S_IFDIR | (mode & 0777);
    m.nlink = 2;
    m.uid = cred.uid;
    m.gid = cred.gid;
    m.project_id = pmeta.project_id;
    m.parent_ino = parent;
    m.atime_ns = m.mtime_ns = m.ctime_ns = ts;
    m.stripe_unit = fs->st->stripe_unit;
    m.stripe_width = fs->st->stripe_width;
    {
      const std::string parent_path = aios::posix::path_of_ino(*fs->st, parent);
      const std::string child_path =
          parent_path == "/" ? std::string("/") + name : parent_path + "/" + name;
      aios::posix::store_inode(*fs->st, m, child_path);
    }
    if (!dir.link_if_absent(name, ino)) {
      aios::posix::delete_orphan_inode(*fs->st, ino);
      return -EEXIST;
    }
    pmeta.mtime_ns = pmeta.ctime_ns = ts;
    pmeta.nlink += 1;
    aios::posix::store_inode(*fs->st, pmeta, std::nullopt, [ts](aios::posix::InodeMeta& next) {
      next.mtime_ns = next.ctime_ns = ts;
      next.nlink += 1;
    });
    aios::posix::mark_rstat_dirty(*fs->st, parent);
    if (st_out) aios::posix::fill_stat(m, st_out);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_create(aios_posix_fs* fs, uint64_t parent, const char* name, uint32_t mode,
                      aios_posix_stat* st_out) {
  if (!fs || !name) return -EINVAL;
  if (int rc = aios::posix::validate_dentry_name(name)) return rc;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    const auto cred = effective_caller(fs);
    if (int ac = aios::posix::check_access(cred, pmeta, kWantW | kWantX)) return ac;
    aios::posix::DirTable dir(fs->st->session, fs->st->volume, parent);
    dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, parent));
    dir.load();
    if (dir.entries().count(name)) return -EEXIST;
    const uint64_t ino = aios::posix::alloc_ino(*fs->st);
    const uint64_t ts = aios::posix::now_ns();
    aios::posix::InodeMeta m;
    m.ino = ino;
    m.mode = S_IFREG | (mode & 0777);
    m.nlink = 1;
    m.uid = cred.uid;
    m.gid = cred.gid;
    m.project_id = pmeta.project_id;
    m.parent_ino = parent;
    m.atime_ns = m.mtime_ns = m.ctime_ns = ts;
    m.stripe_unit = fs->st->stripe_unit;
    m.stripe_width = fs->st->stripe_width;
    {
      const std::string parent_path = aios::posix::path_of_ino(*fs->st, parent);
      const std::string child_path =
          parent_path == "/" ? std::string("/") + name : parent_path + "/" + name;
      aios::posix::store_inode(*fs->st, m, child_path);
    }
    if (!dir.link_if_absent(name, ino)) {
      aios::posix::delete_orphan_inode(*fs->st, ino);
      return -EEXIST;
    }
    pmeta.mtime_ns = pmeta.ctime_ns = ts;
    aios::posix::store_inode(*fs->st, pmeta, std::nullopt, [ts](aios::posix::InodeMeta& next) {
      next.mtime_ns = next.ctime_ns = ts;
    });
    aios::posix::mark_rstat_dirty(*fs->st, parent);
    if (st_out) aios::posix::fill_stat(m, st_out);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_unlink(aios_posix_fs* fs, uint64_t parent, const char* name) {
  if (!fs || !name) return -EINVAL;
  if (int rc = aios::posix::validate_dentry_name(name)) return rc;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    const auto cred = effective_caller(fs);
    if (int ac = aios::posix::check_access(cred, pmeta, kWantW | kWantX)) return ac;
    aios::posix::DirTable dir(fs->st->session, fs->st->volume, parent);
    dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, parent));
    dir.load();
    auto it = dir.entries().find(name);
    if (it == dir.entries().end()) return -ENOENT;
    const uint64_t ino = it->second;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (m.exists && S_ISDIR(m.mode)) return -EISDIR;
    if (m.exists) {
      if (int ac = aios::posix::check_sticky_unlink(cred, pmeta, m)) return ac;
    }
    if (m.exists && m.nlink > 1) {
      aios::posix::drop_nlink(*fs->st, ino);
      dir.unlink(name);
    } else {
      dir.unlink(name);
      if (m.exists) aios::posix::drop_nlink(*fs->st, ino);
    }
    aios::posix::mark_rstat_dirty(*fs->st, parent);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_link(aios_posix_fs* fs, uint64_t old_parent, const char* old_name,
                    uint64_t new_parent, const char* new_name) {
  if (!fs || !old_name || !new_name) return -EINVAL;
  if (int rc = aios::posix::validate_dentry_name(new_name)) return rc;
  if (int fr = ensure_not_frozen(fs)) return fr;
  try {
    const auto cred = effective_caller(fs);
    auto op = aios::posix::load_inode(*fs->st, old_parent);
    if (!op.exists) return -ENOENT;
    if (!S_ISDIR(op.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_access(cred, op, kWantX)) return ac;
    aios::posix::DirTable old_dir(fs->st->session, fs->st->volume, old_parent);
    old_dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, old_parent));
    old_dir.load();
    auto it = old_dir.entries().find(old_name);
    if (it == old_dir.entries().end()) return -ENOENT;
    const uint64_t ino = it->second;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (S_ISDIR(m.mode)) return -EPERM;

    aios::posix::DirTable new_dir(fs->st->session, fs->st->volume, new_parent);
    new_dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, new_parent));
    new_dir.load();
    if (new_dir.entries().count(new_name)) return -EEXIST;
    auto np = aios::posix::load_inode(*fs->st, new_parent);
    if (!np.exists) return -ENOENT;
    if (!S_ISDIR(np.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_access(cred, np, kWantW | kWantX)) return ac;

    // Bump nlink before creating the new dentry (safer on crash).
    // Primary parent_ino is unchanged; only the destination dir is dirty for rstat.
    const uint64_t ts = aios::posix::now_ns();
    m.nlink += 1;
    m.ctime_ns = ts;
    aios::posix::store_inode(*fs->st, m, std::nullopt, [ts](aios::posix::InodeMeta& next) {
      next.nlink += 1;
      next.ctime_ns = ts;
    });
    if (!new_dir.link_if_absent(new_name, ino)) {
      // Roll back the nlink bump; the name lost the race.
      aios::posix::store_inode(*fs->st, m, std::nullopt, [ts](aios::posix::InodeMeta& next) {
        if (next.nlink > 0) next.nlink -= 1;
        next.ctime_ns = ts;
      });
      return -EEXIST;
    }
    np.mtime_ns = np.ctime_ns = ts;
    aios::posix::store_inode(*fs->st, np, std::nullopt, [ts](aios::posix::InodeMeta& next) {
      next.mtime_ns = next.ctime_ns = ts;
    });
    aios::posix::mark_rstat_dirty(*fs->st, new_parent);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_rmdir(aios_posix_fs* fs, uint64_t parent, const char* name) {
  if (!fs || !name) return -EINVAL;
  if (int rc = aios::posix::validate_dentry_name(name)) return rc;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    const auto cred = effective_caller(fs);
    if (int ac = aios::posix::check_access(cred, pmeta, kWantW | kWantX)) return ac;
    aios::posix::DirTable dir(fs->st->session, fs->st->volume, parent);
    dir.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, parent));
    dir.load();
    auto it = dir.entries().find(name);
    if (it == dir.entries().end()) return -ENOENT;
    const uint64_t ino = it->second;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists || !S_ISDIR(m.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_sticky_unlink(cred, pmeta, m)) return ac;
    aios::posix::DirTable child(fs->st->session, fs->st->volume, ino);
    child.set_put_layout(aios::posix::meta_layout_for_ino(*fs->st, ino));
    child.load();
    if (!child.entries().empty()) return -ENOTEMPTY;
    dir.unlink(name);
    fs->st->session.delete_object(aios::posix::ino_oid(fs->st->volume, ino));
    pmeta = aios::posix::load_inode(*fs->st, parent);
    if (pmeta.exists && pmeta.nlink > 2) {
      const uint64_t ts = aios::posix::now_ns();
      aios::posix::store_inode(*fs->st, pmeta, std::nullopt, [ts](aios::posix::InodeMeta& next) {
        if (next.nlink > 2) next.nlink -= 1;
        next.mtime_ns = next.ctime_ns = ts;
      });
    }
    {
      std::lock_guard lock(fs->st->mu);
      fs->st->inode_cache.erase(ino);
    }
    aios::posix::mark_rstat_dirty(*fs->st, parent);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_rename(aios_posix_fs* fs, uint64_t old_parent, const char* old_name,
                      uint64_t new_parent, const char* new_name) {
  if (!fs || !old_name || !new_name) return -EINVAL;
  if (int fr = ensure_not_frozen(fs)) return fr;
  try {
    const auto cred = effective_caller(fs);
    auto op_meta = aios::posix::load_inode(*fs->st, old_parent);
    if (!op_meta.exists) return -ENOENT;
    if (!S_ISDIR(op_meta.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_access(cred, op_meta, kWantW | kWantX)) return ac;
    auto np_meta = aios::posix::load_inode(*fs->st, new_parent);
    if (!np_meta.exists) return -ENOENT;
    if (!S_ISDIR(np_meta.mode)) return -ENOTDIR;
    if (int ac = aios::posix::check_access(cred, np_meta, kWantW | kWantX)) return ac;

    // Cross layout-rule rename is not in-place; clients should copy (EXDEV).
    {
      auto join_path = [](const std::string& dir, const char* name) {
        if (dir == "/") return std::string("/") + name;
        return dir + "/" + name;
      };
      const std::string src =
          join_path(aios::posix::path_of_ino(*fs->st, old_parent), old_name);
      const std::string dst =
          join_path(aios::posix::path_of_ino(*fs->st, new_parent), new_name);
      if (aios::posix::layout_domains_differ(*fs->st, src, dst)) return -EXDEV;
    }

    if (old_parent == new_parent) {
      aios::posix::DirTable dir(fs->st->session, fs->st->volume, old_parent);
      dir.load();
      auto oit = dir.entries().find(old_name);
      if (oit == dir.entries().end()) return -ENOENT;
      auto src = aios::posix::load_inode(*fs->st, oit->second);
      if (src.exists) {
        if (int ac = aios::posix::check_sticky_unlink(cred, op_meta, src)) return ac;
      }
      if (dir.entries().count(new_name) && std::strcmp(old_name, new_name) != 0) {
        auto tit = dir.entries().find(new_name);
        auto tm = aios::posix::load_inode(*fs->st, tit->second);
        if (tm.exists) {
          if (int ac = aios::posix::check_sticky_unlink(cred, op_meta, tm)) return ac;
        }
      }
      return aios::posix::rename_same_dir(*fs->st, old_parent, old_name, new_name);
    }
    // Cross-dir: sticky checks on source and optional victim.
    {
      aios::posix::DirTable dir(fs->st->session, fs->st->volume, old_parent);
      dir.load();
      auto oit = dir.entries().find(old_name);
      if (oit == dir.entries().end()) return -ENOENT;
      auto src = aios::posix::load_inode(*fs->st, oit->second);
      if (src.exists) {
        if (int ac = aios::posix::check_sticky_unlink(cred, op_meta, src)) return ac;
      }
      aios::posix::DirTable ndir(fs->st->session, fs->st->volume, new_parent);
      ndir.load();
      auto tit = ndir.entries().find(new_name);
      if (tit != ndir.entries().end()) {
        auto tm = aios::posix::load_inode(*fs->st, tit->second);
        if (tm.exists) {
          if (int ac = aios::posix::check_sticky_unlink(cred, np_meta, tm)) return ac;
        }
      }
    }
    return aios::posix::rename_cross_dir(*fs->st, old_parent, old_name, new_parent, new_name);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_read(aios_posix_fs* fs, uint64_t ino, uint64_t offset, void* buf, size_t len,
                    size_t* out_len) {
  if (!fs || !buf) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantR)) return ac;
    return aios::posix::read_file(*fs->st, ino, offset, buf, len, out_len);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_write(aios_posix_fs* fs, uint64_t ino, uint64_t offset, const void* buf,
                     size_t len, size_t* out_len) {
  if (!fs || !buf) return -EINVAL;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantW)) return ac;
    return aios::posix::write_file(*fs->st, ino, offset, buf, len, out_len);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_truncate(aios_posix_fs* fs, uint64_t ino, uint64_t size) {
  if (!fs) return -EINVAL;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantW)) return ac;
    return aios::posix::truncate_file(*fs->st, ino, size);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_setattr(aios_posix_fs* fs, uint64_t ino, const aios_posix_stat* st,
                       uint32_t to_set) {
  if (!fs || !st) return -EINVAL;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    const auto cred = effective_caller(fs);
    if (to_set & (AIOS_POSIX_SET_UID | AIOS_POSIX_SET_GID)) {
      if (cred.uid != 0) return -EPERM;
    }
    if (to_set & AIOS_POSIX_SET_MODE) {
      if (cred.uid != 0 && cred.uid != m.uid) return -EPERM;
    }
    if (to_set & (AIOS_POSIX_SET_SIZE | AIOS_POSIX_SET_MTIME | AIOS_POSIX_SET_ATIME)) {
      if (int ac = aios::posix::check_access(cred, m, kWantW)) return ac;
    }
    const auto old_uid = m.uid;
    const auto old_gid = m.gid;
    const uint64_t ts = aios::posix::now_ns();
    const uint32_t new_mode = st->mode;
    const uint32_t new_uid = st->uid;
    const uint32_t new_gid = st->gid;
    const uint64_t new_mtime = st->mtime_ns;
    const uint64_t new_atime = st->atime_ns;
    if (to_set & AIOS_POSIX_SET_MODE) m.mode = (m.mode & S_IFMT) | (st->mode & 07777);
    if (to_set & AIOS_POSIX_SET_UID) m.uid = st->uid;
    if (to_set & AIOS_POSIX_SET_GID) m.gid = st->gid;
    if (to_set & AIOS_POSIX_SET_MTIME) m.mtime_ns = st->mtime_ns;
    if (to_set & AIOS_POSIX_SET_ATIME) m.atime_ns = st->atime_ns;
    m.ctime_ns = ts;
    aios::posix::store_inode(*fs->st, m, std::nullopt,
                             [to_set, new_mode, new_uid, new_gid, new_mtime, new_atime,
                              ts](aios::posix::InodeMeta& next) {
                               if (to_set & AIOS_POSIX_SET_MODE) {
                                 next.mode = (next.mode & S_IFMT) | (new_mode & 07777);
                               }
                               if (to_set & AIOS_POSIX_SET_UID) next.uid = new_uid;
                               if (to_set & AIOS_POSIX_SET_GID) next.gid = new_gid;
                               if (to_set & AIOS_POSIX_SET_MTIME) next.mtime_ns = new_mtime;
                               if (to_set & AIOS_POSIX_SET_ATIME) next.atime_ns = new_atime;
                               next.ctime_ns = ts;
                             });
    if (fs->st->quota && (to_set & (AIOS_POSIX_SET_UID | AIOS_POSIX_SET_GID)) &&
        (old_uid != m.uid || old_gid != m.gid)) {
      fs->st->quota->note_chown(m.project_id, old_uid, old_gid, m.uid, m.gid, m.size);
    }
    if (to_set & AIOS_POSIX_SET_SIZE) {
      int rc = aios::posix::truncate_file(*fs->st, ino, st->size);
      if (rc) return rc;
    }
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_fsync(aios_posix_fs* fs, uint64_t ino) {
  if (!fs) return -EINVAL;
  // Writes are committed per chunk PUT; refresh inode from server.
  try {
    std::lock_guard lock(fs->st->mu);
    fs->st->inode_cache.erase(ino);
    return 0;
  } catch (...) {
    return -EIO;
  }
}

int aios_posix_statfs(aios_posix_fs* fs, aios_posix_statvfs* st_out) {
  if (!fs || !st_out) return -EINVAL;
  std::memset(st_out, 0, sizeof(*st_out));
  st_out->bsize = 4096;
  st_out->blocks = 1ull << 40;
  st_out->bfree = st_out->blocks / 2;
  st_out->bavail = st_out->bfree;
  st_out->files = 1ull << 32;
  st_out->ffree = st_out->files / 2;
  st_out->namemax = 255;
  return 0;
}

int aios_posix_setxattr(aios_posix_fs* fs, uint64_t ino, const char* name, const void* value,
                        size_t size, int flags) {
  if (!fs || !aios::posix::valid_xattr_name(name)) return -EINVAL;
  if (aios::posix::is_rstat_xattr(name)) return -EPERM;
  if (size > aios::posix::kMaxXattrValue) return -E2BIG;
  if (size > 0 && !value) return -EINVAL;
  try {
    if (int fr = ensure_not_frozen(fs)) return fr;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    const auto cred = effective_caller(fs);
    if (cred.uid != 0 && cred.uid != m.uid) {
      if (int ac = aios::posix::check_access(cred, m, kWantW)) return ac;
    }
    const bool present = m.xattrs.count(name) != 0;
    if ((flags & AIOS_POSIX_XATTR_CREATE) && present) return -EEXIST;
    if ((flags & AIOS_POSIX_XATTR_REPLACE) && !present) return -aios::posix::kXattrMissing;
    if (!present && m.xattrs.size() >= aios::posix::kMaxXattrCount) return -ENOSPC;
    const std::string xname = name;
    const std::string xval(static_cast<const char*>(value), size);
    const uint64_t ts = aios::posix::now_ns();
    m.xattrs[xname] = xval;
    m.ctime_ns = ts;
    aios::posix::store_inode(*fs->st, m, std::nullopt,
                             [xname, xval, ts](aios::posix::InodeMeta& next) {
                               next.xattrs[xname] = xval;
                               next.ctime_ns = ts;
                             });
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_getxattr(aios_posix_fs* fs, uint64_t ino, const char* name, void* value,
                        size_t size) {
  if (!fs || !aios::posix::valid_xattr_name(name)) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantR)) return ac;
    if (aios::posix::is_rstat_xattr(name)) {
      return aios::posix::get_rstat_xattr(m, name, value, size);
    }
    auto it = m.xattrs.find(name);
    if (it == m.xattrs.end()) return -aios::posix::kXattrMissing;
    if (size == 0) return static_cast<int>(it->second.size());
    if (size < it->second.size()) return -ERANGE;
    if (value && !it->second.empty()) {
      std::memcpy(value, it->second.data(), it->second.size());
    }
    return static_cast<int>(it->second.size());
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_listxattr(aios_posix_fs* fs, uint64_t ino, char* list, size_t size) {
  if (!fs) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (int ac = aios::posix::check_access(effective_caller(fs), m, kWantR)) return ac;
    static const char* kVirt[] = {aios::posix::kRstatXattrRbytes, aios::posix::kRstatXattrRfiles,
                                  aios::posix::kRstatXattrRdirs, aios::posix::kRstatXattrRtime};
    size_t need = 0;
    for (const auto& [k, _] : m.xattrs) need += k.size() + 1;
    if (S_ISDIR(m.mode)) {
      for (const char* v : kVirt) need += std::strlen(v) + 1;
    }
    if (size == 0) return static_cast<int>(need);
    if (size < need) return -ERANGE;
    if (!list && need > 0) return -EINVAL;
    size_t off = 0;
    for (const auto& [k, _] : m.xattrs) {
      std::memcpy(list + off, k.data(), k.size());
      off += k.size();
      list[off++] = '\0';
    }
    if (S_ISDIR(m.mode)) {
      for (const char* v : kVirt) {
        const size_t n = std::strlen(v);
        std::memcpy(list + off, v, n);
        off += n;
        list[off++] = '\0';
      }
    }
    return static_cast<int>(need);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_removexattr(aios_posix_fs* fs, uint64_t ino, const char* name) {
  if (!fs || !aios::posix::valid_xattr_name(name)) return -EINVAL;
  if (aios::posix::is_rstat_xattr(name)) return -EPERM;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    const auto cred = effective_caller(fs);
    if (cred.uid != 0 && cred.uid != m.uid) {
      if (int ac = aios::posix::check_access(cred, m, kWantW)) return ac;
    }
    if (!m.xattrs.erase(name)) return -aios::posix::kXattrMissing;
    const std::string xname = name;
    const uint64_t ts = aios::posix::now_ns();
    m.ctime_ns = ts;
    aios::posix::store_inode(*fs->st, m, std::nullopt, [xname, ts](aios::posix::InodeMeta& next) {
      next.xattrs.erase(xname);
      next.ctime_ns = ts;
    });
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

namespace {

std::string normalize_snap_path(const char* path) {
  if (!path || !*path) return "/";
  std::string p(path);
  if (p.front() != '/') p = "/" + p;
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

int resolve_path_ino_local(aios_posix_fs* fs, const std::string& norm, uint64_t* out_ino) {
  uint64_t ino = aios::posix::kRootIno;
  if (norm == "/") {
    *out_ino = ino;
    return 0;
  }
  std::string rest = norm.substr(1);
  while (!rest.empty()) {
    const auto slash = rest.find('/');
    const std::string comp = slash == std::string::npos ? rest : rest.substr(0, slash);
    rest = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    if (comp.empty() || comp == ".") continue;
    if (comp == "..") return -EINVAL;
    aios_posix_stat st{};
    int rc = aios_posix_lookup(fs, ino, comp.c_str(), &st);
    if (rc) return rc;
    ino = st.ino;
  }
  *out_ino = ino;
  return 0;
}

void collect_subtree_oids_session(aios::posix::FsState& st, uint64_t root_ino,
                                  std::vector<std::string>& oids) {
  oids.clear();
  oids.push_back(aios::posix::super_oid(st.volume));
  std::queue<uint64_t> q;
  std::unordered_set<uint64_t> seen;
  q.push(root_ino);
  seen.insert(root_ino);
  while (!q.empty()) {
    const uint64_t ino = q.front();
    q.pop();
    auto m = aios::posix::load_inode(st, ino);
    if (!m.exists) continue;
    oids.push_back(aios::posix::ino_oid(st.volume, ino));
    if (S_ISDIR(m.mode)) {
      oids.push_back(aios::posix::dir_meta_oid(st.volume, ino));
      oids.push_back(aios::posix::dir_log_oid(st.volume, ino));
      oids.push_back(aios::posix::dir_snap_oid(st.volume, ino));
      aios::posix::DirTable dt(st.session, st.volume, ino);
      dt.load();
      for (const auto& [name, child] : dt.entries()) {
        (void)name;
        if (seen.insert(child).second) q.push(child);
      }
    } else if (S_ISREG(m.mode)) {
      const uint64_t stripe = m.stripe_unit ? m.stripe_unit : aios::posix::kDefaultStripeUnit;
      const uint64_t nchunk = m.size == 0 ? 0 : (m.size + stripe - 1) / stripe;
      for (uint64_t c = 0; c < nchunk; ++c) {
        oids.push_back(aios::posix::chunk_oid(st.volume, ino, c));
      }
    }
  }
}

}  // namespace

int aios_posix_snapshot(aios_posix_fs* fs, char* snap_id_out, size_t snap_id_len) {
  return aios_posix_snapshot_at(fs, "/", snap_id_out, snap_id_len);
}

int aios_posix_snapshot_at(aios_posix_fs* fs, const char* path, char* snap_id_out,
                           size_t snap_id_len) {
  if (!fs || !fs->st || !snap_id_out || snap_id_len < 17) return -EINVAL;
  try {
    aios::posix::ensure_super(*fs->st);
    auto& st = *fs->st;
    const std::string norm = normalize_snap_path(path);
    uint64_t root_ino = aios::posix::kRootIno;
    if (norm != "/") {
      if (int rc = resolve_path_ino_local(fs, norm, &root_ino)) return rc;
    }
    {
      aios::posix::SuperMeta m = st.super;
      m.frozen = true;
      m.cas = st.session.put_bytes(aios::posix::super_oid(st.volume), aios::posix::super_to_json(m),
                                   {}, m.cas);
      m.exists = true;
      st.super = m;
    }
    const auto sid = [&]() {
      static thread_local std::mt19937_64 rng{
          static_cast<std::uint64_t>(
              std::chrono::steady_clock::now().time_since_epoch().count())};
      std::ostringstream oss;
      oss << std::hex << rng() << rng();
      return oss.str();
    }();
    const std::string live_prefix = "posix/" + st.volume + "/";
    const std::string snap_prefix = live_prefix + ".snap/" + sid + "/";
    const std::string snap_marker = live_prefix + ".snap/";
    std::size_t copied = 0;
    try {
      std::vector<std::string> src_oids;
      if (norm == "/") {
        std::string cursor;
        for (;;) {
          auto page = st.session.list_prefix(live_prefix, 256, cursor);
          for (const auto& e : page.objects) {
            if (e.oid.rfind(snap_marker, 0) == 0) continue;
            src_oids.push_back(e.oid);
          }
          if (page.next_cursor.empty()) break;
          cursor = page.next_cursor;
        }
      } else {
        collect_subtree_oids_session(st, root_ino, src_oids);
      }
      for (const auto& oid : src_oids) {
        if (oid.size() < live_prefix.size()) continue;
        const std::string dst = snap_prefix + oid.substr(live_prefix.size());
        auto snap = st.session.get_object(oid);
        if (!snap.exists) continue;
        st.session.put_bytes(dst, snap.body, snap.attrs, std::nullopt);
        ++copied;
      }
      using clock = std::chrono::system_clock;
      const auto created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  clock::now().time_since_epoch())
                                  .count();
      nlohmann::json man{{"aios_backup_manifest", 1},
                         {"kind", "posix"},
                         {"volume", st.volume},
                         {"snap_id", sid},
                         {"path", norm},
                         {"root_ino", root_ino},
                         {"created_ms", created_ms},
                         {"oids", copied}};
      st.session.put_bytes(snap_prefix + "manifest", man.dump(), {}, std::nullopt);
    } catch (...) {
      aios::posix::SuperMeta m = st.super;
      m.frozen = false;
      try {
        m.cas = st.session.put_bytes(aios::posix::super_oid(st.volume),
                                     aios::posix::super_to_json(m), {}, m.cas);
        st.super = m;
      } catch (...) {
      }
      throw;
    }
    {
      aios::posix::SuperMeta m = st.super;
      m.frozen = false;
      m.cas = st.session.put_bytes(aios::posix::super_oid(st.volume), aios::posix::super_to_json(m),
                                   {}, m.cas);
      m.exists = true;
      st.super = m;
    }
    std::snprintf(snap_id_out, snap_id_len, "%s", sid.c_str());
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

int aios_posix_flock(aios_posix_fs* fs, uint64_t ino, int op) {
  if (!fs) return -EINVAL;
  const int cmd = op & (LOCK_SH | LOCK_EX | LOCK_UN);
  const bool nonblock = (op & LOCK_NB) != 0;
  if (cmd != LOCK_SH && cmd != LOCK_EX && cmd != LOCK_UN) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (cmd != LOCK_UN) {
      const int want = (cmd == LOCK_EX) ? kWantW : kWantR;
      if (int ac = aios::posix::check_access(effective_caller(fs), m, want)) return ac;
    }
    const std::string oid = aios::posix::ino_oid(fs->st->volume, ino);

    if (cmd == LOCK_UN) {
      std::string token;
      {
        std::lock_guard lock(fs->st->mu);
        auto it = fs->st->flock_tokens.find(ino);
        if (it == fs->st->flock_tokens.end()) return 0;
        token = it->second;
        fs->st->flock_tokens.erase(it);
      }
      try {
        fs->st->session.lock_release(oid, token);
      } catch (const aios::client_error& e) {
        if (e.code() != "not_found") return aios::posix::map_error(e);
      }
      return 0;
    }

    // LOCK_SH and LOCK_EX both use exclusive cluster locks.
    std::string held;
    {
      std::lock_guard lock(fs->st->mu);
      auto it = fs->st->flock_tokens.find(ino);
      if (it != fs->st->flock_tokens.end()) held = it->second;
    }
    if (!held.empty()) {
      try {
        fs->st->session.lock_renew(oid, held, aios::posix::kFlockTtlMs);
        return 0;
      } catch (const aios::client_error&) {
        std::lock_guard lock(fs->st->mu);
        auto it = fs->st->flock_tokens.find(ino);
        if (it != fs->st->flock_tokens.end() && it->second == held) {
          fs->st->flock_tokens.erase(it);
        }
      }
    }

    std::string token;
    if (nonblock) {
      if (!fs->st->session.lock_try_acquire(oid, token, aios::posix::kFlockTtlMs)) {
        return -EWOULDBLOCK;
      }
    } else {
      // Blocking: poll try_acquire.
      for (int i = 0; i < 300; ++i) {
        if (fs->st->session.lock_try_acquire(oid, token, aios::posix::kFlockTtlMs)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        token.clear();
      }
      if (token.empty()) return -EAGAIN;
    }
    std::lock_guard lock(fs->st->mu);
    fs->st->flock_tokens[ino] = std::move(token);
    return 0;
  } catch (const aios::client_error& e) {
    if (e.code() == "lock_held") return -EWOULDBLOCK;
    return aios::posix::map_error(e);
  }
  AIOS_POSIX_CATCH_ALL
}

}  // extern "C"
