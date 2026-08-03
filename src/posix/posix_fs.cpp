#include "posix/aios_posix.h"
#include "posix/posix_internal.hpp"

#include "client/changelog.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <memory>
#include <sys/stat.h>
#include <thread>
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
  m.size = j.value("size", static_cast<uint64_t>(0));
  m.atime_ns = j.value("atime_ns", static_cast<uint64_t>(0));
  m.mtime_ns = j.value("mtime_ns", static_cast<uint64_t>(0));
  m.ctime_ns = j.value("ctime_ns", static_cast<uint64_t>(0));
  m.stripe_unit = j.value("stripe_unit", kDefaultStripeUnit);
  m.stripe_width = j.value("stripe_width", kDefaultStripeWidth);
  m.cas = cas_hint;
  return m;
}

std::string inode_to_json(const InodeMeta& m) {
  nlohmann::json j{{"aios_posix_ino", 1},
                   {"ino", m.ino},
                   {"mode", m.mode},
                   {"nlink", m.nlink},
                   {"uid", m.uid},
                   {"gid", m.gid},
                   {"size", m.size},
                   {"atime_ns", m.atime_ns},
                   {"mtime_ns", m.mtime_ns},
                   {"ctime_ns", m.ctime_ns},
                   {"stripe_unit", m.stripe_unit},
                   {"stripe_width", m.stripe_width}};
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
  m.cas = cas_hint;
  return m;
}

std::string super_to_json(const SuperMeta& m) {
  return nlohmann::json{{"aios_posix_super", 1},
                        {"next_ino", m.next_ino},
                        {"stripe_unit", m.stripe_unit},
                        {"stripe_width", m.stripe_width},
                        {"uuid", m.uuid}}
      .dump();
}

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
  meta_cas_ = session_.put_bytes(meta_oid_, j.dump(), {}, meta_cas_);
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
      if (e.code() != "conflict") throw;
    }
  }
  throw client_error("conflict", "dir changelog append failed");
}

void DirTable::link(const std::string& name, uint64_t child) {
  append_ops({{kOpLink, {name, std::to_string(child)}}});
}

void DirTable::unlink(const std::string& name) { append_ops({{kOpUnlink, {name}}}); }

void DirTable::rename_same(const std::string& old_name, const std::string& new_name) {
  append_ops({{kOpRename, {old_name, new_name}}});
}

void DirTable::compact_if_needed() {
  if (log_bytes_ < changelog::kAutoCompactBytes) return;
  nlohmann::json entries = nlohmann::json::object();
  for (const auto& [n, i] : entries_) entries[n] = i;
  nlohmann::json snap{{"entries", entries}};
  session_.put_bytes(snap_oid_, snap.dump(), {}, std::nullopt);
  snapshot_op_ = next_op_ - 1;
  // Truncate log by replacing with empty object.
  session_.put_bytes(log_oid_, "", {}, std::nullopt);
  log_bytes_ = 0;
  store_meta();
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

void store_inode(FsState& st, InodeMeta& m) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    try {
      m.cas = st.session.put_bytes(ino_oid(st.volume, m.ino), inode_to_json(m), {}, m.cas);
      m.exists = true;
      std::lock_guard lock(st.mu);
      st.inode_cache[m.ino] = m;
      return;
    } catch (const client_error& e) {
      if (e.code() != "conflict") throw;
      auto fresh = st.session.get_object(ino_oid(st.volume, m.ino));
      if (!fresh.exists) {
        m.cas = 0;
        continue;
      }
      // Preserve caller fields but refresh cas.
      m.cas = cas_from_attrs(fresh.attrs);
    }
  }
  throw client_error("conflict", "inode store failed");
}

void ensure_super(FsState& st) {
  auto snap = st.session.get_object(super_oid(st.volume));
  if (snap.exists) {
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
  st.super = m;
}

uint64_t alloc_ino(FsState& st) {
  for (int attempt = 0; attempt < 16; ++attempt) {
    ensure_super(st);
    SuperMeta m = st.super;
    const uint64_t id = m.next_ino++;
    try {
      m.cas = st.session.put_bytes(super_oid(st.volume), super_to_json(m), {}, m.cas);
      m.exists = true;
      st.super = m;
      return id;
    } catch (const client_error& e) {
      if (e.code() != "conflict") throw;
      auto snap = st.session.get_object(super_oid(st.volume));
      if (snap.exists) st.super = super_from_json(snap.body, cas_from_attrs(snap.attrs));
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
  root.size = 0;
  root.atime_ns = root.mtime_ns = root.ctime_ns = ts;
  root.stripe_unit = st.stripe_unit;
  root.stripe_width = st.stripe_width;
  root.cas = 0;
  store_inode(st, root);
  DirTable dir(st.session, st.volume, kRootIno);
  dir.load();  // creates empty on first link
}

int read_file(FsState& st, uint64_t ino, uint64_t offset, void* buf, size_t len, size_t* out_len) {
  if (out_len) *out_len = 0;
  auto meta = load_inode(st, ino);
  if (!meta.exists) return -ENOENT;
  if (!S_ISREG(meta.mode)) return -EISDIR;
  if (offset >= meta.size || len == 0) return 0;
  const uint64_t end = std::min(offset + static_cast<uint64_t>(len), meta.size);
  const size_t want = static_cast<size_t>(end - offset);
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
  const uint64_t unit = meta.stripe_unit ? meta.stripe_unit : st.stripe_unit;
  const auto* in = static_cast<const uint8_t*>(buf);
  uint64_t pos = offset;
  size_t done = 0;
  const uint32_t width = meta.stripe_width ? meta.stripe_width : st.stripe_width;

  while (done < len) {
    // Issue up to `width` chunk writes (sequentially in v1 loop but chunked batches).
    struct Job {
      uint64_t chunk;
      std::string body;
    };
    std::vector<Job> jobs;
    while (done < len && jobs.size() < width) {
      const uint64_t chunk = pos / unit;
      const uint64_t chunk_off = pos % unit;
      const size_t n = std::min(static_cast<size_t>(unit - chunk_off), len - done);
      std::string body;
      try {
        auto existing = st.session.get_object(chunk_oid(st.volume, ino, chunk));
        if (existing.exists) body = std::move(existing.body);
      } catch (const client_error& e) {
        return map_error(e);
      }
      if (body.size() < chunk_off + n) body.resize(chunk_off + n, '\0');
      std::memcpy(body.data() + chunk_off, in + done, n);
      jobs.push_back(Job{chunk, std::move(body)});
      pos += n;
      done += n;
    }
    std::vector<std::thread> threads;
    std::atomic<int> err{0};
    for (auto& job : jobs) {
      threads.emplace_back([&st, ino, &job, &err] {
        try {
          st.session.put_bytes(chunk_oid(st.volume, ino, job.chunk), job.body, {}, std::nullopt);
        } catch (const client_error& e) {
          err.store(map_error(e));
        } catch (...) {
          err.store(-EIO);
        }
      });
    }
    for (auto& t : threads) t.join();
    if (err.load() != 0) return err.load();
  }

  const uint64_t new_size = std::max(meta.size, offset + static_cast<uint64_t>(len));
  meta.size = new_size;
  meta.mtime_ns = now_ns();
  meta.ctime_ns = meta.mtime_ns;
  try {
    store_inode(st, meta);
  } catch (const client_error& e) {
    return map_error(e);
  }
  if (out_len) *out_len = len;
  return 0;
}

int truncate_file(FsState& st, uint64_t ino, uint64_t size) {
  auto meta = load_inode(st, ino);
  if (!meta.exists) return -ENOENT;
  if (!S_ISREG(meta.mode)) return -EISDIR;
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
      try {
        auto snap = st.session.get_object(chunk_oid(st.volume, ino, last));
        if (snap.exists) {
          if (snap.body.size() > keep) {
            snap.body.resize(static_cast<size_t>(keep));
            st.session.put_bytes(chunk_oid(st.volume, ino, last), snap.body, {}, std::nullopt);
          }
        }
      } catch (const client_error& e) {
        return map_error(e);
      }
    }
  }
  meta.size = size;
  meta.mtime_ns = meta.ctime_ns = now_ns();
  try {
    store_inode(st, meta);
  } catch (const client_error& e) {
    return map_error(e);
  }
  return 0;
}

}  // namespace posix
}  // namespace aios

using aios::posix::FsState;
using aios::posix::kRootIno;

struct aios_posix_fs {
  std::unique_ptr<FsState> st;
};

extern "C" {

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
    if (cfg->app_label) sc.app_label = cfg->app_label;
    auto fs = new aios_posix_fs;
    fs->st = std::make_unique<FsState>(std::move(sc));
    fs->st->volume = (cfg->volume && cfg->volume[0]) ? cfg->volume : "default";
    fs->st->stripe_unit = cfg->stripe_unit ? cfg->stripe_unit : aios::posix::kDefaultStripeUnit;
    fs->st->stripe_width = cfg->stripe_width ? cfg->stripe_width : aios::posix::kDefaultStripeWidth;
    fs->st->default_uid = cfg->uid;
    fs->st->default_gid = cfg->gid;
    aios::posix::ensure_super(*fs->st);
    aios::posix::ensure_root(*fs->st);
    return fs;
  } catch (const aios::client_error& e) {
    if (err_out) *err_out = -aios::posix::map_error(e);
    return nullptr;
  } catch (...) {
    if (err_out) *err_out = EIO;
    return nullptr;
  }
}

void aios_posix_unmount(aios_posix_fs* fs) { delete fs; }

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
    aios::posix::DirTable dir(fs->st->session, fs->st->volume, parent);
    dir.load();
    auto it = dir.entries().find(name);
    if (it == dir.entries().end()) return -ENOENT;
    return aios_posix_getattr(fs, it->second, st_out);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
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
}

int aios_posix_readdir(aios_posix_fs* fs, uint64_t ino, uint64_t* offset,
                       aios_posix_dirent* buf, size_t max_entries) {
  if (!fs || !offset || !buf || max_entries == 0) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (!S_ISDIR(m.mode)) return -ENOTDIR;
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
      auto cm = aios::posix::load_inode(*fs->st, child);
      aios_posix_dirent& d = buf[nwrite];
      std::memset(&d, 0, sizeof(d));
      d.ino = child;
      d.mode = cm.exists ? (cm.mode & S_IFMT) : S_IFREG;
      std::snprintf(d.name, sizeof(d.name), "%s", name.c_str());
      ++*offset;
      ++nwrite;
    }
    return static_cast<int>(nwrite);
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
}

int aios_posix_mkdir(aios_posix_fs* fs, uint64_t parent, const char* name, uint32_t mode,
                     aios_posix_stat* st_out) {
  if (!fs || !name || !*name || std::strchr(name, '/')) return -EINVAL;
  try {
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    aios::posix::DirTable dir(fs->st->session, fs->st->volume, parent);
    dir.load();
    if (dir.entries().count(name)) return -EEXIST;
    const uint64_t ino = aios::posix::alloc_ino(*fs->st);
    const uint64_t ts = aios::posix::now_ns();
    aios::posix::InodeMeta m;
    m.ino = ino;
    m.mode = S_IFDIR | (mode & 0777);
    m.nlink = 2;
    m.uid = fs->st->default_uid;
    m.gid = fs->st->default_gid;
    m.atime_ns = m.mtime_ns = m.ctime_ns = ts;
    m.stripe_unit = fs->st->stripe_unit;
    m.stripe_width = fs->st->stripe_width;
    aios::posix::store_inode(*fs->st, m);
    dir.link(name, ino);
    pmeta.mtime_ns = pmeta.ctime_ns = ts;
    pmeta.nlink += 1;
    aios::posix::store_inode(*fs->st, pmeta);
    if (st_out) aios::posix::fill_stat(m, st_out);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
}

int aios_posix_create(aios_posix_fs* fs, uint64_t parent, const char* name, uint32_t mode,
                      aios_posix_stat* st_out) {
  if (!fs || !name || !*name || std::strchr(name, '/')) return -EINVAL;
  try {
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (!pmeta.exists) return -ENOENT;
    if (!S_ISDIR(pmeta.mode)) return -ENOTDIR;
    aios::posix::DirTable dir(fs->st->session, fs->st->volume, parent);
    dir.load();
    if (dir.entries().count(name)) return -EEXIST;
    const uint64_t ino = aios::posix::alloc_ino(*fs->st);
    const uint64_t ts = aios::posix::now_ns();
    aios::posix::InodeMeta m;
    m.ino = ino;
    m.mode = S_IFREG | (mode & 0777);
    m.nlink = 1;
    m.uid = fs->st->default_uid;
    m.gid = fs->st->default_gid;
    m.atime_ns = m.mtime_ns = m.ctime_ns = ts;
    m.stripe_unit = fs->st->stripe_unit;
    m.stripe_width = fs->st->stripe_width;
    aios::posix::store_inode(*fs->st, m);
    dir.link(name, ino);
    pmeta.mtime_ns = pmeta.ctime_ns = ts;
    aios::posix::store_inode(*fs->st, pmeta);
    if (st_out) aios::posix::fill_stat(m, st_out);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
}

int aios_posix_unlink(aios_posix_fs* fs, uint64_t parent, const char* name) {
  if (!fs || !name) return -EINVAL;
  try {
    aios::posix::DirTable dir(fs->st->session, fs->st->volume, parent);
    dir.load();
    auto it = dir.entries().find(name);
    if (it == dir.entries().end()) return -ENOENT;
    const uint64_t ino = it->second;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (m.exists && S_ISDIR(m.mode)) return -EISDIR;
    dir.unlink(name);
    if (m.exists) {
      if (m.nlink > 1) {
        m.nlink -= 1;
        m.ctime_ns = aios::posix::now_ns();
        aios::posix::store_inode(*fs->st, m);
      } else {
        aios::posix::truncate_file(*fs->st, ino, 0);
        fs->st->session.delete_object(aios::posix::ino_oid(fs->st->volume, ino));
        std::lock_guard lock(fs->st->mu);
        fs->st->inode_cache.erase(ino);
      }
    }
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
}

int aios_posix_rmdir(aios_posix_fs* fs, uint64_t parent, const char* name) {
  if (!fs || !name) return -EINVAL;
  try {
    aios::posix::DirTable dir(fs->st->session, fs->st->volume, parent);
    dir.load();
    auto it = dir.entries().find(name);
    if (it == dir.entries().end()) return -ENOENT;
    const uint64_t ino = it->second;
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists || !S_ISDIR(m.mode)) return -ENOTDIR;
    aios::posix::DirTable child(fs->st->session, fs->st->volume, ino);
    child.load();
    if (!child.entries().empty()) return -ENOTEMPTY;
    dir.unlink(name);
    fs->st->session.delete_object(aios::posix::ino_oid(fs->st->volume, ino));
    auto pmeta = aios::posix::load_inode(*fs->st, parent);
    if (pmeta.exists && pmeta.nlink > 2) {
      pmeta.nlink -= 1;
      pmeta.mtime_ns = pmeta.ctime_ns = aios::posix::now_ns();
      aios::posix::store_inode(*fs->st, pmeta);
    }
    std::lock_guard lock(fs->st->mu);
    fs->st->inode_cache.erase(ino);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
}

int aios_posix_rename(aios_posix_fs* fs, uint64_t old_parent, const char* old_name,
                      uint64_t new_parent, const char* new_name) {
  if (!fs || !old_name || !new_name) return -EINVAL;
  try {
    if (old_parent == new_parent) {
      aios::posix::DirTable dir(fs->st->session, fs->st->volume, old_parent);
      dir.load();
      if (!dir.entries().count(old_name)) return -ENOENT;
      if (dir.entries().count(new_name) && std::strcmp(old_name, new_name) != 0) {
        // Replace: unlink target first (file only).
        auto tit = dir.entries().find(new_name);
        auto tm = aios::posix::load_inode(*fs->st, tit->second);
        if (tm.exists && S_ISDIR(tm.mode)) return -EISDIR;
        dir.unlink(new_name);
      }
      dir.rename_same(old_name, new_name);
      return 0;
    }
    // Cross-directory: best-effort non-atomic.
    aios::posix::DirTable old_dir(fs->st->session, fs->st->volume, old_parent);
    old_dir.load();
    auto it = old_dir.entries().find(old_name);
    if (it == old_dir.entries().end()) return -ENOENT;
    const uint64_t ino = it->second;
    aios::posix::DirTable new_dir(fs->st->session, fs->st->volume, new_parent);
    new_dir.load();
    if (new_dir.entries().count(new_name)) {
      auto tm = aios::posix::load_inode(*fs->st, new_dir.entries().at(new_name));
      if (tm.exists && S_ISDIR(tm.mode)) return -EISDIR;
      new_dir.unlink(new_name);
    }
    new_dir.link(new_name, ino);
    old_dir.unlink(old_name);
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
}

int aios_posix_read(aios_posix_fs* fs, uint64_t ino, uint64_t offset, void* buf, size_t len,
                    size_t* out_len) {
  if (!fs || !buf) return -EINVAL;
  return aios::posix::read_file(*fs->st, ino, offset, buf, len, out_len);
}

int aios_posix_write(aios_posix_fs* fs, uint64_t ino, uint64_t offset, const void* buf,
                     size_t len, size_t* out_len) {
  if (!fs || !buf) return -EINVAL;
  return aios::posix::write_file(*fs->st, ino, offset, buf, len, out_len);
}

int aios_posix_truncate(aios_posix_fs* fs, uint64_t ino, uint64_t size) {
  if (!fs) return -EINVAL;
  return aios::posix::truncate_file(*fs->st, ino, size);
}

int aios_posix_setattr(aios_posix_fs* fs, uint64_t ino, const aios_posix_stat* st,
                       uint32_t to_set) {
  if (!fs || !st) return -EINVAL;
  try {
    auto m = aios::posix::load_inode(*fs->st, ino);
    if (!m.exists) return -ENOENT;
    if (to_set & AIOS_POSIX_SET_MODE) m.mode = (m.mode & S_IFMT) | (st->mode & 07777);
    if (to_set & AIOS_POSIX_SET_UID) m.uid = st->uid;
    if (to_set & AIOS_POSIX_SET_GID) m.gid = st->gid;
    if (to_set & AIOS_POSIX_SET_MTIME) m.mtime_ns = st->mtime_ns;
    if (to_set & AIOS_POSIX_SET_ATIME) m.atime_ns = st->atime_ns;
    m.ctime_ns = aios::posix::now_ns();
    aios::posix::store_inode(*fs->st, m);
    if (to_set & AIOS_POSIX_SET_SIZE) {
      int rc = aios::posix::truncate_file(*fs->st, ino, st->size);
      if (rc) return rc;
    }
    return 0;
  } catch (const aios::client_error& e) {
    return aios::posix::map_error(e);
  }
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

}  // extern "C"
