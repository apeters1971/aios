#include "posix/posix_fsck.hpp"

#include "posix/posix_internal.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <charconv>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace aios::posix {
namespace {

using Kind = FsckFinding::Kind;

struct Ctx {
  Session& s;
  const std::string vol;
  const FsckOptions& opt;
  FsckReport rep;
  std::int64_t now_ms{0};

  void log(const std::string& line) const {
    if (opt.log) opt.log(line);
  }

  bool young(std::int64_t mtime_ms) const {
    if (mtime_ms <= 0) return false;
    return now_ms - mtime_ms < std::chrono::duration_cast<std::chrono::milliseconds>(opt.min_age).count();
  }
  bool young_ns(std::uint64_t t_ns) const { return young(static_cast<std::int64_t>(t_ns / 1000000)); }

  FsckFinding& add(Kind k, std::string subject, std::string detail, bool repairable = true) {
    rep.findings.push_back(FsckFinding{k, std::move(subject), std::move(detail), repairable});
    auto& f = rep.findings.back();
    log(std::string(fsck_kind_name(k)) + ": " + f.subject + (f.detail.empty() ? "" : " (" + f.detail + ")"));
    return f;
  }

  // Run a repair action for a finding, honouring --repair and the age guard.
  template <class Fn>
  void fix(FsckFinding& f, bool is_young, Fn&& fn) {
    if (!f.repairable) return;
    if (is_young) {
      f.skipped_young = true;
      log("  not repaired: object younger than min_age");
      return;
    }
    if (!opt.repair) return;
    try {
      fn();
      f.repaired = true;
      ++rep.repaired;
      log("  repaired");
    } catch (const std::exception& e) {
      log(std::string("  repair failed: ") + e.what());
    }
  }

  std::optional<InodeMeta> get_inode(std::uint64_t ino, std::int64_t* mtime_ms_out = nullptr) {
    auto snap = s.get_object(ino_oid(vol, ino));
    if (!snap.exists) return std::nullopt;
    try {
      auto m = inode_from_json(snap.body, cas_from_attrs(snap.attrs));
      if (mtime_ms_out) *mtime_ms_out = static_cast<std::int64_t>(m.ctime_ns / 1000000);
      return m;
    } catch (...) {
      return std::nullopt;
    }
  }

  void store_inode(InodeMeta& m) {
    m.cas = s.put_bytes(ino_oid(vol, m.ino), inode_to_json(m), {}, m.cas);
  }

  // Every object under prefix, paged.
  template <class Fn>
  void for_each_object(const std::string& prefix, Fn&& fn) {
    std::string cursor;
    for (;;) {
      auto page = s.list_prefix(prefix, 1000, cursor);
      for (const auto& o : page.objects) fn(o);
      if (page.next_cursor.empty()) break;
      cursor = page.next_cursor;
    }
  }

  void delete_quiet(const std::string& oid) {
    try {
      s.delete_object(oid);
    } catch (const client_error& e) {
      if (e.code() != "not_found") throw;
    }
  }

  void delete_chunks(std::uint64_t ino) {
    for_each_object("posix/" + vol + "/data/" + std::to_string(ino) + "/c/",
                    [&](const ListObject& o) { delete_quiet(o.oid); });
  }
  void delete_dir_objects(std::uint64_t ino) {
    delete_quiet(dir_meta_oid(vol, ino));
    delete_quiet(dir_log_oid(vol, ino));
    delete_quiet(dir_snap_oid(vol, ino));
  }
};

bool parse_u64(std::string_view sv, std::uint64_t& out) {
  if (sv.empty()) return false;
  auto r = std::from_chars(sv.data(), sv.data() + sv.size(), out);
  return r.ec == std::errc{} && r.ptr == sv.data() + sv.size();
}

struct Reached {
  InodeMeta meta;
  std::string path;          // first path the inode was reached under
  std::uint32_t names{0};    // dentries pointing at it
  std::uint32_t subdirs{0};  // for directories
};

}  // namespace

const char* fsck_kind_name(FsckFinding::Kind k) {
  switch (k) {
    case Kind::NoSuperblock: return "no-superblock";
    case Kind::NoRoot: return "no-root";
    case Kind::DanglingDentry: return "dangling-dentry";
    case Kind::DirLinkedTwice: return "dir-linked-twice";
    case Kind::ParentMismatch: return "parent-mismatch";
    case Kind::NlinkMismatch: return "nlink-mismatch";
    case Kind::OrphanInode: return "orphan-inode";
    case Kind::OrphanChunk: return "orphan-chunk";
    case Kind::StrayChunk: return "stray-chunk";
    case Kind::StaleDirObjects: return "stale-dir-objects";
    case Kind::LogGarbage: return "log-garbage";
    case Kind::NextInoBehind: return "next-ino-behind";
  }
  return "?";
}

std::size_t FsckReport::unrepaired() const {
  return static_cast<std::size_t>(
      std::count_if(findings.begin(), findings.end(), [](const FsckFinding& f) { return !f.repaired; }));
}

FsckReport fsck_volume(Session& session, const std::string& volume, const FsckOptions& opt) {
  Ctx c{session, volume, opt, {}, static_cast<std::int64_t>(now_ns() / 1000000)};
  const std::string base = "posix/" + volume + "/";

  // --- superblock -----------------------------------------------------------
  SuperMeta super;
  {
    auto snap = session.get_object(super_oid(volume));
    if (!snap.exists) {
      c.add(Kind::NoSuperblock, super_oid(volume), "", false);
      c.rep.fatal = true;
      return c.rep;
    }
    try {
      super = super_from_json(snap.body, cas_from_attrs(snap.attrs));
    } catch (const std::exception& e) {
      c.add(Kind::NoSuperblock, super_oid(volume), std::string("unparsable: ") + e.what(), false);
      c.rep.fatal = true;
      return c.rep;
    }
  }
  const std::uint64_t default_unit = super.stripe_unit ? super.stripe_unit : kDefaultStripeUnit;

  // --- tree walk ------------------------------------------------------------
  std::unordered_map<std::uint64_t, Reached> reached;
  std::uint64_t max_ino = 1;
  {
    auto root = c.get_inode(1);
    if (!root || !S_ISDIR(root->mode)) {
      c.add(Kind::NoRoot, ino_oid(volume, 1), root ? "not a directory" : "missing", false);
      c.rep.fatal = true;
      return c.rep;
    }
    reached[1] = Reached{*root, "/", 1, 0};
  }
  std::deque<std::uint64_t> todo{1};
  while (!todo.empty()) {
    const std::uint64_t dino = todo.front();
    todo.pop_front();
    Reached& dir = reached[dino];
    const std::string dpath = dir.path == "/" ? "" : dir.path;
    ++c.rep.dirs;

    DirTable table(session, volume, dino, nullptr);
    try {
      table.load(false);
    } catch (const std::exception& e) {
      c.add(Kind::LogGarbage, dir_meta_oid(volume, dino), std::string("directory unreadable: ") + e.what(),
            false);
      continue;
    }
    // Log hygiene: bytes past the committed log_bytes are a refused append's
    // leftovers; the next writer compacts, we only report.
    {
      auto lh = session.head_object(dir_log_oid(volume, dino));
      if (lh.exists && lh.size > table.log_bytes()) {
        c.add(Kind::LogGarbage, dir_log_oid(volume, dino),
              std::to_string(lh.size - table.log_bytes()) + " bytes past committed log_bytes", false);
      }
    }

    // Deterministic order for reproducible reports.
    std::map<std::string, std::uint64_t> names(table.entries().begin(), table.entries().end());
    for (const auto& [name, ino] : names) {
      const std::string path = dpath + "/" + name;
      max_ino = std::max(max_ino, ino);
      auto it = reached.find(ino);
      if (it != reached.end()) {
        if (S_ISDIR(it->second.meta.mode)) {
          c.add(Kind::DirLinkedTwice, path, "also reachable as " + it->second.path, false);
          continue;
        }
        ++it->second.names;
        continue;
      }
      std::int64_t ctime_ms = 0;
      auto m = c.get_inode(ino, &ctime_ms);
      if (!m) {
        auto& f = c.add(Kind::DanglingDentry, path, "inode " + std::to_string(ino) + " missing");
        // A create under a lease publishes the inode before the name, so a
        // missing inode behind a *fresh* dentry is not necessarily damage.
        // The dentry itself has no timestamp; use the parent's mtime.
        c.fix(f, c.young_ns(dir.meta.mtime_ns), [&] {
          DirTable t(session, volume, dino, nullptr);
          t.load(false);
          const int rc = t.unlink_if(name, ino);
          if (rc != 0 && rc != -ENOENT) throw std::runtime_error("unlink_if rc=" + std::to_string(rc));
        });
        continue;
      }
      Reached r{*m, path, 1, 0};
      if (S_ISDIR(m->mode)) {
        ++dir.subdirs;
        if (m->parent_ino != dino) {
          auto& f = c.add(Kind::ParentMismatch, path,
                          "parent_ino=" + std::to_string(m->parent_ino) + " expected " + std::to_string(dino));
          c.fix(f, c.young_ns(m->ctime_ns), [&] {
            InodeMeta fixed = *m;
            fixed.parent_ino = dino;
            c.store_inode(fixed);
            r.meta = fixed;
          });
        }
        todo.push_back(ino);
      } else if (S_ISLNK(m->mode)) {
        ++c.rep.symlinks;
      } else {
        ++c.rep.files;
        c.rep.bytes += m->size;
      }
      reached.emplace(ino, std::move(r));
    }
  }

  // --- nlink ----------------------------------------------------------------
  for (auto& [ino, r] : reached) {
    const std::uint32_t want = S_ISDIR(r.meta.mode) ? 2 + r.subdirs : r.names;
    if (r.meta.nlink == want) continue;
    auto& f = c.add(Kind::NlinkMismatch, r.path,
                    "nlink=" + std::to_string(r.meta.nlink) + " expected " + std::to_string(want));
    // A hard link / unlink in flight updates dentry and nlink in two steps.
    c.fix(f, c.young_ns(r.meta.ctime_ns), [&] {
      InodeMeta fixed = r.meta;
      fixed.nlink = want;
      c.store_inode(fixed);
      r.meta = fixed;
    });
  }

  // --- inode objects not reachable from the root -----------------------------
  std::unordered_set<std::uint64_t> existing;  // every inode object present
  std::vector<std::pair<std::uint64_t, ListObject>> orphans;
  c.for_each_object(base + "ino/", [&](const ListObject& o) {
    std::uint64_t ino = 0;
    if (!parse_u64(std::string_view(o.oid).substr(base.size() + 4), ino)) return;
    ++c.rep.inode_objects;
    existing.insert(ino);
    max_ino = std::max(max_ino, ino);
    if (!reached.count(ino)) orphans.emplace_back(ino, o);
  });
  std::sort(orphans.begin(), orphans.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [ino, o] : orphans) {
    auto m = c.get_inode(ino);
    std::string what = m ? (S_ISDIR(m->mode) ? "directory" : S_ISLNK(m->mode) ? "symlink" : "file, " +
                                                                                                  std::to_string(m->size) + " bytes")
                         : "unparsable";
    auto& f = c.add(Kind::OrphanInode, o.oid, what + ", nlink=" + std::to_string(m ? m->nlink : 0));
    // Unpublished creates under a lease and renames publish inode first.
    const bool is_young = c.young(o.mtime_ms) || (m && c.young_ns(m->ctime_ns));
    c.fix(f, is_young, [&] {
      c.delete_chunks(ino);
      if (m && S_ISDIR(m->mode)) c.delete_dir_objects(ino);
      c.delete_quiet(o.oid);
    });
  }

  // --- data chunks ----------------------------------------------------------
  {
    std::vector<std::pair<std::string, ListObject>> bad;  // detail, object
    c.for_each_object(base + "data/", [&](const ListObject& o) {
      ++c.rep.chunk_objects;
      std::string_view rest = std::string_view(o.oid).substr(base.size() + 5);  // "{ino}/c/{chunk}"
      const auto slash = rest.find("/c/");
      std::uint64_t ino = 0, chunk = 0;
      if (slash == std::string_view::npos || !parse_u64(rest.substr(0, slash), ino) ||
          !parse_u64(rest.substr(slash + 3), chunk)) {
        return;
      }
      if (!existing.count(ino)) {
        bad.emplace_back("orphan", o);
        return;
      }
      auto it = reached.find(ino);
      if (it == reached.end()) return;  // orphan inode: handled (with its chunks) above
      const InodeMeta& m = it->second.meta;
      if (!S_ISREG(m.mode)) {
        bad.emplace_back("chunk of a non-regular inode", o);
        return;
      }
      const std::uint64_t unit = m.stripe_unit ? m.stripe_unit : default_unit;
      const std::uint64_t nchunk = m.size == 0 ? 0 : (m.size + unit - 1) / unit;
      if (chunk >= nchunk) {
        bad.emplace_back("chunk " + std::to_string(chunk) + " past size " + std::to_string(m.size) +
                             " (unit " + std::to_string(unit) + ")",
                         o);
      }
    });
    std::sort(bad.begin(), bad.end(), [](const auto& a, const auto& b) { return a.second.oid < b.second.oid; });
    for (const auto& [detail, o] : bad) {
      auto& f = c.add(detail == "orphan" ? Kind::OrphanChunk : Kind::StrayChunk, o.oid,
                      detail == "orphan" ? "" : detail);
      // A write publishes chunks before it publishes the new size.
      c.fix(f, c.young(o.mtime_ms), [&] { c.delete_quiet(o.oid); });
    }
  }

  // --- directory objects ----------------------------------------------------
  {
    std::map<std::uint64_t, std::vector<ListObject>> by_ino;
    c.for_each_object(base + "dir/", [&](const ListObject& o) {
      std::string_view rest = std::string_view(o.oid).substr(base.size() + 4);
      const auto slash = rest.find('/');
      std::uint64_t ino = 0;
      if (slash == std::string_view::npos || !parse_u64(rest.substr(0, slash), ino)) return;
      by_ino[ino].push_back(o);
    });
    for (const auto& [ino, objs] : by_ino) {
      std::string why;
      if (!existing.count(ino)) {
        why = "inode missing";
      } else if (auto it = reached.find(ino); it != reached.end() && !S_ISDIR(it->second.meta.mode)) {
        why = "inode is not a directory";
      } else {
        continue;
      }
      std::int64_t newest = 0;
      for (const auto& o : objs) newest = std::max(newest, o.mtime_ms);
      auto& f = c.add(Kind::StaleDirObjects, base + "dir/" + std::to_string(ino) + "/", why);
      // mkdir under a lease publishes the child's inode after its dir objects
      // may already exist; rmdir deletes in the other order.
      c.fix(f, c.young(newest), [&] { c.delete_dir_objects(ino); });
    }
  }

  // --- superblock allocator ------------------------------------------------------
  if (super.next_ino <= max_ino) {
    auto& f = c.add(Kind::NextInoBehind, super_oid(volume),
                    "next_ino=" + std::to_string(super.next_ino) + " but inode " + std::to_string(max_ino) +
                        " exists");
    // Mounts reserve inode numbers in batches; the batch may be in use already,
    // so the superblock itself is not the timestamp to trust here. Bump past
    // the highest number seen plus one batch.
    c.fix(f, false, [&] {
      SuperMeta fixed = super;
      fixed.next_ino = max_ino + 1 + kInoBatch;
      session.put_bytes(super_oid(volume), super_to_json(fixed), {}, super.cas);
    });
  }

  return c.rep;
}

}  // namespace aios::posix
