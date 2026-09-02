#pragma once

#include "client/put_layout.hpp"
#include "client/session.hpp"
#include "config.hpp"
#include "posix/aios_posix.h"
#include "posix/qos_controller.hpp"
#include "posix/quota_ledger.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aios {
namespace posix {

inline constexpr uint64_t kRootIno = 1;
inline constexpr uint64_t kDefaultStripeUnit = 1024ull * 1024ull;
inline constexpr uint32_t kDefaultStripeWidth = 4;
inline constexpr const char* kCasAttr = "aios.posix.cas";
inline constexpr size_t kChunkCacheSlots = 8;
inline constexpr auto kDirCacheTtl = std::chrono::milliseconds(250);
inline constexpr size_t kDirCacheMaxEntries = 4096;
// Cached inode records are revalidated (GET + merge) once this old; a pending
// deferred size (dirty_sizes) always wins over the server copy during the merge.
inline constexpr auto kInodeCacheTtl = std::chrono::milliseconds(1000);
inline constexpr size_t kInodeCacheMaxEntries = 65536;
inline constexpr uint64_t kDirtyFlushBytes = 4ull * 1024ull * 1024ull;
inline constexpr auto kDirtyFlushAge = std::chrono::milliseconds(100);
inline constexpr auto kDirtyFlushTick = std::chrono::milliseconds(50);
inline constexpr size_t kMaxSymlinkBytes = 4095;
inline constexpr size_t kChunkLockStripes = 64;
inline constexpr int kChunkWriteRetries = 16;

// Last-N stripe bodies so 128 KiB FUSE I/O does not re-GET the same 1 MiB chunk.
// Bodies are shared immutable buffers so a lookup hands out a pointer instead of
// copying 1 MiB per 128 KiB read.
struct ChunkCache {
  using Body = std::shared_ptr<const std::string>;

  struct Slot {
    uint64_t ino{0};
    uint64_t chunk{std::numeric_limits<uint64_t>::max()};
    uint64_t cas{0};
    uint64_t lru{0};
    Body body;
  };

  Body lookup(uint64_t ino, uint64_t chunk, uint64_t& cas) {
    std::lock_guard lock(mu);
    for (auto& e : slots) {
      if (e.ino == ino && e.chunk == chunk && e.body) {
        e.lru = ++clock;
        cas = e.cas;
        return e.body;
      }
    }
    return nullptr;
  }

  void store(uint64_t ino, uint64_t chunk, std::string body, uint64_t cas) {
    store(ino, chunk, std::make_shared<const std::string>(std::move(body)), cas);
  }

  // A slot already holding the same or a newer version (cas) is kept: a reader
  // that fetched v1 must not clobber v2 stored by a writer that raced it.
  void store(uint64_t ino, uint64_t chunk, Body body, uint64_t cas) {
    if (!body) return;
    std::lock_guard lock(mu);
    Slot* victim = nullptr;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (auto& e : slots) {
      if (e.ino == ino && e.chunk == chunk) {
        if (e.body && e.cas >= cas) {
          e.lru = ++clock;
          return;
        }
        e.body = std::move(body);
        e.cas = cas;
        e.lru = ++clock;
        return;
      }
      if (e.chunk == std::numeric_limits<uint64_t>::max()) {
        victim = &e;
        break;
      }
      if (e.lru < oldest) {
        oldest = e.lru;
        victim = &e;
      }
    }
    if (!victim) victim = &slots[0];
    victim->ino = ino;
    victim->chunk = chunk;
    victim->cas = cas;
    victim->body = std::move(body);
    victim->lru = ++clock;
  }

  void drop(uint64_t ino, std::optional<uint64_t> chunk = std::nullopt) {
    std::lock_guard lock(mu);
    for (auto& e : slots) {
      if (e.ino != ino) continue;
      if (chunk && e.chunk != *chunk) continue;
      e = {};
      e.chunk = std::numeric_limits<uint64_t>::max();
    }
  }

  // Cached version of (ino, chunk) or 0 when absent; for tests and diagnostics.
  uint64_t cached_cas(uint64_t ino, uint64_t chunk) {
    std::lock_guard lock(mu);
    for (const auto& e : slots) {
      if (e.ino == ino && e.chunk == chunk && e.body) return e.cas;
    }
    return 0;
  }

  std::mutex mu;
  uint64_t clock{0};
  std::array<Slot, kChunkCacheSlots> slots{};
};

struct InodeMeta {
  uint64_t ino{0};
  uint32_t mode{0};
  uint32_t nlink{0};
  uint32_t uid{0};
  uint32_t gid{0};
  uint32_t project_id{0};  // 0 = volume domain; inherited from parent on create
  uint64_t parent_ino{0};  // primary parent directory; 0 for root
  uint64_t size{0};
  uint64_t atime_ns{0};
  uint64_t mtime_ns{0};
  uint64_t ctime_ns{0};
  uint64_t stripe_unit{kDefaultStripeUnit};
  uint32_t stripe_width{kDefaultStripeWidth};
  // Lazy recursive accounting (directories); recomputed on flush.
  uint64_t rbytes{0};
  uint64_t rfiles{0};
  uint64_t rdirs{0};
  uint64_t rtime_ns{0};
  uint64_t cas{0};
  bool exists{false};
  std::unordered_map<std::string, std::string> xattrs;  // name → raw bytes
  std::string symlink;                                  // target when S_IFLNK
};

struct InodeCacheEnt {
  InodeMeta meta;
  std::chrono::steady_clock::time_point loaded{};
  uint64_t lru{0};
};

struct DirCacheEnt {
  std::unordered_map<std::string, uint64_t> entries;
  uint64_t meta_cas{0};
  uint64_t next_op{1};
  uint64_t log_bytes{0};
  uint64_t snapshot_op{0};
  std::chrono::steady_clock::time_point loaded{};
};

struct DirtySize {
  uint64_t size{0};
  uint64_t mtime_ns{0};
  uint64_t ctime_ns{0};
  uint64_t dirty_bytes{0};
  std::chrono::steady_clock::time_point since{};
};

struct SuperMeta {
  uint64_t next_ino{2};
  uint64_t stripe_unit{kDefaultStripeUnit};
  uint32_t stripe_width{kDefaultStripeWidth};
  std::string uuid;
  uint64_t cas{0};
  bool exists{false};
  bool frozen{false};  // volume snapshot/backup quiesce
};

std::string super_oid(const std::string& vol);
std::string ino_oid(const std::string& vol, uint64_t ino);
std::string dir_meta_oid(const std::string& vol, uint64_t ino);
std::string dir_log_oid(const std::string& vol, uint64_t ino);
std::string dir_snap_oid(const std::string& vol, uint64_t ino);
std::string chunk_oid(const std::string& vol, uint64_t ino, uint64_t chunk);

uint64_t now_ns();
int map_error(const client_error& e);

InodeMeta inode_from_json(const std::string& body, uint64_t cas_hint);
std::string inode_to_json(const InodeMeta& m);
SuperMeta super_from_json(const std::string& body, uint64_t cas_hint);
std::string super_to_json(const SuperMeta& m);

void fill_stat(const InodeMeta& m, aios_posix_stat* st);

// want: bitmask using S_IROTH=4, S_IWOTH=2, S_IXOTH=1 (same as low triad).
int check_access(const aios_posix_cred& cred, const InodeMeta& m, int want);
// Sticky-bit unlink/rename replace: 0 or -EACCES.
int check_sticky_unlink(const aios_posix_cred& cred, const InodeMeta& parent,
                        const InodeMeta& victim);

struct FsState;

// Directory name → ino map loaded from changelog.
class DirTable {
 public:
  explicit DirTable(Session& session, std::string vol, uint64_t ino, FsState* cache = nullptr);

  void load(bool allow_cache = true);
  const std::unordered_map<std::string, uint64_t>& entries() const { return entries_; }

  void link(const std::string& name, uint64_t child);
  // Lock the directory tip, reload, and link only if `name` is absent.
  // Returns false when the name is already present (caller should orphan `child`).
  bool link_if_absent(const std::string& name, uint64_t child);
  void unlink(const std::string& name);
  // Lock the directory tip (plus extra_locks, e.g. a child directory's tip), reload,
  // and remove `name` only while it still maps to expected_ino. `guard` runs under
  // the locks after the reload and may veto with -errno (rmdir emptiness check).
  // Returns 0, -ENOENT when the dentry changed underneath the caller, or guard's rc.
  int unlink_if(const std::string& name, uint64_t expected_ino,
                std::vector<std::string> extra_locks = {},
                const std::function<int()>& guard = nullptr);
  void rename_same(const std::string& old_name, const std::string& new_name);
  void compact_if_needed();
  void set_put_layout(PutLayout layout) { put_layout_ = std::move(layout); }
  void publish() { publish_cache(); }

  // Mutable entry map for planning a transactional compact rewrite.
  std::unordered_map<std::string, uint64_t>& mutable_entries() { return entries_; }
  uint64_t meta_cas() const { return meta_cas_; }
  const std::string& meta_oid() const { return meta_oid_; }
  const std::string& log_oid() const { return log_oid_; }
  const std::string& snap_oid() const { return snap_oid_; }

  // Bodies for a compacted directory tip (snapshot holds full map, empty log).
  void plan_compact_bodies(std::string& meta_out, std::string& snap_out,
                           std::string& log_out) const;

 private:
  Session& session_;
  FsState* cache_{nullptr};
  std::string vol_;
  uint64_t ino_;
  std::string meta_oid_;
  std::string log_oid_;
  std::string snap_oid_;
  std::unordered_map<std::string, uint64_t> entries_;
  uint64_t next_op_{1};
  uint64_t log_bytes_{0};
  uint64_t snapshot_op_{0};
  uint64_t meta_cas_{0};
  PutLayout put_layout_{};

  void store_meta();
  void apply_record(uint64_t op_id, uint32_t op, const std::vector<std::string>& args);
  void append_ops(const std::vector<std::pair<uint32_t, std::vector<std::string>>>& ops);
  void publish_cache();
};

struct FsState {
  Session session;
  std::string volume;
  uint64_t stripe_unit{kDefaultStripeUnit};
  uint32_t stripe_width{kDefaultStripeWidth};
  uint32_t default_uid{0};
  uint32_t default_gid{0};
  std::string frontend_label{"fs"};  // s3 | fs | custom (for IO monitoring)
  std::mutex mu;  // super, inode_cache, flock_tokens, rstat_dirty, dir_cache, dirty_sizes
  SuperMeta super;
  std::chrono::steady_clock::time_point super_loaded{};
  std::unordered_map<uint64_t, InodeCacheEnt> inode_cache;
  uint64_t inode_cache_clock{0};
  std::unordered_map<uint64_t, std::string> flock_tokens;  // ino → lock token
  std::unordered_set<uint64_t> rstat_dirty;
  int rstat_interval_ms{60000};
  std::atomic<bool> rstat_stop{false};
  std::thread rstat_thread;
  // Layout rules are replaced wholesale on refresh; readers take a snapshot of the
  // shared_ptr so an in-flight lookup keeps the version it started with.
  std::mutex layout_mu;
  std::shared_ptr<const std::vector<PosixLayoutRule>> layout_rules;
  std::chrono::steady_clock::time_point layout_rules_loaded{};
  std::unique_ptr<QuotaLedger> quota;
  std::unique_ptr<QosController> qos;
  ChunkCache chunk_cache;
  std::unordered_map<uint64_t, DirCacheEnt> dir_cache;
  std::unordered_map<uint64_t, DirtySize> dirty_sizes;
  // Serializes in-process read-modify-write of one (ino, chunk) so only cross-client
  // conflicts reach the server's CAS check.
  std::array<std::mutex, kChunkLockStripes> chunk_locks;

  std::mutex& chunk_lock(uint64_t ino, uint64_t chunk) {
    const uint64_t h = (ino * 0x9E3779B97F4A7C15ull) ^ (chunk + 0x7F4A7C15ull);
    return chunk_locks[static_cast<size_t>(h % kChunkLockStripes)];
  }

  explicit FsState(SessionConfig cfg)
      : session(std::move(cfg)) {}

  ~FsState() {
    rstat_stop.store(true);
    if (rstat_thread.joinable()) rstat_thread.join();
  }

  FsState(const FsState&) = delete;
  FsState& operator=(const FsState&) = delete;
};

inline DirTable make_dir(FsState& st, uint64_t ino) {
  return DirTable(st.session, st.volume, ino, &st);
}

void flush_dirty_inode(FsState& st, uint64_t ino);
// min_age: only entries dirty at least this long (nullopt = all).
void flush_all_dirty_inodes(FsState& st,
                            std::optional<std::chrono::steady_clock::duration> min_age = std::nullopt);

// Inode cache maintenance; all require st.mu held by the caller.
// cache_inode_locked replaces the record wholesale (fresh server copy or a PUT we
// just made). cache_touch_size_locked merges a larger size/newer times without
// disturbing other fields, which is what concurrent writers must use.
void cache_inode_locked(FsState& st, const InodeMeta& m);
void cache_erase_locked(FsState& st, uint64_t ino);
void dir_cache_evict_locked(FsState& st);

// Cross-directory rename via /txn (compact rewrite of both dir tips under locks).
int rename_cross_dir(FsState& st, uint64_t old_parent, const std::string& old_name,
                     uint64_t new_parent, const std::string& new_name);

// Same-directory rename/replace via /txn (compact rewrite under locks).
int rename_same_dir(FsState& st, uint64_t parent, const std::string& old_name,
                    const std::string& new_name);

InodeMeta load_inode(FsState& st, uint64_t ino);

// Restates this operation's own mutation against a record freshly loaded from the
// server. Required for any store_inode that read-modify-writes an existing inode:
// without it a CAS conflict can only be answered by failing, since re-sending the
// caller's stale copy would discard the other writer's fields.
using InodeReapply = std::function<void(InodeMeta&)>;

// path_for_layout: use when the dentry is not linked yet (create/mkdir).
void store_inode(FsState& st, InodeMeta& m,
                 const std::optional<std::string>& path_for_layout = std::nullopt,
                 const InodeReapply& reapply = nullptr);
uint64_t alloc_ino(FsState& st);
void ensure_super(FsState& st);
void ensure_root(FsState& st);
void drop_nlink(FsState& st, uint64_t ino);
void release_all_flocks(FsState& st);

int read_file(FsState& st, uint64_t ino, uint64_t offset, void* buf, size_t len, size_t* out_len);
int write_file(FsState& st, uint64_t ino, uint64_t offset, const void* buf, size_t len,
               size_t* out_len);
int truncate_file(FsState& st, uint64_t ino, uint64_t size);

void mark_rstat_dirty(FsState& st, uint64_t dir_ino);
void flush_rstats(FsState& st);
void start_rstat_thread(FsState& st);
void stop_rstat_thread(FsState& st);

inline constexpr const char* kRstatXattrRbytes = "aios.rbytes";
inline constexpr const char* kRstatXattrRfiles = "aios.rfiles";
inline constexpr const char* kRstatXattrRdirs = "aios.rdirs";
inline constexpr const char* kRstatXattrRtime = "aios.rtime";

bool is_rstat_xattr(const char* name);
// Returns length written/needed, or -errno. For directories only.
int get_rstat_xattr(const InodeMeta& m, const char* name, void* value, size_t size);

}  // namespace posix
}  // namespace aios
