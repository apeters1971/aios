#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3;

namespace aios {

struct ObjectStoreOptions {
  std::uint32_t shard_count{256};
  std::size_t inline_max_bytes{64 * 1024};
  std::string force_mode{"auto"};  // "auto" | "inline" | "fs"
  // Retain newest N versions per oid after publish (2B).
  int max_versions{16};
  // If true, FS COW requires reflink/clone; if false, allow full-copy fallback.
  bool clone_required{true};
};

struct ObjectInfo {
  std::string oid;
  std::uint32_t shard{0};
  std::uint64_t seq{0};  // version id (tip unless queried)
  std::uint64_t size{0};
  bool inline_body{false};
  std::string fs_path;
  std::int64_t ctime_ms{0};
  std::int64_t mtime_ms{0};  // same as version ctime for immutable versions
  std::uint32_t crc32c{0};
  bool crc32c_known{false};
  bool is_delete{false};
  // If non-empty, this version is a redirect to another oid (no body).
  std::string redirect_oid;
};

struct VersionInfo {
  std::uint64_t seq{0};
  std::uint64_t size{0};
  std::uint32_t crc32c{0};
  bool crc32c_known{false};
  bool is_delete{false};
  std::int64_t ctime_ms{0};
  bool inline_body{false};
  std::string redirect_oid;
};

struct ObjectListEntry {
  std::string oid;
  std::uint64_t seq{0};
  std::uint64_t size{0};
  std::int64_t mtime_ms{0};
  std::uint32_t crc32c{0};
  bool crc32c_known{false};
  bool is_delete{false};
  std::string redirect_oid;
  std::unordered_map<std::string, std::string> attrs;
};

struct ObjectListResult {
  std::vector<ObjectListEntry> objects;
  std::string next_cursor;
};

enum class PrecondResult { Ok, NotFound, Conflict };

struct AttrPrecondition {
  enum class Kind { Eq, Ne, Absent, Present, MustExist, MustNotExist };
  Kind kind{Kind::Eq};
  std::string key;
  std::string value;
};

// Prepared unpublished version (tip unchanged until publish_tip).
struct PreparedVersion {
  std::string oid;
  std::uint64_t seq{0};
  std::uint64_t prev_tip{0};
  std::uint64_t size{0};
  std::uint32_t crc32c{0};
  bool inline_body{false};
  std::string fs_path;
  bool is_delete{false};
  std::string redirect_oid;
};

std::uint32_t shard_of_oid(const std::string& oid, std::uint32_t shard_count);

class ObjectStore {
 public:
  ObjectStore() = default;
  ~ObjectStore();

  ObjectStore(const ObjectStore&) = delete;
  ObjectStore& operator=(const ObjectStore&) = delete;

  bool open(const std::string& aios_root, ObjectStoreOptions opts, std::string& err);
  void close();

  bool is_open() const { return !shards_.empty(); }
  const std::string& root() const { return root_; }
  const ObjectStoreOptions& options() const { return opts_; }
  std::uint32_t shard_count() const { return opts_.shard_count; }

  // Convenience: prepare + publish + trim. Returns new seq via out_seq if non-null.
  bool put(const std::string& oid, const std::uint8_t* data, std::size_t len,
           const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
           std::string& err) {
    return put(oid, data, len, attrs, replace_attrs, std::nullopt, nullptr, err);
  }
  bool put(const std::string& oid, const std::string& data,
           const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
           std::string& err) {
    return put(oid, reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), attrs,
               replace_attrs, std::nullopt, nullptr, err);
  }
  bool put(const std::string& oid, const std::uint8_t* data, std::size_t len,
           const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
           std::optional<std::uint32_t> expected_crc32c, std::string& err) {
    return put(oid, data, len, attrs, replace_attrs, expected_crc32c, nullptr, err);
  }
  bool put(const std::string& oid, const std::uint8_t* data, std::size_t len,
           const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
           std::optional<std::uint32_t> expected_crc32c, std::uint64_t* out_seq,
           std::string& err);

  bool put_range(const std::string& oid, std::uint64_t offset, const std::uint8_t* data,
                 std::size_t len, const std::unordered_map<std::string, std::string>& attrs,
                 bool replace_attrs, std::string& err) {
    return put_range(oid, offset, data, len, attrs, replace_attrs, nullptr, err);
  }
  bool put_range(const std::string& oid, std::uint64_t offset, const std::uint8_t* data,
                 std::size_t len, const std::unordered_map<std::string, std::string>& attrs,
                 bool replace_attrs, std::uint64_t* out_seq, std::string& err);

  // Transactional API for replication (publish-after-quorum).
  bool prepare_put(const std::string& oid, const std::uint8_t* data, std::size_t len,
                   const std::unordered_map<std::string, std::string>& attrs,
                   bool replace_attrs, std::optional<std::uint32_t> expected_crc32c,
                   PreparedVersion& out, std::string& err);
  bool prepare_put_range(const std::string& oid, std::uint64_t offset,
                         const std::uint8_t* data, std::size_t len,
                         const std::unordered_map<std::string, std::string>& attrs,
                         bool replace_attrs, PreparedVersion& out, std::string& err);
  bool prepare_delete(const std::string& oid, PreparedVersion& out, std::string& err);
  // Create a redirect version (empty body) pointing at target_oid.
  bool prepare_redirect(const std::string& oid, const std::string& target_oid,
                        const std::unordered_map<std::string, std::string>& attrs,
                        bool replace_attrs, PreparedVersion& out, std::string& err);
  bool put_redirect(const std::string& oid, const std::string& target_oid,
                    const std::unordered_map<std::string, std::string>& attrs,
                    bool replace_attrs, std::uint64_t* out_seq, std::string& err);
  // Install a replica-prepared version at exact seq (tip not published).
  bool install_version(const PreparedVersion& v, const std::uint8_t* data, std::size_t len,
                       const std::unordered_map<std::string, std::string>& attrs,
                       std::string& err);
  bool publish_tip(const std::string& oid, std::uint64_t seq, std::string& err);
  bool abort_version(const std::string& oid, std::uint64_t seq, std::string& err);

  bool recompute_crc32c(const std::string& oid, std::uint32_t& out_crc, std::string& err);

  std::optional<std::vector<std::uint8_t>> get(const std::string& oid, std::string& err) {
    return get(oid, std::nullopt, err);
  }
  std::optional<std::vector<std::uint8_t>> get(const std::string& oid,
                                               std::optional<std::uint64_t> seq,
                                               std::string& err);

  std::optional<std::vector<std::uint8_t>> get_range(const std::string& oid,
                                                     std::uint64_t offset, std::size_t len,
                                                     std::string& err) {
    return get_range(oid, std::nullopt, offset, len, err);
  }
  std::optional<std::vector<std::uint8_t>> get_range(const std::string& oid,
                                                     std::optional<std::uint64_t> seq,
                                                     std::uint64_t offset, std::size_t len,
                                                     std::string& err);

  std::optional<std::string> fs_body_path(const std::string& oid, std::string& err) {
    return fs_body_path(oid, std::nullopt, err);
  }
  std::optional<std::string> fs_body_path(const std::string& oid,
                                          std::optional<std::uint64_t> seq, std::string& err);

  std::optional<ObjectInfo> stat(const std::string& oid, std::string& err) {
    return stat(oid, std::nullopt, err);
  }
  std::optional<ObjectInfo> stat(const std::string& oid, std::optional<std::uint64_t> seq,
                                 std::string& err);

  // Tip delete-marker version (published).
  bool del(const std::string& oid, std::string& err);

  bool set_attr(const std::string& oid, const std::string& key, const std::string& value,
                std::string& err);
  std::optional<std::string> get_attr(const std::string& oid, const std::string& key,
                                      std::string& err);
  std::unordered_map<std::string, std::string> list_attrs(const std::string& oid,
                                                          std::string& err);

  PrecondResult check_preconditions(const std::string& oid,
                                    const std::vector<AttrPrecondition>& preds,
                                    std::string& err);

  std::vector<VersionInfo> list_versions(const std::string& oid, std::string& err);
  bool purge_version(const std::string& oid, std::uint64_t seq, bool allow_tip,
                     std::string& err);
  bool trim_versions(const std::string& oid, int keep, std::string& err);

  ObjectListResult list(const std::string& prefix, const std::string& attr_eq_key,
                        const std::string& attr_eq_value, std::size_t limit,
                        const std::string& cursor, bool include_attrs, std::string& err);

  std::size_t scrub_orphans(std::string& err);
  std::vector<std::string> list_oids(std::size_t max_count, std::string& err);

 private:
  struct Shard {
    std::uint32_t id{0};
    std::string dir;
    sqlite3* db{nullptr};
  };

  Shard* shard_for(const std::string& oid);
  bool open_shard(std::uint32_t id, std::string& err);
  bool load_or_init_layout(ObjectStoreOptions requested, std::string& err);
  bool write_layout(std::string& err) const;

  static bool exec_db(sqlite3* db, const char* sql, std::string& err);
  static bool ensure_schema(sqlite3* db, std::string& err);
  static bool migrate_legacy_if_needed(sqlite3* db, const std::string& shard_dir,
                                       std::string& err);
  bool use_inline(std::size_t len) const;

  std::string version_relpath(const std::string& oid, std::uint64_t seq) const;
  bool write_fs_object(Shard& shard, const std::string& relpath, const std::uint8_t* data,
                       std::size_t len, std::string& err);
  bool remove_fs_object(Shard& shard, const std::string& relpath, std::string& err);
  bool ensure_fs_size(Shard& shard, const std::string& relpath, std::uint64_t size,
                      std::string& err);
  bool pwrite_fs(Shard& shard, const std::string& relpath, std::uint64_t offset,
                 const std::uint8_t* data, std::size_t len, std::string& err);
  bool crc_file_range(Shard& shard, const std::string& relpath, std::uint64_t offset,
                      std::uint64_t len, std::uint32_t& out_crc, std::string& err);
  bool crc_after_range_update(Shard& shard, const std::string& relpath,
                              std::uint64_t old_size, std::uint64_t offset,
                              const std::uint8_t* data, std::size_t len,
                              std::uint64_t new_size, std::uint32_t& out_crc,
                              std::string& err);

  bool begin(Shard& s, std::string& err);
  bool commit(Shard& s, std::string& err);
  bool rollback(Shard& s);

  bool tip_seq_locked(Shard& s, const std::string& oid, std::uint64_t& tip, std::string& err);
  bool next_seq_locked(Shard& s, const std::string& oid, std::uint64_t& seq, std::string& err);
  bool insert_version_locked(Shard& s, const PreparedVersion& v, const std::uint8_t* inline_data,
                             std::size_t inline_len,
                             const std::unordered_map<std::string, std::string>& attrs,
                             std::string& err);
  bool load_version_locked(Shard& s, const std::string& oid, std::uint64_t seq, ObjectInfo& out,
                           std::string& err);
  bool delete_version_row_locked(Shard& s, const std::string& oid, std::uint64_t seq,
                                 std::string& err);

  std::string root_;
  ObjectStoreOptions opts_;
  std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace aios
