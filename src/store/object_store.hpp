#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace aios {

struct ObjectStoreOptions {
  std::uint32_t shard_count{256};
  std::size_t inline_max_bytes{64 * 1024};
  std::string force_mode{"auto"};  // "auto" | "inline" | "fs"
  // Retain newest N versions per oid after publish (2B).
  int max_versions{16};
  // If true, FS COW requires reflink/clone; if false, allow full-copy fallback.
  bool clone_required{true};
  // When false, skip body/dir fsync and use SQLite synchronous=OFF (dev/bench only).
  bool data_fsync{true};
  // Ranged writes on FS-backed tips are recorded as deltas (base file + patch
  // rows in SQLite) instead of cloning and rewriting the body. A chain is
  // materialized into a fresh body file once it holds delta_max_chain patches,
  // delta_max_bytes patch bytes, or when a single write exceeds delta_max_write.
  // delta_max_chain = 0 disables deltas (every range write clones the tip).
  std::uint32_t delta_max_chain{64};
  std::uint64_t delta_max_bytes{1024 * 1024};
  std::size_t delta_max_write{256 * 1024};
  // Debug/test: after every range write re-read the whole body and check the
  // block-combined CRC against a full scan (O(object) — never enable in prod).
  bool verify_range_crc{false};
};

// Body CRCs are also kept per fixed-size block so a ranged write only re-hashes
// the blocks it touches (crc32c of the whole body == combine of the block CRCs).
constexpr std::uint64_t kStoreCrcBlockSize = 64 * 1024;
std::vector<std::uint32_t> crc32c_blocks(const std::uint8_t* data, std::size_t len);
// Whole-body CRC from block CRCs of a body of `size` bytes (last block short).
std::uint32_t crc32c_from_blocks(const std::vector<std::uint32_t>& blocks, std::uint64_t size);

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
  // True when the body is fs_path (shared base file) plus the version_deltas
  // patches with seq <= this seq. Such a version has no standalone body file.
  bool delta{false};
  // Per-block CRC32C (kStoreCrcBlockSize); empty when unknown (legacy rows or
  // streamed uploads that never had a ranged write).
  std::vector<std::uint32_t> block_crcs;
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
  // When true, install_version trusts crc32c/size without re-reading the FS body
  // (e.g. CRC was accumulated while staging).
  bool crc_verified{false};
  // Set by prepare_put_range when the version was recorded as a delta over the
  // tip's body file (no standalone file; fs_path names the shared base).
  bool delta{false};
  // Per-block CRCs of the new body when known (see kStoreCrcBlockSize).
  std::vector<std::uint32_t> block_crcs;
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
                   PreparedVersion& out, std::string& err) {
    return prepare_put(oid, data, len, attrs, replace_attrs, expected_crc32c, std::nullopt, out,
                       err);
  }
  // expected_prev_tip: fail with "tip changed during upload" when the current tip
  // differs (compare-and-swap for background jobs that read a body earlier).
  bool prepare_put(const std::string& oid, const std::uint8_t* data, std::size_t len,
                   const std::unordered_map<std::string, std::string>& attrs,
                   bool replace_attrs, std::optional<std::uint32_t> expected_crc32c,
                   std::optional<std::uint64_t> expected_prev_tip, PreparedVersion& out,
                   std::string& err);
  // FS-backed prepare from a completed staging file (moved into place). Always non-inline.
  bool prepare_put_file(const std::string& oid, const std::string& staging_abs_path,
                        std::uint64_t size, std::uint32_t crc32c_val,
                        const std::unordered_map<std::string, std::string>& attrs,
                        bool replace_attrs, std::optional<std::uint32_t> expected_crc32c,
                        PreparedVersion& out, std::string& err);
  // Like prepare_put_file but uses a seq reserved earlier (pipelined upload).
  bool prepare_put_file_at_seq(const std::string& oid, std::uint64_t seq, std::uint64_t prev_tip,
                               const std::string& staging_abs_path, std::uint64_t size,
                               std::uint32_t crc32c_val,
                               const std::unordered_map<std::string, std::string>& attrs,
                               bool replace_attrs, std::optional<std::uint32_t> expected_crc32c,
                               PreparedVersion& out, std::string& err);
  // Peek next seq / tip without inserting (caller must serialize with pipelines_).
  bool peek_next_seq(const std::string& oid, std::uint64_t& seq_out, std::uint64_t& tip_out,
                     std::string& err);

  // Streaming upload helpers (shard tmp → version path).
  bool create_staging_file(const std::string& oid, std::string& abs_path_out,
                           std::string& err);
  // Deterministic replica staging path for oid@seq (under shard tmp/).
  bool stage_path_for(const std::string& oid, std::uint64_t seq, std::string& abs_path_out,
                      std::string& err);
  bool stage_truncate(const std::string& abs_path, std::string& err);
  bool stage_pwrite(const std::string& abs_path, std::uint64_t offset, const std::uint8_t* data,
                    std::size_t len, std::string& err);
  bool place_staging_as_version(const std::string& oid, std::uint64_t seq,
                                const std::string& staging_abs_path, std::string& relpath_out,
                                std::string& err);
  bool prepare_put_range(const std::string& oid, std::uint64_t offset,
                         const std::uint8_t* data, std::size_t len,
                         const std::unordered_map<std::string, std::string>& attrs,
                         bool replace_attrs, PreparedVersion& out, std::string& err);
  // Replica-side counterpart of prepare_put_range: apply the same ranged write
  // on top of the local tip, which must equal prev_tip, at exactly `seq`, and
  // require the resulting body to have expected_size/expected_crc32c. Fails
  // with err starting "range base mismatch" when the local history diverges
  // (the caller then falls back to a full-body install). Idempotent when the
  // identical version is already present.
  bool install_range_version(const std::string& oid, std::uint64_t seq, std::uint64_t prev_tip,
                             std::uint64_t offset, const std::uint8_t* data, std::size_t len,
                             std::uint64_t expected_size, std::uint32_t expected_crc32c,
                             const std::unordered_map<std::string, std::string>& attrs,
                             std::string& err);
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

  // Current tip sequence, including delete markers (0 if none). Unlike tip stat(),
  // this does not filter is_delete — repair needs it to allocate a newer seq.
  bool tip_seq(const std::string& oid, std::uint64_t& out_seq, std::string& err);

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
  // Remove leftover staging/upload files under each shard's tmp/ that are older
  // than max_age_ms (0 = all). Only safe when no other process writes this store,
  // e.g. at daemon startup before any pipeline/stage session exists.
  std::size_t sweep_tmp(std::int64_t max_age_ms, std::string& err);
  std::vector<std::string> list_oids(std::size_t max_count, std::string& err);

  // Test-only: true if any cached statement of any open shard is still stepped.
  bool debug_any_stmt_busy() const;

 private:
  struct Shard {
    std::uint32_t id{0};
    std::string dir;
    sqlite3* db{nullptr};
    mutable std::recursive_mutex mu;
    sqlite3_stmt* stmt_tip_seq{nullptr};
    sqlite3_stmt* stmt_max_seq{nullptr};
    sqlite3_stmt* stmt_load_version{nullptr};
    sqlite3_stmt* stmt_load_attrs{nullptr};
    sqlite3_stmt* stmt_get_inline{nullptr};
    sqlite3_stmt* stmt_load_deltas{nullptr};
    sqlite3_stmt* stmt_delta_stats{nullptr};
  };

  Shard* shard_for(const std::string& oid);
  bool open_shard(std::uint32_t id, std::string& err);
  bool load_or_init_layout(ObjectStoreOptions requested, std::string& err);
  bool write_layout(std::string& err) const;

  static bool exec_db(sqlite3* db, const char* sql, std::string& err);
  static bool ensure_schema(sqlite3* db, std::string& err, bool data_fsync = true);
  static bool migrate_legacy_if_needed(sqlite3* db, const std::string& shard_dir,
                                       std::string& err);
  bool use_inline(std::size_t len) const;

  std::string version_relpath(const std::string& oid, std::uint64_t seq) const;
  // A body relpath must stay inside shard.dir: relative, no leading "..".
  static bool relpath_ok(const std::string& relpath);
  bool fsync_file(const std::string& abs_path, std::string& err) const;
  bool fsync_parent_dir(const std::string& abs_path, std::string& err) const;
  // fsync tmp, rename tmp -> final, fsync final's directory (honors data_fsync).
  bool place_file_durably(const std::string& tmp_abs, const std::string& final_abs,
                          std::string& err) const;
  bool write_fs_object(Shard& shard, const std::string& relpath, const std::uint8_t* data,
                       std::size_t len, std::string& err);
  bool remove_fs_object(Shard& shard, const std::string& relpath, std::string& err);
  bool ensure_fs_size(Shard& shard, const std::string& relpath, std::uint64_t size,
                      std::string& err);
  bool pwrite_fs(Shard& shard, const std::string& relpath, std::uint64_t offset,
                 const std::uint8_t* data, std::size_t len, std::string& err,
                 bool do_fsync = true);
  bool crc_file_range(Shard& shard, const std::string& relpath, std::uint64_t offset,
                      std::uint64_t len, std::uint32_t& out_crc, std::string& err);
  // One ranged patch over a shared base body file.
  struct Delta {
    std::uint64_t seq{0};
    std::uint64_t offset{0};
    std::vector<std::uint8_t> data;
  };
  // Patches recorded on `fs_path` for `oid` with seq <= max_seq, oldest first.
  bool load_deltas_locked(Shard& s, const std::string& oid, const std::string& fs_path,
                          std::uint64_t max_seq, std::vector<Delta>& out, std::string& err);
  // Patch count / bytes on a base file (materialization policy).
  bool delta_stats_locked(Shard& s, const std::string& oid, const std::string& fs_path,
                          std::uint64_t& count, std::uint64_t& bytes, std::string& err);
  bool insert_delta_locked(Shard& s, const std::string& oid, std::uint64_t seq,
                           const std::string& fs_path, std::uint64_t offset,
                           const std::uint8_t* data, std::size_t len, std::string& err);
  // Read [offset, offset+len) of a version's logical body: inline blob, body
  // file (zero-filled past EOF up to the version size) plus delta overlay.
  // `len` must fit inside info.size.
  bool read_version_range_locked(Shard& s, const ObjectInfo& info, std::uint64_t offset,
                                 std::size_t len, std::uint8_t* out, std::string& err);
  // Read a byte range of a body file; bytes past EOF read as zero (sparse /
  // delta-grown bodies). Fails only on I/O errors.
  bool pread_fs_zero_fill(Shard& shard, const std::string& relpath, std::uint64_t offset,
                          std::size_t len, std::uint8_t* out, std::string& err);
  // Block CRCs of the tip body (loaded, or computed by one full read when the
  // row predates block CRCs).
  bool tip_block_crcs_locked(Shard& s, const ObjectInfo& tip, std::vector<std::uint32_t>& out,
                             std::string& err);
  // Shared body of prepare_put_range / install_range_version.
  bool prepare_range_locked(Shard& s, const std::string& oid, std::optional<std::uint64_t> fixed_seq,
                            std::optional<std::uint64_t> expected_prev_tip,
                            std::optional<std::uint64_t> expected_size,
                            std::optional<std::uint32_t> expected_crc32c, std::uint64_t offset,
                            const std::uint8_t* data, std::size_t len,
                            const std::unordered_map<std::string, std::string>& attrs,
                            bool replace_attrs, PreparedVersion& out, std::string& err);
  bool update_crc_locked(Shard& s, const std::string& oid, std::uint64_t seq, std::uint32_t crc,
                         const std::vector<std::uint32_t>& blocks, std::string& err);

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
                                 std::vector<std::string>& fs_unlink_out, std::string& err);
  bool rewrite_version_attrs_locked(Shard& s, const std::string& oid, std::uint64_t seq,
                                    const std::unordered_map<std::string, std::string>& attrs,
                                    std::string& err);

  std::string root_;
  ObjectStoreOptions opts_;
  std::vector<std::unique_ptr<Shard>> shards_;
  // Guards lazy open_shard; shard ops themselves use Shard::mu.
  mutable std::mutex open_mu_;
};

}  // namespace aios
