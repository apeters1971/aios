#pragma once

#include "client/put_layout.hpp"
#include "client/session.hpp"
#include "config.hpp"
#include "posix/aios_posix.h"
#include "posix/qos_controller.hpp"
#include "posix/quota_ledger.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
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

// Directory name → ino map loaded from changelog.
class DirTable {
 public:
  explicit DirTable(Session& session, std::string vol, uint64_t ino);

  void load();
  const std::unordered_map<std::string, uint64_t>& entries() const { return entries_; }

  void link(const std::string& name, uint64_t child);
  // Lock the directory tip, reload, and link only if `name` is absent.
  // Returns false when the name is already present (caller should orphan `child`).
  bool link_if_absent(const std::string& name, uint64_t child);
  void unlink(const std::string& name);
  void rename_same(const std::string& old_name, const std::string& new_name);
  void compact_if_needed();
  void set_put_layout(PutLayout layout) { put_layout_ = std::move(layout); }

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
};

struct FsState {
  Session session;
  std::string volume;
  uint64_t stripe_unit{kDefaultStripeUnit};
  uint32_t stripe_width{kDefaultStripeWidth};
  uint32_t default_uid{0};
  uint32_t default_gid{0};
  std::string frontend_label{"fs"};  // s3 | fs | custom (for IO monitoring)
  std::mutex mu;                     // guards super, inode_cache, flock_tokens, rstat_dirty
  SuperMeta super;
  std::unordered_map<uint64_t, InodeMeta> inode_cache;
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

  explicit FsState(SessionConfig cfg)
      : session(std::move(cfg)) {}

  ~FsState() {
    rstat_stop.store(true);
    if (rstat_thread.joinable()) rstat_thread.join();
  }

  FsState(const FsState&) = delete;
  FsState& operator=(const FsState&) = delete;
};

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
