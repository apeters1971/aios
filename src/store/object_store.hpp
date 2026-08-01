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
  // Number of shards under aios/shards/<id>/. Must be a power of two in [1, 65536].
  // Fixed at first open and persisted in store.json.
  std::uint32_t shard_count{256};

  // Bodies with size <= inline_max_bytes go into the shard SQLite; larger onto FS.
  std::size_t inline_max_bytes{64 * 1024};

  // Force a storage path regardless of size (for benchmarks): "auto" | "inline" | "fs"
  std::string force_mode{"auto"};
};

struct ObjectInfo {
  std::string oid;
  std::uint32_t shard{0};
  std::uint64_t size{0};
  bool inline_body{false};
  std::string fs_path;  // path relative to shard directory when !inline_body
  std::int64_t ctime_ms{0};
  std::int64_t mtime_ms{0};
};

struct ObjectListEntry {
  std::string oid;
  std::uint64_t size{0};
  std::int64_t mtime_ms{0};
  std::unordered_map<std::string, std::string> attrs;
};

struct ObjectListResult {
  std::vector<ObjectListEntry> objects;
  std::string next_cursor;  // empty if done; opaque "shard:oid"
};

enum class PrecondResult { Ok, NotFound, Conflict };

struct AttrPrecondition {
  enum class Kind {
    Eq,
    Ne,
    Absent,
    Present,
    MustExist,     // If-Match: *
    MustNotExist,  // If-None-Match: *
  };
  Kind kind{Kind::Eq};
  std::string key;
  std::string value;
};

// Maps oid -> shard index via SHA-256 (stable across processes).
std::uint32_t shard_of_oid(const std::string& oid, std::uint32_t shard_count);

class ObjectStore {
 public:
  ObjectStore() = default;
  ~ObjectStore();

  ObjectStore(const ObjectStore&) = delete;
  ObjectStore& operator=(const ObjectStore&) = delete;

  // Open or create a store rooted at an existing .../aios directory.
  bool open(const std::string& aios_root, ObjectStoreOptions opts, std::string& err);
  void close();

  bool is_open() const { return !shards_.empty(); }
  const std::string& root() const { return root_; }
  const ObjectStoreOptions& options() const { return opts_; }
  std::uint32_t shard_count() const { return opts_.shard_count; }

  bool put(const std::string& oid, const std::uint8_t* data, std::size_t len,
           const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
           std::string& err);

  bool put(const std::string& oid, const std::string& data,
           const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
           std::string& err) {
    return put(oid, reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), attrs,
               replace_attrs, err);
  }

  // Random-overwrite ranged put. Always FS-backed; grows object to offset+len.
  // Holes are sparse zeros. Merges attrs unless replace_attrs.
  bool put_range(const std::string& oid, std::uint64_t offset, const std::uint8_t* data,
                 std::size_t len, const std::unordered_map<std::string, std::string>& attrs,
                 bool replace_attrs, std::string& err);

  std::optional<std::vector<std::uint8_t>> get(const std::string& oid, std::string& err);

  // Read [offset, offset+len). Returns empty optional on error; empty vector if len==0.
  // err = "object not found" | "range unsatisfiable" | ...
  std::optional<std::vector<std::uint8_t>> get_range(const std::string& oid,
                                                     std::uint64_t offset, std::size_t len,
                                                     std::string& err);

  // Absolute path to FS body for sendfile, if FS-backed.
  std::optional<std::string> fs_body_path(const std::string& oid, std::string& err);

  std::optional<ObjectInfo> stat(const std::string& oid, std::string& err);
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

  // Fan-out prefix scan across shards. cursor empty = start.
  // attr_eq empty key means no attr filter. include_attrs fills attrs map.
  ObjectListResult list(const std::string& prefix, const std::string& attr_eq_key,
                        const std::string& attr_eq_value, std::size_t limit,
                        const std::string& cursor, bool include_attrs, std::string& err);

  // Remove orphan files under each shard's objects/ with no DB row.
  std::size_t scrub_orphans(std::string& err);

  // List object ids present in any opened or on-disk shard (for repair).
  std::vector<std::string> list_oids(std::size_t max_count, std::string& err);

 private:
  struct Shard {
    std::uint32_t id{0};
    std::string dir;  // absolute .../aios/shards/<hex>
    sqlite3* db{nullptr};
  };

  Shard* shard_for(const std::string& oid);
  bool open_shard(std::uint32_t id, std::string& err);
  bool load_or_init_layout(ObjectStoreOptions requested, std::string& err);
  bool write_layout(std::string& err) const;

  static bool exec_db(sqlite3* db, const char* sql, std::string& err);
  static bool ensure_schema(sqlite3* db, std::string& err);
  bool use_inline(std::size_t len) const;

  std::string body_relpath(const std::string& oid) const;
  bool write_fs_object(Shard& shard, const std::string& relpath, const std::uint8_t* data,
                       std::size_t len, std::string& err);
  bool remove_fs_object(Shard& shard, const std::string& relpath, std::string& err);
  bool ensure_fs_size(Shard& shard, const std::string& relpath, std::uint64_t size,
                      std::string& err);
  bool pwrite_fs(Shard& shard, const std::string& relpath, std::uint64_t offset,
                 const std::uint8_t* data, std::size_t len, std::string& err);
  bool promote_inline_to_fs_locked(Shard& s, const std::string& oid,
                                   const std::uint8_t* inline_data, std::size_t inline_len,
                                   std::string& fs_path_out, std::string& err);

  bool begin(Shard& s, std::string& err);
  bool commit(Shard& s, std::string& err);
  bool rollback(Shard& s);

  bool delete_attrs_locked(Shard& s, const std::string& oid, std::string& err);
  bool insert_attrs_locked(Shard& s, const std::string& oid,
                           const std::unordered_map<std::string, std::string>& attrs,
                           std::string& err);
  bool upsert_attrs_locked(Shard& s, const std::string& oid,
                           const std::unordered_map<std::string, std::string>& attrs,
                           std::string& err);
  bool object_exists_locked(Shard& s, const std::string& oid, std::string& err);

  std::string root_;
  ObjectStoreOptions opts_;
  std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace aios
