#pragma once

#include "client/session.hpp"
#include "posix/aios_posix.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
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
  uint64_t size{0};
  uint64_t atime_ns{0};
  uint64_t mtime_ns{0};
  uint64_t ctime_ns{0};
  uint64_t stripe_unit{kDefaultStripeUnit};
  uint32_t stripe_width{kDefaultStripeWidth};
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

// Directory name → ino map loaded from changelog.
class DirTable {
 public:
  explicit DirTable(Session& session, std::string vol, uint64_t ino);

  void load();
  const std::unordered_map<std::string, uint64_t>& entries() const { return entries_; }

  void link(const std::string& name, uint64_t child);
  void unlink(const std::string& name);
  void rename_same(const std::string& old_name, const std::string& new_name);
  void compact_if_needed();

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
  SuperMeta super;
  std::mutex mu;
  std::unordered_map<uint64_t, InodeMeta> inode_cache;
  std::unordered_map<uint64_t, std::string> flock_tokens;  // ino → lock token

  explicit FsState(SessionConfig cfg)
      : session(std::move(cfg)) {}
};

InodeMeta load_inode(FsState& st, uint64_t ino);
void store_inode(FsState& st, InodeMeta& m);
uint64_t alloc_ino(FsState& st);
void ensure_super(FsState& st);
void ensure_root(FsState& st);
void drop_nlink(FsState& st, uint64_t ino);
void release_all_flocks(FsState& st);

int read_file(FsState& st, uint64_t ino, uint64_t offset, void* buf, size_t len, size_t* out_len);
int write_file(FsState& st, uint64_t ino, uint64_t offset, const void* buf, size_t len,
               size_t* out_len);
int truncate_file(FsState& st, uint64_t ino, uint64_t size);

}  // namespace posix
}  // namespace aios
