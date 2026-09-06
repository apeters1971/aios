#include "store/object_store.hpp"

#include "store/fs_clone.hpp"
#include "util/crc32c.hpp"
#include "util/file_io.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <chrono>
#include <filesystem>
#include <sys/stat.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace aios {
namespace {

std::string sha256_hex(const std::string& s) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  EVP_Digest(s.data(), s.size(), md, &md_len, EVP_sha256(), nullptr);
  static const char* hexd = "0123456789abcdef";
  std::string out(md_len * 2, '\0');
  for (unsigned int i = 0; i < md_len; ++i) {
    out[i * 2] = hexd[md[i] >> 4];
    out[i * 2 + 1] = hexd[md[i] & 0xf];
  }
  return out;
}

std::uint32_t sha256_u32(const std::string& s) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  EVP_Digest(s.data(), s.size(), md, &md_len, EVP_sha256(), nullptr);
  std::uint32_t v = 0;
  if (md_len >= 4) {
    v = (static_cast<std::uint32_t>(md[0]) << 24) |
        (static_cast<std::uint32_t>(md[1]) << 16) |
        (static_cast<std::uint32_t>(md[2]) << 8) |
        static_cast<std::uint32_t>(md[3]);
  }
  return v;
}

bool is_power_of_two(std::uint32_t n) { return n > 0 && (n & (n - 1)) == 0; }

std::string shard_dirname(std::uint32_t id, std::uint32_t shard_count) {
  unsigned width = 1;
  for (std::uint32_t x = shard_count - 1; x > 0xf; x >>= 4) ++width;
  std::ostringstream oss;
  oss << std::hex << std::setfill('0') << std::setw(static_cast<int>(width)) << id;
  return oss.str();
}

bool table_exists(sqlite3* db, const char* name, std::string& err) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1;", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(db);
    return false;
  }
  sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) {
    err.clear();
    return false;
  }
  err = sqlite3_errmsg(db);
  return false;
}

sqlite3_stmt* cached_prepare(sqlite3* db, sqlite3_stmt*& slot, const char* sql, std::string& err) {
  if (slot) {
    sqlite3_reset(slot);
    sqlite3_clear_bindings(slot);
    return slot;
  }
  if (sqlite3_prepare_v2(db, sql, -1, &slot, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(db);
    slot = nullptr;
    return nullptr;
  }
  return slot;
}

void finalize_cached(sqlite3_stmt*& slot) {
  if (slot) {
    sqlite3_finalize(slot);
    slot = nullptr;
  }
}

// Cached statements must not stay stepped between calls: a busy statement pins a
// read snapshot/WAL frames and blocks checkpoints until the next reuse.
struct StmtReset {
  sqlite3_stmt* stmt{nullptr};
  explicit StmtReset(sqlite3_stmt* s) : stmt(s) {}
  StmtReset(const StmtReset&) = delete;
  StmtReset& operator=(const StmtReset&) = delete;
  ~StmtReset() {
    if (stmt) {
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);
    }
  }
};

std::atomic<std::uint64_t> g_staging_counter{0};

bool load_attrs_for_seq(sqlite3* db, sqlite3_stmt*& slot, const std::string& oid,
                        std::uint64_t seq, std::unordered_map<std::string, std::string>& out,
                        std::string& err) {
  out.clear();
  sqlite3_stmt* stmt =
      cached_prepare(db, slot, "SELECT key, value FROM version_attrs WHERE oid=?1 AND seq=?2;", err);
  if (!stmt) return false;
  StmtReset reset(stmt);
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(seq));
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const void* blob = sqlite3_column_blob(stmt, 1);
    const int n = sqlite3_column_bytes(stmt, 1);
    std::string v;
    if (n > 0 && blob) {
      v.assign(reinterpret_cast<const char*>(blob), static_cast<std::size_t>(n));
    }
    if (k) out.emplace(k, std::move(v));
  }
  return true;
}

bool migrate_fs_legacy_file(const std::string& shard_dir, const std::string& old_rel,
                            std::string& new_rel, std::string& err) {
  // old: objects/ab/cd/hash (file) → objects/ab/cd/hash/v0000000000000001
  char buf[32];
  std::snprintf(buf, sizeof(buf), "v%016llx", static_cast<unsigned long long>(1));
  new_rel = old_rel + "/" + buf;
  const fs::path old_path = fs::path(shard_dir) / old_rel;
  const fs::path new_path = fs::path(shard_dir) / new_rel;
  std::error_code ec;
  if (!fs::is_regular_file(old_path, ec)) {
    // Already migrated or missing; keep new_rel for DB.
    return true;
  }
  if (fs::exists(new_path, ec)) return true;

  const fs::path tmp = old_path.parent_path() / (old_path.filename().string() + ".mig");
  fs::rename(old_path, tmp, ec);
  if (ec) {
    err = "migrate rename to tmp: " + ec.message();
    return false;
  }
  fs::create_directories(old_path, ec);
  if (ec) {
    err = "migrate mkdir: " + ec.message();
    fs::rename(tmp, old_path, ec);
    return false;
  }
  fs::rename(tmp, new_path, ec);
  if (ec) {
    err = "migrate rename into version: " + ec.message();
    return false;
  }
  return true;
}

// block_crcs column encoding: little-endian u32 per block.
std::vector<std::uint8_t> encode_block_crcs(const std::vector<std::uint32_t>& blocks) {
  std::vector<std::uint8_t> out(blocks.size() * 4);
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    out[i * 4] = static_cast<std::uint8_t>(blocks[i]);
    out[i * 4 + 1] = static_cast<std::uint8_t>(blocks[i] >> 8);
    out[i * 4 + 2] = static_cast<std::uint8_t>(blocks[i] >> 16);
    out[i * 4 + 3] = static_cast<std::uint8_t>(blocks[i] >> 24);
  }
  return out;
}

std::vector<std::uint32_t> decode_block_crcs(const std::uint8_t* p, std::size_t n) {
  std::vector<std::uint32_t> out;
  if (!p || n % 4 != 0) return out;
  out.resize(n / 4);
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint32_t>(p[i * 4]) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 8) |
             (static_cast<std::uint32_t>(p[i * 4 + 2]) << 16) |
             (static_cast<std::uint32_t>(p[i * 4 + 3]) << 24);
  }
  return out;
}

std::uint64_t block_count(std::uint64_t size) {
  return (size + kStoreCrcBlockSize - 1) / kStoreCrcBlockSize;
}

std::uint64_t block_len(std::uint64_t index, std::uint64_t size) {
  const std::uint64_t start = index * kStoreCrcBlockSize;
  if (start >= size) return 0;
  return std::min<std::uint64_t>(kStoreCrcBlockSize, size - start);
}

}  // namespace

std::vector<std::uint32_t> crc32c_blocks(const std::uint8_t* data, std::size_t len) {
  std::vector<std::uint32_t> out;
  out.reserve(static_cast<std::size_t>(block_count(len)));
  for (std::size_t off = 0; off < len; off += static_cast<std::size_t>(kStoreCrcBlockSize)) {
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(kStoreCrcBlockSize),
                                                len - off);
    out.push_back(crc32c(data + off, n));
  }
  return out;
}

std::uint32_t crc32c_from_blocks(const std::vector<std::uint32_t>& blocks, std::uint64_t size) {
  std::uint32_t crc = 0;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    const auto n = block_len(i, size);
    if (n == 0) break;
    crc = i == 0 ? blocks[i] : crc32c_combine(crc, blocks[i], static_cast<std::size_t>(n));
  }
  return crc;
}

std::uint32_t shard_of_oid(const std::string& oid, std::uint32_t shard_count) {
  if (shard_count == 0) return 0;
  return sha256_u32(oid) & (shard_count - 1);
}

ObjectStore::~ObjectStore() { close(); }

void ObjectStore::close() {
  for (auto& s : shards_) {
    if (!s) continue;
    // Serialize with in-flight shard operations so a statement is never
    // finalized (or the connection closed) underneath them.
    std::lock_guard<std::recursive_mutex> guard(s->mu);
    finalize_cached(s->stmt_tip_seq);
    finalize_cached(s->stmt_max_seq);
    finalize_cached(s->stmt_load_version);
    finalize_cached(s->stmt_load_attrs);
    finalize_cached(s->stmt_get_inline);
    finalize_cached(s->stmt_load_deltas);
    finalize_cached(s->stmt_delta_stats);
    if (s->db) {
      sqlite3_close(s->db);
      s->db = nullptr;
    }
  }
  shards_.clear();
  root_.clear();
}

bool ObjectStore::debug_any_stmt_busy() const {
  for (const auto& s : shards_) {
    if (!s) continue;
    std::lock_guard<std::recursive_mutex> guard(s->mu);
    for (sqlite3_stmt* st : {s->stmt_tip_seq, s->stmt_max_seq, s->stmt_load_version,
                             s->stmt_load_attrs, s->stmt_get_inline, s->stmt_load_deltas,
                             s->stmt_delta_stats}) {
      if (st && sqlite3_stmt_busy(st)) return true;
    }
  }
  return false;
}

bool ObjectStore::exec_db(sqlite3* db, const char* sql, std::string& err) {
  char* errmsg = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
    err = errmsg ? errmsg : "sqlite exec failed";
    sqlite3_free(errmsg);
    return false;
  }
  return true;
}

bool ObjectStore::ensure_schema(sqlite3* db, std::string& err, bool data_fsync) {
  const char* ddl = R"SQL(
CREATE TABLE IF NOT EXISTS object_tips (
  oid TEXT PRIMARY KEY,
  tip_seq INTEGER NOT NULL,
  ctime_ms INTEGER NOT NULL,
  place_verified INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS object_placement (
  oid TEXT NOT NULL,
  slot INTEGER NOT NULL,
  target_key TEXT NOT NULL,
  PRIMARY KEY (oid, slot)
);
CREATE INDEX IF NOT EXISTS idx_object_placement_target ON object_placement(target_key);
CREATE TABLE IF NOT EXISTS object_versions (
  oid TEXT NOT NULL,
  seq INTEGER NOT NULL,
  size INTEGER NOT NULL,
  inline BLOB,
  fs_path TEXT,
  crc32c INTEGER,
  is_delete INTEGER NOT NULL DEFAULT 0,
  ctime_ms INTEGER NOT NULL,
  redirect_oid TEXT,
  block_crcs BLOB,
  delta INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (oid, seq)
);
CREATE TABLE IF NOT EXISTS version_attrs (
  oid TEXT NOT NULL,
  seq INTEGER NOT NULL,
  key TEXT NOT NULL,
  value BLOB NOT NULL,
  PRIMARY KEY (oid, seq, key)
);
CREATE TABLE IF NOT EXISTS version_deltas (
  oid TEXT NOT NULL,
  seq INTEGER NOT NULL,
  fs_path TEXT NOT NULL,
  offset INTEGER NOT NULL,
  data BLOB NOT NULL,
  PRIMARY KEY (oid, seq)
);
CREATE INDEX IF NOT EXISTS idx_object_versions_oid ON object_versions(oid);
CREATE INDEX IF NOT EXISTS idx_version_attrs_oid_seq ON version_attrs(oid, seq);
CREATE INDEX IF NOT EXISTS idx_object_versions_fs_path ON object_versions(fs_path);
CREATE INDEX IF NOT EXISTS idx_version_deltas_file ON version_deltas(oid, fs_path, seq);
)SQL";
  if (!exec_db(db, "PRAGMA foreign_keys = ON;", err)) return false;
  if (!exec_db(db, "PRAGMA journal_mode = WAL;", err)) return false;
  // WAL + FULL fsyncs the log on every commit, so a published tip survives power
  // loss together with the fsynced body it points at.
  if (!exec_db(db, data_fsync ? "PRAGMA synchronous = FULL;" : "PRAGMA synchronous = OFF;", err))
    return false;
  if (!exec_db(db, ddl, err)) return false;
  // Older versioned DBs may lack these columns (duplicate column errors ignored).
  sqlite3_exec(db, "ALTER TABLE object_versions ADD COLUMN redirect_oid TEXT;", nullptr,
               nullptr, nullptr);
  sqlite3_exec(db, "ALTER TABLE object_versions ADD COLUMN block_crcs BLOB;", nullptr, nullptr,
               nullptr);
  sqlite3_exec(db, "ALTER TABLE object_versions ADD COLUMN delta INTEGER NOT NULL DEFAULT 0;",
               nullptr, nullptr, nullptr);
  sqlite3_exec(db, "ALTER TABLE object_tips ADD COLUMN place_verified INTEGER NOT NULL DEFAULT 0;",
               nullptr, nullptr, nullptr);
  return true;
}

bool ObjectStore::migrate_legacy_if_needed(sqlite3* db, const std::string& shard_dir,
                                          std::string& err) {
  std::string terr;
  if (!table_exists(db, "objects", terr)) {
    if (!terr.empty()) {
      err = terr;
      return false;
    }
    return true;
  }

  if (!exec_db(db, "BEGIN IMMEDIATE;", err)) return false;

  sqlite3_stmt* sel = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT oid, size, inline, fs_path, ctime_ms, crc32c FROM objects;", -1,
                         &sel, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(db);
    exec_db(db, "ROLLBACK;", terr);
    return false;
  }

  while (sqlite3_step(sel) == SQLITE_ROW) {
    const auto* oidp = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0));
    if (!oidp) continue;
    const std::string oid = oidp;
    const auto size = sqlite3_column_int64(sel, 1);
    const void* blob = sqlite3_column_blob(sel, 2);
    const int blob_len = sqlite3_column_bytes(sel, 2);
    const bool has_inline = sqlite3_column_type(sel, 2) != SQLITE_NULL;
    const auto* fsp = reinterpret_cast<const char*>(sqlite3_column_text(sel, 3));
    std::string fs_path = fsp ? fsp : "";
    const auto ctime = sqlite3_column_int64(sel, 4);
    const bool has_crc = sqlite3_column_type(sel, 5) != SQLITE_NULL;
    const auto crc = has_crc ? sqlite3_column_int64(sel, 5) : 0;

    if (!fs_path.empty()) {
      std::string new_rel;
      if (!migrate_fs_legacy_file(shard_dir, fs_path, new_rel, err)) {
        sqlite3_finalize(sel);
        exec_db(db, "ROLLBACK;", terr);
        return false;
      }
      fs_path = std::move(new_rel);
    }

    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db,
                           "INSERT OR REPLACE INTO object_versions"
                           "(oid,seq,size,inline,fs_path,crc32c,is_delete,ctime_ms) "
                           "VALUES(?1,1,?2,?3,?4,?5,0,?6);",
                           -1, &ins, nullptr) != SQLITE_OK) {
      err = sqlite3_errmsg(db);
      sqlite3_finalize(sel);
      exec_db(db, "ROLLBACK;", terr);
      return false;
    }
    sqlite3_bind_text(ins, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 2, size);
    if (has_inline && fs_path.empty()) {
      sqlite3_bind_blob(ins, 3, blob_len ? blob : "", blob_len, SQLITE_TRANSIENT);
      sqlite3_bind_null(ins, 4);
    } else {
      sqlite3_bind_null(ins, 3);
      if (fs_path.empty()) sqlite3_bind_null(ins, 4);
      else
        sqlite3_bind_text(ins, 4, fs_path.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (has_crc) sqlite3_bind_int64(ins, 5, crc);
    else
      sqlite3_bind_null(ins, 5);
    sqlite3_bind_int64(ins, 6, ctime);
    if (sqlite3_step(ins) != SQLITE_DONE) {
      err = sqlite3_errmsg(db);
      sqlite3_finalize(ins);
      sqlite3_finalize(sel);
      exec_db(db, "ROLLBACK;", terr);
      return false;
    }
    sqlite3_finalize(ins);

    sqlite3_stmt* tip = nullptr;
    if (sqlite3_prepare_v2(db,
                           "INSERT OR REPLACE INTO object_tips(oid,tip_seq,ctime_ms) "
                           "VALUES(?1,1,?2);",
                           -1, &tip, nullptr) != SQLITE_OK) {
      err = sqlite3_errmsg(db);
      sqlite3_finalize(sel);
      exec_db(db, "ROLLBACK;", terr);
      return false;
    }
    sqlite3_bind_text(tip, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(tip, 2, ctime);
    if (sqlite3_step(tip) != SQLITE_DONE) {
      err = sqlite3_errmsg(db);
      sqlite3_finalize(tip);
      sqlite3_finalize(sel);
      exec_db(db, "ROLLBACK;", terr);
      return false;
    }
    sqlite3_finalize(tip);
  }
  sqlite3_finalize(sel);

  // Copy attrs → version_attrs(seq=1) if legacy attrs table present.
  {
    std::string aerr;
    if (table_exists(db, "attrs", aerr)) {
      sqlite3_stmt* asel = nullptr;
      if (sqlite3_prepare_v2(db, "SELECT oid, key, value FROM attrs;", -1, &asel, nullptr) !=
          SQLITE_OK) {
        err = sqlite3_errmsg(db);
        exec_db(db, "ROLLBACK;", terr);
        return false;
      }
      while (sqlite3_step(asel) == SQLITE_ROW) {
        const auto* oidp = reinterpret_cast<const char*>(sqlite3_column_text(asel, 0));
        const auto* keyp = reinterpret_cast<const char*>(sqlite3_column_text(asel, 1));
        const void* blob = sqlite3_column_blob(asel, 2);
        const int n = sqlite3_column_bytes(asel, 2);
        if (!oidp || !keyp) continue;
        sqlite3_stmt* ains = nullptr;
        if (sqlite3_prepare_v2(db,
                               "INSERT OR REPLACE INTO version_attrs(oid,seq,key,value) "
                               "VALUES(?1,1,?2,?3);",
                               -1, &ains, nullptr) != SQLITE_OK) {
          err = sqlite3_errmsg(db);
          sqlite3_finalize(asel);
          exec_db(db, "ROLLBACK;", terr);
          return false;
        }
        sqlite3_bind_text(ains, 1, oidp, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ains, 2, keyp, -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(ains, 3, n ? blob : "", n, SQLITE_TRANSIENT);
        if (sqlite3_step(ains) != SQLITE_DONE) {
          err = sqlite3_errmsg(db);
          sqlite3_finalize(ains);
          sqlite3_finalize(asel);
          exec_db(db, "ROLLBACK;", terr);
          return false;
        }
        sqlite3_finalize(ains);
      }
      sqlite3_finalize(asel);
      if (!exec_db(db, "DROP TABLE attrs;", err)) {
        exec_db(db, "ROLLBACK;", terr);
        return false;
      }
    } else if (!aerr.empty()) {
      err = aerr;
      exec_db(db, "ROLLBACK;", terr);
      return false;
    }
  }

  if (!exec_db(db, "DROP TABLE objects;", err)) {
    exec_db(db, "ROLLBACK;", terr);
    return false;
  }
  return exec_db(db, "COMMIT;", err);
}

bool ObjectStore::begin(Shard& s, std::string& err) {
  return exec_db(s.db, "BEGIN IMMEDIATE;", err);
}
bool ObjectStore::commit(Shard& s, std::string& err) {
  return exec_db(s.db, "COMMIT;", err);
}
bool ObjectStore::rollback(Shard& s) {
  std::string err;
  return exec_db(s.db, "ROLLBACK;", err);
}

bool ObjectStore::use_inline(std::size_t len) const {
  if (opts_.force_mode == "inline") return true;
  if (opts_.force_mode == "fs") return false;
  return len <= opts_.inline_max_bytes;
}

std::string ObjectStore::version_relpath(const std::string& oid, std::uint64_t seq) const {
  const auto h = sha256_hex(oid);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "v%016llx", static_cast<unsigned long long>(seq));
  return std::string("objects/") + h.substr(0, 2) + "/" + h.substr(2, 2) + "/" + h + "/" +
         buf;
}

bool ObjectStore::write_layout(std::string& err) const {
  nlohmann::json j = {
      {"version", 1},
      {"shard_count", opts_.shard_count},
      {"inline_max_bytes", opts_.inline_max_bytes},
  };
  const auto path = fs::path(root_) / "store.json";
  const auto tmp = fs::path(root_) / "store.json.tmp";
  {
    std::ofstream out(tmp);
    if (!out) {
      err = "cannot write store.json.tmp";
      return false;
    }
    out << j.dump(2) << '\n';
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    err = "rename store.json: " + ec.message();
    return false;
  }
  return true;
}

bool ObjectStore::load_or_init_layout(ObjectStoreOptions requested, std::string& err) {
  const auto path = fs::path(root_) / "store.json";
  std::error_code ec;
  if (fs::is_regular_file(path, ec)) {
    try {
      std::ifstream in(path);
      nlohmann::json j;
      in >> j;
      opts_.shard_count = j.value("shard_count", requested.shard_count);
      opts_.inline_max_bytes = j.value("inline_max_bytes", requested.inline_max_bytes);
      // Runtime-only options always taken from the request.
      opts_.force_mode = requested.force_mode;
      opts_.max_versions = requested.max_versions;
      opts_.clone_required = requested.clone_required;
      opts_.data_fsync = requested.data_fsync;
      opts_.delta_max_chain = requested.delta_max_chain;
      opts_.delta_max_bytes = requested.delta_max_bytes;
      opts_.delta_max_write = requested.delta_max_write;
      opts_.verify_range_crc = requested.verify_range_crc;
      if (requested.shard_count != opts_.shard_count) {
        AIOS_LOG_WARN("ignoring requested shard_count=", requested.shard_count,
                      "; store.json has ", opts_.shard_count);
      }
    } catch (const std::exception& e) {
      err = std::string("bad store.json: ") + e.what();
      return false;
    }
  } else {
    opts_ = requested;
    if (!is_power_of_two(opts_.shard_count) || opts_.shard_count > 65536) {
      err = "shard_count must be a power of two in [1, 65536]";
      return false;
    }
    if (!write_layout(err)) return false;
  }
  if (!is_power_of_two(opts_.shard_count) || opts_.shard_count > 65536) {
    err = "invalid shard_count in store.json";
    return false;
  }
  if (opts_.max_versions < 1) opts_.max_versions = 1;
  return true;
}

bool ObjectStore::open_shard(std::uint32_t id, std::string& err) {
  std::lock_guard<std::mutex> lock(open_mu_);
  auto& slot = shards_[id];
  if (slot && slot->db) return true;

  auto shard = std::make_unique<Shard>();
  shard->id = id;
  shard->dir =
      (fs::path(root_) / "shards" / shard_dirname(id, opts_.shard_count)).string();

  std::error_code ec;
  fs::create_directories(fs::path(shard->dir) / "objects", ec);
  fs::create_directories(fs::path(shard->dir) / "tmp", ec);

  const auto db_path = (fs::path(shard->dir) / "meta.sqlite").string();
  // Concurrent openers of the same path race without open_mu_ (SQLITE busy /
  // "shard open failed" under parallel large PUTs after UnlockForRpc).
  if (sqlite3_open(db_path.c_str(), &shard->db) != SQLITE_OK) {
    err = shard->db ? sqlite3_errmsg(shard->db) : "sqlite3_open failed";
    if (shard->db) sqlite3_close(shard->db);
    return false;
  }
  if (!ensure_schema(shard->db, err, opts_.data_fsync)) {
    sqlite3_close(shard->db);
    return false;
  }
  if (!migrate_legacy_if_needed(shard->db, shard->dir, err)) {
    sqlite3_close(shard->db);
    return false;
  }
  slot = std::move(shard);
  return true;
}

bool ObjectStore::open(const std::string& aios_root, ObjectStoreOptions opts, std::string& err) {
  close();
  root_ = aios_root;
  std::error_code ec;
  if (!fs::is_directory(root_, ec)) {
    err = "aios root is not a directory: " + root_;
    return false;
  }
  if (!load_or_init_layout(std::move(opts), err)) {
    close();
    return false;
  }
  shards_.resize(opts_.shard_count);
  if (!open_shard(0, err)) {
    close();
    return false;
  }
  AIOS_LOG_INFO("object store open root=", root_, " shards=", opts_.shard_count,
                " inline_max=", opts_.inline_max_bytes, " max_versions=", opts_.max_versions);
  return true;
}

ObjectStore::Shard* ObjectStore::shard_for(const std::string& oid) {
  const auto id = shard_of_oid(oid, opts_.shard_count);
  std::string err;
  if (!open_shard(id, err)) {
    AIOS_LOG_ERROR("open shard ", id, ": ", err);
    return nullptr;
  }
  return shards_[id].get();
}

bool ObjectStore::relpath_ok(const std::string& relpath) {
  if (relpath.empty()) return false;
  const fs::path p(relpath);
  if (p.is_absolute()) return false;
  const fs::path norm = p.lexically_normal();
  if (norm.empty()) return false;
  auto first = norm.begin();
  if (first == norm.end()) return false;
  const std::string head = first->string();
  if (head == ".." || head == "." || head.empty()) return false;
  for (const auto& part : norm) {
    if (part == "..") return false;
  }
  return true;
}

bool ObjectStore::fsync_file(const std::string& abs_path, std::string& err) const {
  if (!opts_.data_fsync) return true;
  int fd = ::open(abs_path.c_str(), O_RDONLY);
  if (fd < 0) {
    err = std::string("open for fsync: ") + std::strerror(errno);
    return false;
  }
  if (::fsync(fd) != 0) {
    err = std::string("fsync data: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  ::close(fd);
  return true;
}

bool ObjectStore::fsync_parent_dir(const std::string& abs_path, std::string& err) const {
  if (!opts_.data_fsync) return true;
  const fs::path dir = fs::path(abs_path).parent_path();
  int dir_fd = ::open(dir.c_str(), O_RDONLY);
  if (dir_fd < 0) return true;  // some filesystems refuse O_RDONLY on dirs
  if (::fsync(dir_fd) != 0) {
    err = std::string("fsync dir: ") + std::strerror(errno);
    ::close(dir_fd);
    return false;
  }
  ::close(dir_fd);
  return true;
}

bool ObjectStore::place_file_durably(const std::string& tmp_abs, const std::string& final_abs,
                                     std::string& err) const {
  std::error_code ec;
  if (!fsync_file(tmp_abs, err)) {
    fs::remove(tmp_abs, ec);
    return false;
  }
  fs::create_directories(fs::path(final_abs).parent_path(), ec);
  ec.clear();
  fs::rename(tmp_abs, final_abs, ec);
  if (ec) {
    err = "rename: " + ec.message();
    fs::remove(tmp_abs, ec);
    return false;
  }
  return fsync_parent_dir(final_abs, err);
}

bool ObjectStore::write_fs_object(Shard& shard, const std::string& relpath,
                                 const std::uint8_t* data, std::size_t len,
                                 std::string& err) {
  if (!relpath_ok(relpath)) {
    err = "invalid fs relpath";
    return false;
  }
  const fs::path final_path = fs::path(shard.dir) / relpath;
  fs::path tmp_rel = fs::path("tmp") / relpath;
  tmp_rel.replace_extension(".tmp");
  const fs::path tmp_path = fs::path(shard.dir) / tmp_rel;

  std::error_code ec;
  fs::create_directories(final_path.parent_path(), ec);
  fs::create_directories(tmp_path.parent_path(), ec);

  int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    err = std::string("cannot open tmp: ") + std::strerror(errno);
    return false;
  }
  std::size_t done = 0;
  while (done < len) {
    const ssize_t n = ::write(fd, data + done, len - done);
    if (n < 0) {
      err = std::string("write failed: ") + std::strerror(errno);
      ::close(fd);
      fs::remove(tmp_path, ec);
      return false;
    }
    if (n == 0) {
      err = "write short write";
      ::close(fd);
      fs::remove(tmp_path, ec);
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  if (opts_.data_fsync && ::fsync(fd) != 0) {
    err = std::string("fsync data: ") + std::strerror(errno);
    ::close(fd);
    fs::remove(tmp_path, ec);
    return false;
  }
  if (::close(fd) != 0) {
    err = std::string("close data: ") + std::strerror(errno);
    fs::remove(tmp_path, ec);
    return false;
  }
  fs::rename(tmp_path, final_path, ec);
  if (ec) {
    err = "rename: " + ec.message();
    fs::remove(tmp_path, ec);
    return false;
  }
  return fsync_parent_dir(final_path.string(), err);
}

bool ObjectStore::remove_fs_object(Shard& shard, const std::string& relpath, std::string& err) {
  if (relpath.empty()) return true;
  if (!relpath_ok(relpath)) {
    err = "invalid fs relpath";
    return false;
  }
  std::error_code ec;
  fs::remove(fs::path(shard.dir) / relpath, ec);
  if (ec) {
    err = ec.message();
    return false;
  }
  return true;
}

bool ObjectStore::ensure_fs_size(Shard& shard, const std::string& relpath, std::uint64_t size,
                                std::string& err) {
  if (!relpath_ok(relpath)) {
    err = "invalid fs relpath";
    return false;
  }
  const fs::path path = fs::path(shard.dir) / relpath;
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    err = std::string("open for truncate: ") + std::strerror(errno);
    return false;
  }
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
    err = std::string("ftruncate: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  ::close(fd);
  return true;
}

bool ObjectStore::pwrite_fs(Shard& shard, const std::string& relpath, std::uint64_t offset,
                            const std::uint8_t* data, std::size_t len, std::string& err,
                            bool do_fsync) {
  if (!relpath_ok(relpath)) {
    err = "invalid fs relpath";
    return false;
  }
  const fs::path path = fs::path(shard.dir) / relpath;
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    err = std::string("open for pwrite: ") + std::strerror(errno);
    return false;
  }
  std::size_t done = 0;
  while (done < len) {
    const ssize_t n =
        ::pwrite(fd, data + done, len - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      err = std::string("pwrite: ") + std::strerror(errno);
      ::close(fd);
      return false;
    }
    if (n == 0) {
      err = "pwrite short write";
      ::close(fd);
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  if (do_fsync && opts_.data_fsync && ::fsync(fd) != 0) {
    err = std::string("fsync: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  ::close(fd);
  return true;
}

bool ObjectStore::crc_file_range(Shard& shard, const std::string& relpath,
                                std::uint64_t offset, std::uint64_t len,
                                std::uint32_t& out_crc, std::string& err) {
  out_crc = 0;
  if (len == 0) return true;
  if (!relpath_ok(relpath)) {
    err = "invalid fs relpath";
    return false;
  }
  const fs::path path = fs::path(shard.dir) / relpath;
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    err = std::string("open for crc: ") + std::strerror(errno);
    return false;
  }
  std::uint32_t crc = 0;
  std::uint64_t done = 0;
  std::uint8_t buf[64 * 1024];
  while (done < len) {
    const std::size_t chunk =
        static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(buf), len - done));
    ssize_t n = ::pread(fd, buf, chunk, static_cast<off_t>(offset + done));
    if (n < 0) {
      err = std::string("pread crc: ") + std::strerror(errno);
      ::close(fd);
      return false;
    }
    if (n == 0) {
      // Sparse holes inside st_size should read as zeros. Some clone+ftruncate
      // paths (reflink on overlay/tmp) report EOF instead; treat that as zeros
      // when the file is still as long as the requested range. A truly truncated
      // backing file (st_size behind the range) still fails.
      struct stat st {};
      if (::fstat(fd, &st) != 0) {
        err = std::string("fstat crc: ") + std::strerror(errno);
        ::close(fd);
        return false;
      }
      const auto pos = offset + done;
      if (static_cast<std::uint64_t>(st.st_size) <= pos) {
        err = "short read: file truncated";
        ::close(fd);
        return false;
      }
      const std::size_t hole = static_cast<std::size_t>(
          std::min<std::uint64_t>(chunk, static_cast<std::uint64_t>(st.st_size) - pos));
      std::memset(buf, 0, hole);
      n = static_cast<ssize_t>(hole);
    }
    crc = crc32c_update(crc, buf, static_cast<std::size_t>(n));
    done += static_cast<std::uint64_t>(n);
  }
  ::close(fd);
  out_crc = crc;
  return true;
}

bool ObjectStore::tip_seq_locked(Shard& s, const std::string& oid, std::uint64_t& tip,
                                 std::string& err) {
  tip = 0;
  sqlite3_stmt* stmt =
      cached_prepare(s.db, s.stmt_tip_seq, "SELECT tip_seq FROM object_tips WHERE oid=?1;", err);
  if (!stmt) return false;
  StmtReset reset(stmt);
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    tip = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    return true;
  }
  if (rc == SQLITE_DONE) return true;
  err = sqlite3_errmsg(s.db);
  return false;
}

bool ObjectStore::next_seq_locked(Shard& s, const std::string& oid, std::uint64_t& seq,
                                  std::string& err) {
  std::uint64_t tip = 0;
  if (!tip_seq_locked(s, oid, tip, err)) return false;
  // PRIMARY KEY (oid, seq) makes this an index probe, not a history scan.
  std::uint64_t max_seq = 0;
  sqlite3_stmt* stmt = cached_prepare(
      s.db, s.stmt_max_seq,
      "SELECT seq FROM object_versions WHERE oid=?1 ORDER BY seq DESC LIMIT 1;", err);
  if (!stmt) return false;
  StmtReset reset(stmt);
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    max_seq = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
  } else if (rc != SQLITE_DONE) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  seq = std::max(max_seq, tip) + 1;
  return true;
}

bool ObjectStore::insert_version_locked(
    Shard& s, const PreparedVersion& v, const std::uint8_t* inline_data, std::size_t inline_len,
    const std::unordered_map<std::string, std::string>& attrs, std::string& err) {
  const auto now = now_ms();
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(s.db,
                         "INSERT INTO object_versions"
                         "(oid,seq,size,inline,fs_path,crc32c,is_delete,ctime_ms,redirect_oid,"
                         "block_crcs,delta) "
                         "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11);",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  sqlite3_bind_text(stmt, 1, v.oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(v.seq));
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(v.size));
  const bool is_redir = !v.redirect_oid.empty();
  if (v.inline_body && !v.is_delete && !is_redir) {
    sqlite3_bind_blob(stmt, 4, inline_len ? inline_data : reinterpret_cast<const std::uint8_t*>(""),
                      static_cast<int>(inline_len), SQLITE_TRANSIENT);
    sqlite3_bind_null(stmt, 5);
  } else if (!v.fs_path.empty() && !v.is_delete && !is_redir) {
    sqlite3_bind_null(stmt, 4);
    sqlite3_bind_text(stmt, 5, v.fs_path.c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 4);
    sqlite3_bind_null(stmt, 5);
  }
  sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(v.crc32c));
  sqlite3_bind_int(stmt, 7, v.is_delete ? 1 : 0);
  sqlite3_bind_int64(stmt, 8, now);
  if (is_redir) {
    sqlite3_bind_text(stmt, 9, v.redirect_oid.c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 9);
  }
  std::vector<std::uint8_t> blocks_enc;
  if (!v.block_crcs.empty() && !v.is_delete && !is_redir &&
      v.block_crcs.size() == block_count(v.size)) {
    blocks_enc = encode_block_crcs(v.block_crcs);
    sqlite3_bind_blob(stmt, 10, blocks_enc.data(), static_cast<int>(blocks_enc.size()),
                      SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, 10);
  }
  sqlite3_bind_int(stmt, 11, (v.delta && !v.fs_path.empty() && !v.is_delete && !is_redir) ? 1 : 0);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    err = sqlite3_errmsg(s.db);
    sqlite3_finalize(stmt);
    return false;
  }
  sqlite3_finalize(stmt);

  if (!attrs.empty() && !v.is_delete) {
    sqlite3_stmt* a = nullptr;
    if (sqlite3_prepare_v2(s.db,
                           "INSERT INTO version_attrs(oid,seq,key,value) VALUES(?1,?2,?3,?4);",
                           -1, &a, nullptr) != SQLITE_OK) {
      err = sqlite3_errmsg(s.db);
      return false;
    }
    for (const auto& [k, val] : attrs) {
      sqlite3_reset(a);
      sqlite3_clear_bindings(a);
      sqlite3_bind_text(a, 1, v.oid.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(a, 2, static_cast<sqlite3_int64>(v.seq));
      sqlite3_bind_text(a, 3, k.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_blob(a, 4, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
      if (sqlite3_step(a) != SQLITE_DONE) {
        err = sqlite3_errmsg(s.db);
        sqlite3_finalize(a);
        return false;
      }
    }
    sqlite3_finalize(a);
  }
  return true;
}

bool ObjectStore::load_version_locked(Shard& s, const std::string& oid, std::uint64_t seq,
                                      ObjectInfo& out, std::string& err) {
  sqlite3_stmt* stmt = cached_prepare(
      s.db, s.stmt_load_version,
      "SELECT size, inline, fs_path, crc32c, is_delete, ctime_ms, "
      "redirect_oid, block_crcs, delta FROM object_versions WHERE oid=?1 AND seq=?2;",
      err);
  if (!stmt) return false;
  StmtReset reset(stmt);
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(seq));
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    err = "object not found";
    return false;
  }
  if (rc != SQLITE_ROW) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  out = ObjectInfo{};
  out.oid = oid;
  out.shard = shard_of_oid(oid, opts_.shard_count);
  out.seq = seq;
  out.size = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
  out.inline_body = sqlite3_column_type(stmt, 1) != SQLITE_NULL;
  const auto* fsp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  if (fsp) out.fs_path = fsp;
  if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
    out.crc32c = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 3));
    out.crc32c_known = true;
  }
  out.is_delete = sqlite3_column_int(stmt, 4) != 0;
  out.ctime_ms = sqlite3_column_int64(stmt, 5);
  out.mtime_ms = out.ctime_ms;
  const auto* redir = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  if (redir) out.redirect_oid = redir;
  if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
    out.block_crcs = decode_block_crcs(
        reinterpret_cast<const std::uint8_t*>(sqlite3_column_blob(stmt, 7)),
        static_cast<std::size_t>(sqlite3_column_bytes(stmt, 7)));
    if (out.block_crcs.size() != block_count(out.size)) out.block_crcs.clear();
  }
  out.delta = sqlite3_column_int(stmt, 8) != 0;
  if (!out.fs_path.empty()) out.inline_body = false;
  if (out.fs_path.empty()) out.delta = false;
  return true;
}

bool ObjectStore::load_deltas_locked(Shard& s, const std::string& oid, const std::string& fs_path,
                                     std::uint64_t max_seq, std::vector<Delta>& out,
                                     std::string& err) {
  out.clear();
  sqlite3_stmt* stmt = cached_prepare(
      s.db, s.stmt_load_deltas,
      "SELECT seq, offset, data FROM version_deltas WHERE oid=?1 AND fs_path=?2 AND seq<=?3 "
      "ORDER BY seq;",
      err);
  if (!stmt) return false;
  StmtReset reset(stmt);
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, fs_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(max_seq));
  int rc = 0;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    Delta d;
    d.seq = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    d.offset = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
    const auto* p = reinterpret_cast<const std::uint8_t*>(sqlite3_column_blob(stmt, 2));
    const int n = sqlite3_column_bytes(stmt, 2);
    if (p && n > 0) d.data.assign(p, p + n);
    out.push_back(std::move(d));
  }
  if (rc != SQLITE_DONE) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  return true;
}

bool ObjectStore::delta_stats_locked(Shard& s, const std::string& oid, const std::string& fs_path,
                                     std::uint64_t& count, std::uint64_t& bytes,
                                     std::string& err) {
  count = 0;
  bytes = 0;
  sqlite3_stmt* stmt = cached_prepare(
      s.db, s.stmt_delta_stats,
      "SELECT COUNT(*), COALESCE(SUM(LENGTH(data)),0) FROM version_deltas "
      "WHERE oid=?1 AND fs_path=?2;",
      err);
  if (!stmt) return false;
  StmtReset reset(stmt);
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, fs_path.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  count = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
  bytes = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
  return true;
}

bool ObjectStore::insert_delta_locked(Shard& s, const std::string& oid, std::uint64_t seq,
                                      const std::string& fs_path, std::uint64_t offset,
                                      const std::uint8_t* data, std::size_t len,
                                      std::string& err) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(s.db,
                         "INSERT INTO version_deltas(oid,seq,fs_path,offset,data) "
                         "VALUES(?1,?2,?3,?4,?5);",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(seq));
  sqlite3_bind_text(stmt, 3, fs_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(offset));
  sqlite3_bind_blob(stmt, 5, len ? data : reinterpret_cast<const std::uint8_t*>(""),
                    static_cast<int>(len), SQLITE_STATIC);
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) err = sqlite3_errmsg(s.db);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool ObjectStore::pread_fs_zero_fill(Shard& shard, const std::string& relpath,
                                     std::uint64_t offset, std::size_t len, std::uint8_t* out,
                                     std::string& err) {
  if (len == 0) return true;
  if (!relpath_ok(relpath)) {
    err = "invalid fs relpath";
    return false;
  }
  const fs::path path = fs::path(shard.dir) / relpath;
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    err = std::string("open: ") + std::strerror(errno);
    return false;
  }
  std::size_t done = 0;
  while (done < len) {
    const ssize_t n = ::pread(fd, out + done, len - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      err = std::string("pread: ") + std::strerror(errno);
      ::close(fd);
      return false;
    }
    if (n == 0) break;
    done += static_cast<std::size_t>(n);
  }
  ::close(fd);
  if (done < len) std::memset(out + done, 0, len - done);
  return true;
}

bool ObjectStore::read_version_range_locked(Shard& s, const ObjectInfo& info,
                                            std::uint64_t offset, std::size_t len,
                                            std::uint8_t* out, std::string& err) {
  if (len == 0) return true;
  if (offset > info.size || static_cast<std::uint64_t>(len) > info.size - offset) {
    err = "range beyond version size";
    return false;
  }
  if (info.fs_path.empty()) {
    // Inline blob.
    sqlite3_stmt* stmt = cached_prepare(
        s.db, s.stmt_get_inline, "SELECT inline FROM object_versions WHERE oid=?1 AND seq=?2;",
        err);
    if (!stmt) return false;
    StmtReset reset(stmt);
    sqlite3_bind_text(stmt, 1, info.oid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(info.seq));
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
      err = rc == SQLITE_DONE ? "object not found" : sqlite3_errmsg(s.db);
      return false;
    }
    const auto* blob = reinterpret_cast<const std::uint8_t*>(sqlite3_column_blob(stmt, 0));
    const auto blob_len = static_cast<std::uint64_t>(sqlite3_column_bytes(stmt, 0));
    if (blob_len != info.size) {
      err = "inline size mismatch";
      return false;
    }
    std::memcpy(out, blob + offset, len);
    return true;
  }
  if (!info.delta) {
    // A standalone body file must hold every byte of the version; a short read
    // means it was truncated or is being modified — never hand out zeros.
    if (!relpath_ok(info.fs_path)) {
      err = "invalid fs relpath";
      return false;
    }
    const fs::path path = fs::path(s.dir) / info.fs_path;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      err = std::string("open: ") + std::strerror(errno);
      return false;
    }
    std::size_t done = 0;
    while (done < len) {
      const ssize_t n = ::pread(fd, out + done, len - done, static_cast<off_t>(offset + done));
      if (n < 0) {
        err = std::string("pread: ") + std::strerror(errno);
        ::close(fd);
        return false;
      }
      if (n == 0) break;
      done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (done != len) {
      err = "short read: got " + std::to_string(done) + " of " + std::to_string(len) + " bytes";
      return false;
    }
    return true;
  }
  // Delta version: the base file may be shorter than the logical size (appends
  // live in the patch rows); bytes past EOF are zero before the overlay.
  if (!pread_fs_zero_fill(s, info.fs_path, offset, len, out, err)) return false;
  std::vector<Delta> deltas;
  if (!load_deltas_locked(s, info.oid, info.fs_path, info.seq, deltas, err)) return false;
  const std::uint64_t end = offset + len;
  for (const auto& d : deltas) {
    const std::uint64_t d_end = d.offset + d.data.size();
    const std::uint64_t lo = std::max(offset, d.offset);
    const std::uint64_t hi = std::min(end, d_end);
    if (lo >= hi) continue;
    std::memcpy(out + (lo - offset), d.data.data() + (lo - d.offset),
                static_cast<std::size_t>(hi - lo));
  }
  return true;
}

bool ObjectStore::delete_version_row_locked(Shard& s, const std::string& oid, std::uint64_t seq,
                                            std::vector<std::string>& fs_unlink_out,
                                            std::string& err) {
  ObjectInfo info;
  std::string lerr;
  const bool has = load_version_locked(s, oid, seq, info, lerr);
  if (!has && lerr != "object not found") {
    err = lerr;
    return false;
  }

  sqlite3_stmt* a = nullptr;
  if (sqlite3_prepare_v2(s.db, "DELETE FROM version_attrs WHERE oid=?1 AND seq=?2;", -1, &a,
                         nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  sqlite3_bind_text(a, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(a, 2, static_cast<sqlite3_int64>(seq));
  if (sqlite3_step(a) != SQLITE_DONE) {
    err = sqlite3_errmsg(s.db);
    sqlite3_finalize(a);
    return false;
  }
  sqlite3_finalize(a);

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(s.db, "DELETE FROM object_versions WHERE oid=?1 AND seq=?2;", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(seq));
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    err = sqlite3_errmsg(s.db);
    sqlite3_finalize(stmt);
    return false;
  }
  sqlite3_finalize(stmt);

  if (has && !info.fs_path.empty()) {
    // The body file may be shared by a delta chain (base + patch versions).
    // Unlink it — and drop its patch rows — only once no version references it;
    // otherwise drop just this version's own patch when nothing newer on the
    // same file depends on it (abort of the newest delta; seq may be reused).
    sqlite3_stmt* ref = nullptr;
    if (sqlite3_prepare_v2(s.db,
                           "SELECT MAX(seq) FROM object_versions WHERE oid=?1 AND fs_path=?2;",
                           -1, &ref, nullptr) != SQLITE_OK) {
      err = sqlite3_errmsg(s.db);
      return false;
    }
    sqlite3_bind_text(ref, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ref, 2, info.fs_path.c_str(), -1, SQLITE_TRANSIENT);
    const int rrc = sqlite3_step(ref);
    const bool referenced = rrc == SQLITE_ROW && sqlite3_column_type(ref, 0) != SQLITE_NULL;
    const std::uint64_t max_ref =
        referenced ? static_cast<std::uint64_t>(sqlite3_column_int64(ref, 0)) : 0;
    sqlite3_finalize(ref);
    if (rrc != SQLITE_ROW) {
      err = sqlite3_errmsg(s.db);
      return false;
    }
    const char* sql = !referenced
                          ? "DELETE FROM version_deltas WHERE oid=?1 AND fs_path=?2;"
                          : (max_ref < seq ? "DELETE FROM version_deltas WHERE oid=?1 AND "
                                             "fs_path=?2 AND seq>?3;"
                                           : nullptr);
    if (sql) {
      sqlite3_stmt* dd = nullptr;
      if (sqlite3_prepare_v2(s.db, sql, -1, &dd, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(s.db);
        return false;
      }
      sqlite3_bind_text(dd, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(dd, 2, info.fs_path.c_str(), -1, SQLITE_TRANSIENT);
      if (referenced) sqlite3_bind_int64(dd, 3, static_cast<sqlite3_int64>(max_ref));
      const int drc = sqlite3_step(dd);
      sqlite3_finalize(dd);
      if (drc != SQLITE_DONE) {
        err = sqlite3_errmsg(s.db);
        return false;
      }
    }
    if (!referenced) fs_unlink_out.push_back(info.fs_path);
  }
  return true;
}

bool ObjectStore::rewrite_version_attrs_locked(
    Shard& s, const std::string& oid, std::uint64_t seq,
    const std::unordered_map<std::string, std::string>& attrs, std::string& err) {
  sqlite3_stmt* del = nullptr;
  if (sqlite3_prepare_v2(s.db, "DELETE FROM version_attrs WHERE oid=?1 AND seq=?2;", -1, &del,
                         nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  sqlite3_bind_text(del, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(del, 2, static_cast<sqlite3_int64>(seq));
  if (sqlite3_step(del) != SQLITE_DONE) {
    err = sqlite3_errmsg(s.db);
    sqlite3_finalize(del);
    return false;
  }
  sqlite3_finalize(del);

  if (attrs.empty()) return true;

  sqlite3_stmt* ins = nullptr;
  if (sqlite3_prepare_v2(s.db,
                         "INSERT INTO version_attrs(oid,seq,key,value) VALUES(?1,?2,?3,?4);",
                         -1, &ins, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  for (const auto& [k, val] : attrs) {
    sqlite3_reset(ins);
    sqlite3_clear_bindings(ins);
    sqlite3_bind_text(ins, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 2, static_cast<sqlite3_int64>(seq));
    sqlite3_bind_text(ins, 3, k.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(ins, 4, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(ins) != SQLITE_DONE) {
      err = sqlite3_errmsg(s.db);
      sqlite3_finalize(ins);
      return false;
    }
  }
  sqlite3_finalize(ins);
  return true;
}

bool ObjectStore::prepare_put(const std::string& oid, const std::uint8_t* data, std::size_t len,
                              const std::unordered_map<std::string, std::string>& attrs,
                              bool replace_attrs, std::optional<std::uint32_t> expected_crc32c,
                              std::optional<std::uint64_t> expected_prev_tip,
                              PreparedVersion& out, std::string& err) {
  out = PreparedVersion{};
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  if (oid.empty()) {
    err = "empty oid";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);

  auto body_blocks = crc32c_blocks(data, len);
  const std::uint32_t body_crc = crc32c_from_blocks(body_blocks, len);
  if (expected_crc32c.has_value() && *expected_crc32c != body_crc) {
    err = "crc32c mismatch";
    return false;
  }

  if (!begin(s, err)) return false;

  std::uint64_t tip = 0;
  if (!tip_seq_locked(s, oid, tip, err)) {
    rollback(s);
    return false;
  }
  if (expected_prev_tip.has_value() && *expected_prev_tip != tip) {
    rollback(s);
    err = "tip changed during upload";
    return false;
  }

  std::unordered_map<std::string, std::string> merged;
  if (replace_attrs) {
    merged = attrs;
  } else if (tip > 0) {
    ObjectInfo tip_info;
    std::string lerr;
    if (load_version_locked(s, oid, tip, tip_info, lerr) && !tip_info.is_delete) {
      if (!load_attrs_for_seq(s.db, s.stmt_load_attrs, oid, tip, merged, err)) {
        rollback(s);
        return false;
      }
      for (const auto& [k, v] : attrs) merged[k] = v;
    } else if (lerr != "object not found" && !lerr.empty() && !tip_info.is_delete) {
      err = lerr;
      rollback(s);
      return false;
    } else {
      merged = attrs;
    }
  } else {
    merged = attrs;
  }

  std::uint64_t seq = 0;
  if (!next_seq_locked(s, oid, seq, err)) {
    rollback(s);
    return false;
  }

  PreparedVersion pv;
  pv.oid = oid;
  pv.seq = seq;
  pv.prev_tip = tip;
  pv.size = len;
  pv.crc32c = body_crc;
  pv.is_delete = false;
  pv.block_crcs = std::move(body_blocks);

  const bool as_inline = use_inline(len);
  if (as_inline) {
    pv.inline_body = true;
  } else {
    pv.inline_body = false;
    pv.fs_path = version_relpath(oid, seq);
    if (!write_fs_object(s, pv.fs_path, data, len, err)) {
      rollback(s);
      return false;
    }
  }

  if (!insert_version_locked(s, pv, data, len, merged, err)) {
    if (!pv.fs_path.empty()) {
      std::string rm_err;
      remove_fs_object(s, pv.fs_path, rm_err);
    }
    rollback(s);
    return false;
  }

  if (!commit(s, err)) {
    if (!pv.fs_path.empty()) {
      std::string rm_err;
      remove_fs_object(s, pv.fs_path, rm_err);
    }
    rollback(s);
    return false;
  }
  out = std::move(pv);
  return true;
}

bool ObjectStore::create_staging_file(const std::string& oid, std::string& abs_path_out,
                                     std::string& err) {
  abs_path_out.clear();
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  std::lock_guard<std::recursive_mutex> guard(sp->mu);
  const auto h = sha256_hex(oid);
  // Two uploads of the same oid in the same millisecond must never share (and
  // O_TRUNC) one staging file; the counter makes the name unique per process and
  // O_EXCL catches any collision with a leftover from another process.
  const auto seqno = g_staging_counter.fetch_add(1, std::memory_order_relaxed);
  const auto name = "upload-" + h + "-" + std::to_string(now_ms()) + "-" +
                    std::to_string(static_cast<unsigned long long>(getpid())) + "-" +
                    std::to_string(seqno);
  const fs::path tmp = fs::path(sp->dir) / "tmp" / name;
  std::error_code ec;
  fs::create_directories(tmp.parent_path(), ec);
  std::string cerr;
  if (!file_create_exclusive(tmp.string(), cerr)) {
    AIOS_LOG_ERROR("staging file collision or create failure ", tmp.string(), ": ", cerr);
    err = "cannot create staging file: " + cerr;
    return false;
  }
  abs_path_out = tmp.string();
  return true;
}

bool ObjectStore::stage_path_for(const std::string& oid, std::uint64_t seq,
                                std::string& abs_path_out, std::string& err) {
  abs_path_out.clear();
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  std::lock_guard<std::recursive_mutex> guard(sp->mu);
  const auto h = sha256_hex(oid);
  const fs::path tmp = fs::path(sp->dir) / "tmp" / ("stage-" + h + "-" + std::to_string(seq));
  std::error_code ec;
  fs::create_directories(tmp.parent_path(), ec);
  abs_path_out = tmp.string();
  return true;
}

bool ObjectStore::stage_truncate(const std::string& abs_path, std::string& err) {
  if (!file_truncate(abs_path, err)) {
    err = "cannot truncate staging file";
    return false;
  }
  return true;
}

bool ObjectStore::stage_pwrite(const std::string& abs_path, std::uint64_t offset,
                               const std::uint8_t* data, std::size_t len, std::string& err) {
  // Prefer a caller-held FD (ObjectService stage sessions). This path is the
  // fallback: still avoid stdio open/seek/close thrash via pwrite.
  int fd = ::open(abs_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    err = std::string("cannot open staging file: ") + std::strerror(errno);
    return false;
  }
  std::size_t done = 0;
  while (done < len) {
    const ssize_t n =
        ::pwrite(fd, data + done, len - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      err = std::string("pwrite: ") + std::strerror(errno);
      ::close(fd);
      return false;
    }
    if (n == 0) {
      err = "pwrite short write";
      ::close(fd);
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  ::close(fd);
  return true;
}

bool ObjectStore::place_staging_as_version(const std::string& oid, std::uint64_t seq,
                                          const std::string& staging_abs_path,
                                          std::string& relpath_out, std::string& err) {
  relpath_out.clear();
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  std::lock_guard<std::recursive_mutex> guard(sp->mu);
  const std::string rel = version_relpath(oid, seq);
  const fs::path final_path = fs::path(sp->dir) / rel;
  std::error_code ec;
  fs::create_directories(final_path.parent_path(), ec);
  ec.clear();
  if (!fsync_file(staging_abs_path, err)) return false;
  fs::rename(staging_abs_path, final_path, ec);
  if (ec) {
    // Cross-device rename may fail; copy next to the destination, then place durably.
    ec.clear();
    const fs::path copy_tmp = final_path.string() + ".tmp";
    fs::copy_file(staging_abs_path, copy_tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      err = "place staging: " + ec.message();
      fs::remove(copy_tmp, ec);
      return false;
    }
    if (!place_file_durably(copy_tmp.string(), final_path.string(), err)) return false;
    fs::remove(staging_abs_path, ec);
  } else if (!fsync_parent_dir(final_path.string(), err)) {
    return false;
  }
  relpath_out = rel;
  return true;
}

bool ObjectStore::peek_next_seq(const std::string& oid, std::uint64_t& seq_out,
                               std::uint64_t& tip_out, std::string& err) {
  seq_out = 0;
  tip_out = 0;
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);
  if (!tip_seq_locked(s, oid, tip_out, err)) return false;
  return next_seq_locked(s, oid, seq_out, err);
}

bool ObjectStore::prepare_put_file(const std::string& oid, const std::string& staging_abs_path,
                                   std::uint64_t size, std::uint32_t crc32c_val,
                                   const std::unordered_map<std::string, std::string>& attrs,
                                   bool replace_attrs,
                                   std::optional<std::uint32_t> expected_crc32c,
                                   PreparedVersion& out, std::string& err) {
  std::uint64_t seq = 0;
  std::uint64_t tip = 0;
  if (!peek_next_seq(oid, seq, tip, err)) return false;
  return prepare_put_file_at_seq(oid, seq, tip, staging_abs_path, size, crc32c_val, attrs,
                                 replace_attrs, expected_crc32c, out, err);
}

bool ObjectStore::prepare_put_file_at_seq(
    const std::string& oid, std::uint64_t seq, std::uint64_t prev_tip,
    const std::string& staging_abs_path, std::uint64_t size, std::uint32_t crc32c_val,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    std::optional<std::uint32_t> expected_crc32c, PreparedVersion& out, std::string& err) {
  out = PreparedVersion{};
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  if (oid.empty() || seq == 0) {
    err = "empty oid or seq";
    return false;
  }
  if (expected_crc32c.has_value() && *expected_crc32c != crc32c_val) {
    err = "crc32c mismatch";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);

  if (!begin(s, err)) return false;

  std::uint64_t tip = 0;
  if (!tip_seq_locked(s, oid, tip, err)) {
    rollback(s);
    return false;
  }
  if (tip != prev_tip) {
    rollback(s);
    err = "tip changed during upload";
    return false;
  }
  {
    ObjectInfo existing;
    std::string lerr;
    if (load_version_locked(s, oid, seq, existing, lerr)) {
      rollback(s);
      err = "version already exists";
      return false;
    }
    if (lerr != "object not found") {
      rollback(s);
      err = lerr;
      return false;
    }
  }

  std::unordered_map<std::string, std::string> merged;
  if (replace_attrs) {
    merged = attrs;
  } else if (tip > 0) {
    ObjectInfo tip_info;
    std::string lerr;
    if (load_version_locked(s, oid, tip, tip_info, lerr) && !tip_info.is_delete) {
      if (!load_attrs_for_seq(s.db, s.stmt_load_attrs, oid, tip, merged, err)) {
        rollback(s);
        return false;
      }
      for (const auto& [k, v] : attrs) merged[k] = v;
    } else {
      merged = attrs;
    }
  } else {
    merged = attrs;
  }

  PreparedVersion pv;
  pv.oid = oid;
  pv.seq = seq;
  pv.prev_tip = tip;
  pv.size = size;
  pv.crc32c = crc32c_val;
  pv.inline_body = false;
  pv.is_delete = false;
  pv.crc_verified = true;

  std::string rel;
  if (!place_staging_as_version(oid, seq, staging_abs_path, rel, err)) {
    rollback(s);
    return false;
  }
  pv.fs_path = rel;

  if (!insert_version_locked(s, pv, nullptr, 0, merged, err)) {
    std::string rm_err;
    remove_fs_object(s, pv.fs_path, rm_err);
    rollback(s);
    return false;
  }
  if (!commit(s, err)) {
    std::string rm_err;
    remove_fs_object(s, pv.fs_path, rm_err);
    rollback(s);
    return false;
  }
  out = std::move(pv);
  return true;
}

bool ObjectStore::tip_block_crcs_locked(Shard& s, const ObjectInfo& tip,
                                        std::vector<std::uint32_t>& out, std::string& err) {
  out.clear();
  if (tip.size == 0) return true;
  if (!tip.block_crcs.empty() && tip.block_crcs.size() == block_count(tip.size)) {
    out = tip.block_crcs;
    return true;
  }
  // Legacy row (or streamed upload) without block CRCs: one full read, after
  // which the new version carries them and later writes stay O(io).
  const auto nblocks = block_count(tip.size);
  out.reserve(static_cast<std::size_t>(nblocks));
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(kStoreCrcBlockSize));
  for (std::uint64_t b = 0; b < nblocks; ++b) {
    const auto n = static_cast<std::size_t>(block_len(b, tip.size));
    if (!read_version_range_locked(s, tip, b * kStoreCrcBlockSize, n, buf.data(), err)) {
      return false;
    }
    out.push_back(crc32c(buf.data(), n));
  }
  return true;
}

bool ObjectStore::prepare_range_locked(
    Shard& s, const std::string& oid, std::optional<std::uint64_t> fixed_seq,
    std::optional<std::uint64_t> expected_prev_tip, std::optional<std::uint64_t> expected_size,
    std::optional<std::uint32_t> expected_crc32c, std::uint64_t offset, const std::uint8_t* data,
    std::size_t len, const std::unordered_map<std::string, std::string>& attrs,
    bool replace_attrs, PreparedVersion& out, std::string& err) {
  out = PreparedVersion{};
  const std::uint64_t end = offset + static_cast<std::uint64_t>(len);

  if (!begin(s, err)) return false;
  // Any file created below is unlinked on failure.
  std::string new_file;
  auto fail = [&](const std::string& why) {
    if (!new_file.empty()) {
      std::string rm_err;
      remove_fs_object(s, new_file, rm_err);
    }
    rollback(s);
    if (!why.empty()) err = why;
    return false;
  };

  std::uint64_t tip = 0;
  if (!tip_seq_locked(s, oid, tip, err)) return fail("");
  if (expected_prev_tip.has_value() && *expected_prev_tip != tip) {
    return fail("range base mismatch: local tip " + std::to_string(tip) + " expected " +
                std::to_string(*expected_prev_tip));
  }

  std::uint64_t old_size = 0;
  bool tip_live = false;
  ObjectInfo tip_info;
  if (tip > 0) {
    if (!load_version_locked(s, oid, tip, tip_info, err)) return fail("");
    if (!tip_info.is_delete) {
      tip_live = true;
      old_size = tip_info.size;
    }
  }
  // A redirect tip has no body: treat as empty.
  const bool tip_has_body = tip_live && tip_info.redirect_oid.empty();
  if (!tip_has_body) old_size = 0;

  std::unordered_map<std::string, std::string> merged = attrs;
  if (tip_live && !replace_attrs) {
    std::unordered_map<std::string, std::string> tip_attrs;
    if (!load_attrs_for_seq(s.db, s.stmt_load_attrs, oid, tip, tip_attrs, err)) return fail("");
    merged = std::move(tip_attrs);
    for (const auto& [k, v] : attrs) merged[k] = v;
  }

  std::uint64_t seq = 0;
  if (fixed_seq.has_value()) {
    seq = *fixed_seq;
    ObjectInfo existing;
    std::string lerr;
    if (load_version_locked(s, oid, seq, existing, lerr)) return fail("version already exists");
    if (lerr != "object not found") return fail(lerr);
  } else if (!next_seq_locked(s, oid, seq, err)) {
    return fail("");
  }

  const std::uint64_t new_size = std::max(old_size, end);
  if (expected_size.has_value() && *expected_size != new_size) {
    return fail("range result mismatch: size " + std::to_string(new_size) + " expected " +
                std::to_string(*expected_size));
  }

  // --- CRC: reuse untouched block CRCs, hash only the blocks the write changes.
  std::vector<std::uint32_t> old_blocks;
  if (tip_has_body && old_size > 0) {
    if (!tip_block_crcs_locked(s, tip_info, old_blocks, err)) return fail("");
  }
  const std::uint64_t nblocks = block_count(new_size);
  std::vector<std::uint32_t> blocks(static_cast<std::size_t>(nblocks), 0);
  // Only the write-touched blocks are re-read / re-hashed (O(io)). A hole
  // between old_size and offset is all zeros and uses crc32c_update_zeros.
  // The last old block is a special case: if the object grew past its end, that
  // short block is now longer (zero-padded) and cannot reuse its old CRC.
  std::vector<std::uint8_t> region;
  std::uint64_t region_start = 0;
  std::uint64_t write_b_lo = 0;
  std::uint64_t write_b_hi = 0;
  const bool have_write = len > 0;
  if (have_write) {
    write_b_lo = offset / kStoreCrcBlockSize;
    write_b_hi = (end - 1) / kStoreCrcBlockSize;
    region_start = write_b_lo * kStoreCrcBlockSize;
    const std::uint64_t region_end =
        std::min(new_size, (write_b_hi + 1) * kStoreCrcBlockSize);
    region.assign(static_cast<std::size_t>(region_end - region_start), 0);
    if (tip_has_body && region_start < old_size) {
      const std::uint64_t keep = std::min(old_size, region_end) - region_start;
      if (!read_version_range_locked(s, tip_info, region_start, static_cast<std::size_t>(keep),
                                     region.data(), err)) {
        return fail("");
      }
    }
    std::memcpy(region.data() + (offset - region_start), data, len);
  }
  for (std::uint64_t b = 0; b < nblocks; ++b) {
    const auto n_new = static_cast<std::size_t>(block_len(b, new_size));
    const auto n_old = static_cast<std::size_t>(block_len(b, old_size));
    if (have_write && b >= write_b_lo && b <= write_b_hi) {
      blocks[static_cast<std::size_t>(b)] =
          crc32c(region.data() + (b * kStoreCrcBlockSize - region_start), n_new);
    } else if (b < old_blocks.size() && n_new == n_old) {
      blocks[static_cast<std::size_t>(b)] = old_blocks[static_cast<std::size_t>(b)];
    } else if (n_old == 0) {
      blocks[static_cast<std::size_t>(b)] = crc32c_update_zeros(0, n_new);
    } else {
      std::vector<std::uint8_t> blk(n_new, 0);
      if (!read_version_range_locked(s, tip_info, b * kStoreCrcBlockSize, n_old, blk.data(),
                                     err)) {
        return fail("");
      }
      blocks[static_cast<std::size_t>(b)] = crc32c(blk.data(), n_new);
    }
  }
  const std::uint32_t body_crc = crc32c_from_blocks(blocks, new_size);
  if (expected_crc32c.has_value() && *expected_crc32c != body_crc) {
    return fail("range result mismatch: crc32c differs (local history diverged)");
  }

  PreparedVersion pv;
  pv.oid = oid;
  pv.seq = seq;
  pv.prev_tip = tip;
  pv.size = new_size;
  pv.crc32c = body_crc;
  pv.is_delete = false;
  pv.crc_verified = true;
  pv.block_crcs = blocks;

  // --- Body placement.
  const bool tip_is_file = tip_has_body && !tip_info.fs_path.empty();
  bool use_delta = tip_is_file && opts_.delta_max_chain > 0 && len <= opts_.delta_max_write;
  if (use_delta) {
    std::uint64_t dcount = 0, dbytes = 0;
    if (!delta_stats_locked(s, oid, tip_info.fs_path, dcount, dbytes, err)) return fail("");
    if (dcount + 1 > opts_.delta_max_chain || dbytes + len > opts_.delta_max_bytes) {
      use_delta = false;
    }
  }

  if (use_delta) {
    // Patch row over the shared base file; no body I/O, no clone, no fsync
    // beyond the SQLite commit.
    pv.inline_body = false;
    pv.fs_path = tip_info.fs_path;
    pv.delta = true;
    if (!insert_delta_locked(s, oid, seq, pv.fs_path, offset, data, len, err)) return fail("");
  } else if ((!tip_is_file) && use_inline(new_size)) {
    // Small body stays inline: build the new blob in memory.
    std::vector<std::uint8_t> body(static_cast<std::size_t>(new_size), 0);
    if (tip_has_body && old_size > 0) {
      if (!read_version_range_locked(s, tip_info, 0, static_cast<std::size_t>(old_size),
                                     body.data(), err)) {
        return fail("");
      }
    }
    if (len > 0) std::memcpy(body.data() + offset, data, len);
    pv.inline_body = true;
    if (!insert_version_locked(s, pv, body.data(), body.size(), merged, err)) return fail("");
    if (!commit(s, err)) return fail("");
    out = std::move(pv);
    return true;
  } else {
    // Materialize a standalone body file for the new version.
    const std::string new_rel = version_relpath(oid, seq);
    const fs::path new_abs = fs::path(s.dir) / new_rel;
    std::error_code ec;
    fs::create_directories(new_abs.parent_path(), ec);
    if (tip_is_file) {
      if (!relpath_ok(tip_info.fs_path)) return fail("invalid fs relpath");
      const fs::path src = fs::path(s.dir) / tip_info.fs_path;
      if (!clone_or_copy_file(src.string(), new_abs.string(), !opts_.clone_required, err)) {
        return fail("");
      }
      new_file = new_rel;
      if (tip_info.delta) {
        // Fold the chain into the clone.
        std::vector<Delta> deltas;
        if (!load_deltas_locked(s, oid, tip_info.fs_path, tip_info.seq, deltas, err)) {
          return fail("");
        }
        for (const auto& d : deltas) {
          if (d.data.empty()) continue;
          if (!pwrite_fs(s, new_rel, d.offset, d.data.data(), d.data.size(), err,
                         /*do_fsync=*/false)) {
            return fail("");
          }
        }
      }
    } else if (tip_has_body && old_size > 0) {
      // Inline tip outgrowing the inline limit.
      std::vector<std::uint8_t> body(static_cast<std::size_t>(old_size));
      if (!read_version_range_locked(s, tip_info, 0, body.size(), body.data(), err)) {
        return fail("");
      }
      if (!write_fs_object(s, new_rel, body.data(), body.size(), err)) return fail("");
      new_file = new_rel;
    } else {
      if (!ensure_fs_size(s, new_rel, 0, err)) return fail("");
      new_file = new_rel;
    }
    if (!ensure_fs_size(s, new_rel, new_size, err)) return fail("");
    if (len > 0 && !pwrite_fs(s, new_rel, offset, data, len, err, /*do_fsync=*/false)) {
      return fail("");
    }
    if (opts_.data_fsync && !pwrite_fs(s, new_rel, 0, nullptr, 0, err, /*do_fsync=*/true)) {
      return fail("");
    }
    pv.inline_body = false;
    pv.fs_path = new_rel;
    pv.delta = false;
  }

  if (!insert_version_locked(s, pv, nullptr, 0, merged, err)) return fail("");

  if (opts_.verify_range_crc) {
    ObjectInfo chk;
    chk.oid = oid;
    chk.seq = seq;
    chk.size = new_size;
    chk.fs_path = pv.fs_path;
    chk.inline_body = false;
    chk.delta = pv.delta;
    std::vector<std::uint8_t> full(static_cast<std::size_t>(new_size));
    if (!read_version_range_locked(s, chk, 0, full.size(), full.data(), err)) return fail("");
    const std::uint32_t scan = crc32c(full.data(), full.size());
    if (scan != body_crc) {
      return fail("range crc verification failed: block-combined " + std::to_string(body_crc) +
                  " vs scan " + std::to_string(scan));
    }
  }

  if (!commit(s, err)) return fail("");
  out = std::move(pv);
  return true;
}

bool ObjectStore::prepare_put_range(const std::string& oid, std::uint64_t offset,
                                    const std::uint8_t* data, std::size_t len,
                                    const std::unordered_map<std::string, std::string>& attrs,
                                    bool replace_attrs, PreparedVersion& out, std::string& err) {
  out = PreparedVersion{};
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  if (oid.empty()) {
    err = "empty oid";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  std::lock_guard<std::recursive_mutex> guard(sp->mu);
  return prepare_range_locked(*sp, oid, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                              offset, data, len, attrs, replace_attrs, out, err);
}

bool ObjectStore::install_range_version(
    const std::string& oid, std::uint64_t seq, std::uint64_t prev_tip, std::uint64_t offset,
    const std::uint8_t* data, std::size_t len, std::uint64_t expected_size,
    std::uint32_t expected_crc32c, const std::unordered_map<std::string, std::string>& attrs,
    std::string& err) {
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  if (oid.empty() || seq == 0) {
    err = "empty oid or seq";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);

  // Idempotent retry: same version already installed.
  ObjectInfo existing;
  std::string lerr;
  if (load_version_locked(s, oid, seq, existing, lerr)) {
    if (!existing.is_delete && existing.redirect_oid.empty() && existing.size == expected_size &&
        existing.crc32c_known && existing.crc32c == expected_crc32c) {
      return true;
    }
    err = "version already exists";
    return false;
  }
  if (lerr != "object not found") {
    err = lerr;
    return false;
  }
  PreparedVersion pv;
  // Replicas apply the primary's merged attribute set verbatim.
  return prepare_range_locked(s, oid, seq, prev_tip, expected_size, expected_crc32c, offset, data,
                              len, attrs, /*replace_attrs=*/true, pv, err);
}


bool ObjectStore::prepare_delete(const std::string& oid, PreparedVersion& out, std::string& err) {
  out = PreparedVersion{};
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  if (oid.empty()) {
    err = "empty oid";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);

  if (!begin(s, err)) return false;

  std::uint64_t tip = 0;
  if (!tip_seq_locked(s, oid, tip, err)) {
    rollback(s);
    return false;
  }
  if (tip == 0) {
    rollback(s);
    err = "object not found";
    return false;
  }
  ObjectInfo tip_info;
  if (!load_version_locked(s, oid, tip, tip_info, err)) {
    rollback(s);
    return false;
  }
  if (tip_info.is_delete) {
    rollback(s);
    err = "object not found";
    return false;
  }

  std::uint64_t seq = 0;
  if (!next_seq_locked(s, oid, seq, err)) {
    rollback(s);
    return false;
  }

  PreparedVersion pv;
  pv.oid = oid;
  pv.seq = seq;
  pv.prev_tip = tip;
  pv.size = 0;
  pv.crc32c = crc32c(nullptr, 0);
  pv.inline_body = false;
  pv.is_delete = true;

  if (!insert_version_locked(s, pv, nullptr, 0, {}, err)) {
    rollback(s);
    return false;
  }
  if (!commit(s, err)) {
    rollback(s);
    return false;
  }
  out = std::move(pv);
  return true;
}

bool ObjectStore::prepare_redirect(const std::string& oid, const std::string& target_oid,
                                  const std::unordered_map<std::string, std::string>& attrs,
                                  bool replace_attrs, PreparedVersion& out, std::string& err) {
  out = PreparedVersion{};
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  if (oid.empty()) {
    err = "empty oid";
    return false;
  }
  if (target_oid.empty()) {
    err = "empty redirect target";
    return false;
  }
  if (target_oid == oid) {
    err = "redirect to self";
    return false;
  }

  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);

  if (!begin(s, err)) return false;

  std::uint64_t tip = 0;
  if (!tip_seq_locked(s, oid, tip, err)) {
    rollback(s);
    return false;
  }

  std::unordered_map<std::string, std::string> merged;
  if (replace_attrs) {
    merged = attrs;
  } else if (tip > 0) {
    ObjectInfo tip_info;
    std::string lerr;
    if (load_version_locked(s, oid, tip, tip_info, lerr) && !tip_info.is_delete) {
      if (!load_attrs_for_seq(s.db, s.stmt_load_attrs, oid, tip, merged, err)) {
        rollback(s);
        return false;
      }
      for (const auto& [k, v] : attrs) merged[k] = v;
    } else {
      merged = attrs;
    }
  } else {
    merged = attrs;
  }

  std::uint64_t seq = 0;
  if (!next_seq_locked(s, oid, seq, err)) {
    rollback(s);
    return false;
  }

  PreparedVersion pv;
  pv.oid = oid;
  pv.seq = seq;
  pv.prev_tip = tip;
  pv.size = 0;
  pv.crc32c = crc32c(nullptr, 0);
  pv.inline_body = false;
  pv.is_delete = false;
  pv.redirect_oid = target_oid;

  if (!insert_version_locked(s, pv, nullptr, 0, merged, err)) {
    rollback(s);
    return false;
  }
  if (!commit(s, err)) {
    rollback(s);
    return false;
  }
  out = std::move(pv);
  return true;
}

bool ObjectStore::put_redirect(const std::string& oid, const std::string& target_oid,
                              const std::unordered_map<std::string, std::string>& attrs,
                              bool replace_attrs, std::uint64_t* out_seq, std::string& err) {
  PreparedVersion pv;
  if (!prepare_redirect(oid, target_oid, attrs, replace_attrs, pv, err)) return false;
  if (!publish_tip(oid, pv.seq, err)) {
    std::string aerr;
    abort_version(oid, pv.seq, aerr);
    return false;
  }
  if (out_seq) *out_seq = pv.seq;
  return true;
}

bool ObjectStore::install_version(const PreparedVersion& v, const std::uint8_t* data,
                                  std::size_t len,
                                  const std::unordered_map<std::string, std::string>& attrs,
                                  std::string& err) {
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  if (v.oid.empty() || v.seq == 0) {
    err = "invalid prepared version";
    return false;
  }

  Shard* sp = shard_for(v.oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);

  if (!begin(s, err)) return false;

  // Idempotent if the same version is already present (seq divergence / retries).
  ObjectInfo existing;
  std::string lerr;
  if (load_version_locked(s, v.oid, v.seq, existing, lerr)) {
    const bool same_body =
        existing.is_delete == v.is_delete && existing.size == v.size &&
        existing.crc32c == v.crc32c && existing.redirect_oid == v.redirect_oid;
    if (!same_body) {
      rollback(s);
      err = "version already exists";
      return false;
    }
    std::unordered_map<std::string, std::string> existing_attrs;
    if (!load_attrs_for_seq(s.db, s.stmt_load_attrs, v.oid, v.seq, existing_attrs, err)) {
      rollback(s);
      return false;
    }
    if (existing_attrs == attrs) {
      rollback(s);
      return true;
    }
    if (!rewrite_version_attrs_locked(s, v.oid, v.seq, attrs, err)) {
      rollback(s);
      return false;
    }
    if (!commit(s, err)) {
      rollback(s);
      return false;
    }
    return true;
  }
  if (lerr != "object not found") {
    err = lerr;
    rollback(s);
    return false;
  }

  PreparedVersion pv = v;
  std::string written_fs;
  // Caller-supplied bodies are verified against the prepared CRC unless the
  // caller already accumulated it while staging.
  auto data_crc_ok = [&]() -> bool {
    // Callers may reuse a PreparedVersion of another body (EC shards): trust
    // supplied block CRCs only when they combine to the declared crc32c.
    const bool blocks_fit = pv.block_crcs.size() == block_count(len) &&
                            crc32c_from_blocks(pv.block_crcs, len) == pv.crc32c;
    if (pv.crc_verified && blocks_fit) return true;
    pv.block_crcs = crc32c_blocks(data, len);
    if (pv.crc_verified) return true;
    const std::uint32_t got = crc32c_from_blocks(pv.block_crcs, len);
    if (got != pv.crc32c) {
      rollback(s);
      err = "install crc32c mismatch";
      return false;
    }
    return true;
  };
  if (pv.is_delete || !pv.redirect_oid.empty()) {
    pv.size = 0;
    pv.inline_body = false;
    pv.fs_path.clear();
  } else if (pv.inline_body) {
    if (len != pv.size) {
      rollback(s);
      err = "install size mismatch";
      return false;
    }
    if (!data_crc_ok()) return false;
  } else {
    if (!pv.fs_path.empty() && !relpath_ok(pv.fs_path)) {
      AIOS_LOG_WARN("install_version ", pv.oid, "@", pv.seq, ": ignoring unsafe fs_path '",
                    pv.fs_path, "'");
      pv.fs_path.clear();
    }
    if (pv.fs_path.empty()) pv.fs_path = version_relpath(pv.oid, pv.seq);
    if (data && len > 0) {
      if (len != pv.size) {
        rollback(s);
        err = "install size mismatch";
        return false;
      }
      if (!data_crc_ok()) return false;
      if (!write_fs_object(s, pv.fs_path, data, len, err)) {
        rollback(s);
        return false;
      }
      written_fs = pv.fs_path;
    } else if (pv.size > 0) {
      const fs::path body_path = fs::path(s.dir) / pv.fs_path;
      std::error_code ec;
      if (!fs::is_regular_file(body_path, ec)) {
        rollback(s);
        err = "install missing fs body";
        return false;
      }
      const auto fsize = fs::file_size(body_path, ec);
      if (ec || fsize != pv.size) {
        rollback(s);
        err = ec ? ("install stat body: " + ec.message()) : "install fs size mismatch";
        return false;
      }
      if (!pv.crc_verified) {
        // Verify by blocks so the row gets block CRCs from the same read.
        const auto nblocks = block_count(pv.size);
        std::vector<std::uint32_t> blocks;
        blocks.reserve(static_cast<std::size_t>(nblocks));
        for (std::uint64_t b = 0; b < nblocks; ++b) {
          std::uint32_t c = 0;
          if (!crc_file_range(s, pv.fs_path, b * kStoreCrcBlockSize, block_len(b, pv.size), c,
                              err)) {
            rollback(s);
            return false;
          }
          blocks.push_back(c);
        }
        if (crc32c_from_blocks(blocks, pv.size) != pv.crc32c) {
          rollback(s);
          err = "install crc32c mismatch";
          return false;
        }
        pv.block_crcs = std::move(blocks);
      } else if (pv.block_crcs.size() != block_count(pv.size) ||
                 crc32c_from_blocks(pv.block_crcs, pv.size) != pv.crc32c) {
        pv.block_crcs.clear();
      }
    } else {
      if (!ensure_fs_size(s, pv.fs_path, 0, err)) {
        rollback(s);
        return false;
      }
      written_fs = pv.fs_path;
    }
  }

  if (!insert_version_locked(s, pv, data, len, attrs, err)) {
    if (!written_fs.empty()) {
      std::string rm_err;
      remove_fs_object(s, written_fs, rm_err);
    }
    rollback(s);
    return false;
  }
  if (!commit(s, err)) {
    if (!written_fs.empty()) {
      std::string rm_err;
      remove_fs_object(s, written_fs, rm_err);
    }
    rollback(s);
    return false;
  }
  return true;
}

bool ObjectStore::publish_tip(const std::string& oid, std::uint64_t seq, std::string& err) {
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);
  std::vector<std::string> fs_unlink;

  if (!begin(s, err)) return false;

  ObjectInfo info;
  if (!load_version_locked(s, oid, seq, info, err)) {
    rollback(s);
    return false;
  }

  const auto now = now_ms();
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(s.db,
                         "INSERT INTO object_tips(oid, tip_seq, ctime_ms, place_verified) "
                         "VALUES(?1,?2,?3,0) "
                         "ON CONFLICT(oid) DO UPDATE SET tip_seq=excluded.tip_seq, "
                         "ctime_ms=excluded.ctime_ms, place_verified=0 "
                         "WHERE excluded.tip_seq > object_tips.tip_seq;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    rollback(s);
    return false;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(seq));
  sqlite3_bind_int64(stmt, 3, now);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    err = sqlite3_errmsg(s.db);
    sqlite3_finalize(stmt);
    rollback(s);
    return false;
  }
  sqlite3_finalize(stmt);

  if (sqlite3_changes(s.db) == 0) {
    std::uint64_t cur_tip = 0;
    if (!tip_seq_locked(s, oid, cur_tip, err)) {
      rollback(s);
      return false;
    }
    if (cur_tip != seq) {
      rollback(s);
      err = "tip seq regression";
      return false;
    }
  }

  // Trim while tip is already published (tip always retained). Nested txn not allowed.
  {
    int keep = opts_.max_versions;
    if (keep < 1) keep = 1;
    std::vector<std::uint64_t> seqs;
    sqlite3_stmt* ls = nullptr;
    if (sqlite3_prepare_v2(s.db,
                           "SELECT seq FROM object_versions WHERE oid=?1 ORDER BY seq DESC;",
                           -1, &ls, nullptr) != SQLITE_OK) {
      err = sqlite3_errmsg(s.db);
      rollback(s);
      return false;
    }
    sqlite3_bind_text(ls, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(ls) == SQLITE_ROW) {
      seqs.push_back(static_cast<std::uint64_t>(sqlite3_column_int64(ls, 0)));
    }
    sqlite3_finalize(ls);

    std::unordered_map<std::uint64_t, bool> keep_set;
    for (int i = 0; i < keep && i < static_cast<int>(seqs.size()); ++i) {
      keep_set[seqs[static_cast<std::size_t>(i)]] = true;
    }
    keep_set[seq] = true;

    for (std::uint64_t old : seqs) {
      if (keep_set.count(old)) continue;
      if (!delete_version_row_locked(s, oid, old, fs_unlink, err)) {
        rollback(s);
        return false;
      }
    }
  }

  if (!commit(s, err)) {
    rollback(s);
    return false;
  }
  for (const auto& rel : fs_unlink) {
    std::string rm_err;
    if (!remove_fs_object(s, rel, rm_err)) {
      AIOS_LOG_WARN("deferred unlink failed: ", rel, ": ", rm_err);
    }
  }
  return true;
}

bool ObjectStore::abort_version(const std::string& oid, std::uint64_t seq, std::string& err) {
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);
  std::vector<std::string> fs_unlink;

  if (!begin(s, err)) return false;
  std::uint64_t tip = 0;
  if (!tip_seq_locked(s, oid, tip, err)) {
    rollback(s);
    return false;
  }
  if (tip == seq) {
    rollback(s);
    err = "cannot abort tip version";
    return false;
  }
  if (!delete_version_row_locked(s, oid, seq, fs_unlink, err)) {
    rollback(s);
    return false;
  }
  if (!commit(s, err)) {
    rollback(s);
    return false;
  }
  for (const auto& rel : fs_unlink) {
    std::string rm_err;
    if (!remove_fs_object(s, rel, rm_err)) {
      AIOS_LOG_WARN("deferred unlink failed: ", rel, ": ", rm_err);
    }
  }
  return true;
}

bool ObjectStore::put(const std::string& oid, const std::uint8_t* data, std::size_t len,
                     const std::unordered_map<std::string, std::string>& attrs,
                     bool replace_attrs, std::optional<std::uint32_t> expected_crc32c,
                     std::uint64_t* out_seq, std::string& err) {
  PreparedVersion pv;
  if (!prepare_put(oid, data, len, attrs, replace_attrs, expected_crc32c, pv, err)) {
    return false;
  }
  if (!publish_tip(oid, pv.seq, err)) {
    std::string aerr;
    abort_version(oid, pv.seq, aerr);
    return false;
  }
  if (out_seq) *out_seq = pv.seq;
  return true;
}

bool ObjectStore::put_range(const std::string& oid, std::uint64_t offset,
                            const std::uint8_t* data, std::size_t len,
                            const std::unordered_map<std::string, std::string>& attrs,
                            bool replace_attrs, std::uint64_t* out_seq, std::string& err) {
  PreparedVersion pv;
  if (!prepare_put_range(oid, offset, data, len, attrs, replace_attrs, pv, err)) {
    return false;
  }
  if (!publish_tip(oid, pv.seq, err)) {
    std::string aerr;
    abort_version(oid, pv.seq, aerr);
    return false;
  }
  if (out_seq) *out_seq = pv.seq;
  return true;
}

bool ObjectStore::del(const std::string& oid, std::string& err) {
  PreparedVersion pv;
  if (!prepare_delete(oid, pv, err)) return false;
  if (!publish_tip(oid, pv.seq, err)) {
    std::string aerr;
    abort_version(oid, pv.seq, aerr);
    return false;
  }
  return true;
}

bool ObjectStore::tip_seq(const std::string& oid, std::uint64_t& out_seq, std::string& err) {
  out_seq = 0;
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  std::lock_guard<std::recursive_mutex> guard(sp->mu);
  return tip_seq_locked(*sp, oid, out_seq, err);
}

std::optional<ObjectInfo> ObjectStore::stat(const std::string& oid,
                                            std::optional<std::uint64_t> seq,
                                            std::string& err) {
  if (!is_open()) {
    err = "store not open";
    return std::nullopt;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return std::nullopt;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);

  std::uint64_t want = 0;
  if (seq.has_value()) {
    want = *seq;
  } else {
    if (!tip_seq_locked(s, oid, want, err)) return std::nullopt;
    if (want == 0) {
      err = "object not found";
      return std::nullopt;
    }
  }

  ObjectInfo info;
  if (!load_version_locked(s, oid, want, info, err)) return std::nullopt;
  if (!seq.has_value() && info.is_delete) {
    err = "object not found";
    return std::nullopt;
  }
  return info;
}

std::optional<std::vector<std::uint8_t>> ObjectStore::get(const std::string& oid,
                                                          std::optional<std::uint64_t> seq,
                                                          std::string& err) {
  auto info = stat(oid, seq, err);
  if (!info) return std::nullopt;
  if (info->is_delete) {
    // Explicit seq may address a delete marker — no body.
    err = "object is delete marker";
    return std::nullopt;
  }
  if (!info->redirect_oid.empty()) {
    err = "object is redirect";
    return std::nullopt;
  }

  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return std::nullopt;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);

  std::vector<std::uint8_t> out(static_cast<std::size_t>(info->size));
  if (!read_version_range_locked(s, *info, 0, out.size(), out.data(), err)) {
    if (err.empty()) err = "cannot read object body: " + info->fs_path;
    return std::nullopt;
  }
  return out;
}

std::optional<std::vector<std::uint8_t>> ObjectStore::get_range(
    const std::string& oid, std::optional<std::uint64_t> seq, std::uint64_t offset,
    std::size_t len, std::string& err) {
  auto info = stat(oid, seq, err);
  if (!info) return std::nullopt;
  if (info->is_delete) {
    err = "object is delete marker";
    return std::nullopt;
  }
  if (!info->redirect_oid.empty()) {
    err = "object is redirect";
    return std::nullopt;
  }
  if (offset >= info->size) {
    err = "range unsatisfiable";
    return std::nullopt;
  }
  const std::uint64_t avail = info->size - offset;
  const std::size_t want = std::min(len, static_cast<std::size_t>(avail));
  if (want == 0) return std::vector<std::uint8_t>{};

  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return std::nullopt;
  }
  std::lock_guard<std::recursive_mutex> guard(sp->mu);
  std::vector<std::uint8_t> out(want);
  if (!read_version_range_locked(*sp, *info, offset, want, out.data(), err)) return std::nullopt;
  return out;
}

std::optional<std::string> ObjectStore::fs_body_path(const std::string& oid,
                                                     std::optional<std::uint64_t> seq,
                                                     std::string& err) {
  auto info = stat(oid, seq, err);
  if (!info) return std::nullopt;
  if (info->inline_body || info->fs_path.empty()) {
    err = "not fs-backed";
    return std::nullopt;
  }
  if (info->delta) {
    // Body = base file + patches; there is no single file to stream. Callers
    // fall back to get()/get_range().
    err = "delta version has no standalone body file";
    return std::nullopt;
  }
  if (!relpath_ok(info->fs_path)) {
    err = "invalid fs relpath";
    return std::nullopt;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return std::nullopt;
  }
  return (fs::path(sp->dir) / info->fs_path).string();
}

bool ObjectStore::set_attr(const std::string& oid, const std::string& key,
                           const std::string& value, std::string& err) {
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);

  if (!begin(s, err)) return false;

  std::uint64_t tip = 0;
  if (!tip_seq_locked(s, oid, tip, err)) {
    rollback(s);
    return false;
  }
  if (tip == 0) {
    rollback(s);
    err = "object not found";
    return false;
  }
  ObjectInfo tip_info;
  if (!load_version_locked(s, oid, tip, tip_info, err)) {
    rollback(s);
    return false;
  }
  if (tip_info.is_delete) {
    rollback(s);
    err = "object not found";
    return false;
  }

  std::unordered_map<std::string, std::string> attrs;
  if (!load_attrs_for_seq(s.db, s.stmt_load_attrs, oid, tip, attrs, err)) {
    rollback(s);
    return false;
  }
  attrs[key] = value;

  std::uint64_t seq = 0;
  if (!next_seq_locked(s, oid, seq, err)) {
    rollback(s);
    return false;
  }

  PreparedVersion pv;
  pv.oid = oid;
  pv.seq = seq;
  pv.prev_tip = tip;
  pv.size = tip_info.size;
  pv.crc32c = tip_info.crc32c;
  pv.is_delete = false;

  std::vector<std::uint8_t> inline_copy;
  if (tip_info.inline_body) {
    pv.inline_body = true;
    sqlite3_stmt* stmt = cached_prepare(
        s.db, s.stmt_get_inline, "SELECT inline FROM object_versions WHERE oid=?1 AND seq=?2;",
        err);
    if (!stmt) {
      rollback(s);
      return false;
    }
    StmtReset reset(stmt);
    sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(tip));
    if (sqlite3_step(stmt) != SQLITE_ROW) {
      err = "tip inline missing";
      rollback(s);
      return false;
    }
    const void* blob = sqlite3_column_blob(stmt, 0);
    const int n = sqlite3_column_bytes(stmt, 0);
    if (n > 0 && blob) {
      inline_copy.assign(reinterpret_cast<const std::uint8_t*>(blob),
                         reinterpret_cast<const std::uint8_t*>(blob) + n);
    }
  } else {
    pv.inline_body = false;
    pv.fs_path = version_relpath(oid, seq);
    if (!relpath_ok(tip_info.fs_path)) {
      rollback(s);
      err = "invalid fs relpath";
      return false;
    }
    const fs::path src = fs::path(s.dir) / tip_info.fs_path;
    const fs::path dst = fs::path(s.dir) / pv.fs_path;
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    if (!clone_or_copy_file(src.string(), dst.string(), !opts_.clone_required, err)) {
      rollback(s);
      return false;
    }
    if (!fsync_file(dst.string(), err) || !fsync_parent_dir(dst.string(), err)) {
      std::string rm_err;
      remove_fs_object(s, pv.fs_path, rm_err);
      rollback(s);
      return false;
    }
  }

  if (!insert_version_locked(s, pv, inline_copy.data(), inline_copy.size(), attrs, err)) {
    if (!pv.fs_path.empty()) {
      std::string rm_err;
      remove_fs_object(s, pv.fs_path, rm_err);
    }
    rollback(s);
    return false;
  }
  if (!commit(s, err)) {
    if (!pv.fs_path.empty()) {
      std::string rm_err;
      remove_fs_object(s, pv.fs_path, rm_err);
    }
    rollback(s);
    return false;
  }

  if (!publish_tip(oid, pv.seq, err)) {
    std::string aerr;
    abort_version(oid, pv.seq, aerr);
    return false;
  }
  return true;
}

std::optional<std::string> ObjectStore::get_attr(const std::string& oid, const std::string& key,
                                                 std::string& err) {
  auto info = stat(oid, std::nullopt, err);
  if (!info) return std::nullopt;
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return std::nullopt;
  }
  std::lock_guard<std::recursive_mutex> guard(sp->mu);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(sp->db,
                         "SELECT value FROM version_attrs WHERE oid=?1 AND seq=?2 AND key=?3;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(sp->db);
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(info->seq));
  sqlite3_bind_text(stmt, 3, key.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    sqlite3_finalize(stmt);
    err = "attr not found";
    return std::nullopt;
  }
  if (rc != SQLITE_ROW) {
    err = sqlite3_errmsg(sp->db);
    sqlite3_finalize(stmt);
    return std::nullopt;
  }
  const void* blob = sqlite3_column_blob(stmt, 0);
  const int n = sqlite3_column_bytes(stmt, 0);
  std::string out;
  if (n > 0 && blob) {
    out.assign(reinterpret_cast<const char*>(blob), static_cast<std::size_t>(n));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::unordered_map<std::string, std::string> ObjectStore::list_attrs(const std::string& oid,
                                                                     std::string& err) {
  std::unordered_map<std::string, std::string> out;
  auto info = stat(oid, std::nullopt, err);
  if (!info) return out;
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return out;
  }
  std::lock_guard<std::recursive_mutex> guard(sp->mu);
  if (!load_attrs_for_seq(sp->db, sp->stmt_load_attrs, oid, info->seq, out, err)) return {};
  return out;
}

PrecondResult ObjectStore::check_preconditions(const std::string& oid,
                                               const std::vector<AttrPrecondition>& preds,
                                               std::string& err) {
  err.clear();
  if (preds.empty()) return PrecondResult::Ok;

  std::string serr;
  auto info = stat(oid, std::nullopt, serr);
  const bool exists = info.has_value();  // tip_seq>0 && !is_delete

  for (const auto& p : preds) {
    if (p.kind == AttrPrecondition::Kind::MustExist) {
      if (!exists) {
        err = "object must exist";
        return PrecondResult::NotFound;
      }
      continue;
    }
    if (p.kind == AttrPrecondition::Kind::MustNotExist) {
      if (exists) {
        err = "object must not exist";
        return PrecondResult::Conflict;
      }
      continue;
    }
    if (!exists) {
      if (p.kind == AttrPrecondition::Kind::Absent) continue;
      err = "object not found";
      return PrecondResult::NotFound;
    }
    auto cur = get_attr(oid, p.key, serr);
    const bool present = cur.has_value();
    switch (p.kind) {
      case AttrPrecondition::Kind::Eq:
        if (!present || *cur != p.value) {
          err = "attr eq failed: " + p.key;
          return PrecondResult::Conflict;
        }
        break;
      case AttrPrecondition::Kind::Ne:
        if (present && *cur == p.value) {
          err = "attr ne failed: " + p.key;
          return PrecondResult::Conflict;
        }
        break;
      case AttrPrecondition::Kind::Absent:
        if (present) {
          err = "attr must be absent: " + p.key;
          return PrecondResult::Conflict;
        }
        break;
      case AttrPrecondition::Kind::Present:
        if (!present) {
          err = "attr must be present: " + p.key;
          return PrecondResult::Conflict;
        }
        break;
      default:
        break;
    }
  }
  return PrecondResult::Ok;
}

std::vector<VersionInfo> ObjectStore::list_versions(const std::string& oid, std::string& err) {
  std::vector<VersionInfo> out;
  if (!is_open()) {
    err = "store not open";
    return out;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return out;
  }
  std::lock_guard<std::recursive_mutex> guard(sp->mu);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(sp->db,
                         "SELECT seq, size, crc32c, is_delete, ctime_ms, inline, fs_path, "
                         "redirect_oid FROM object_versions WHERE oid=?1 ORDER BY seq DESC;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(sp->db);
    return out;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    VersionInfo v;
    v.seq = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    v.size = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
    if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
      v.crc32c = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 2));
      v.crc32c_known = true;
    }
    v.is_delete = sqlite3_column_int(stmt, 3) != 0;
    v.ctime_ms = sqlite3_column_int64(stmt, 4);
    const bool has_inline = sqlite3_column_type(stmt, 5) != SQLITE_NULL;
    const auto* fsp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    v.inline_body = has_inline && !(fsp && *fsp);
    const auto* redir = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
    if (redir) v.redirect_oid = redir;
    out.push_back(v);
  }
  sqlite3_finalize(stmt);
  return out;
}

bool ObjectStore::purge_version(const std::string& oid, std::uint64_t seq, bool allow_tip,
                                std::string& err) {
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);
  if (!begin(s, err)) return false;
  std::vector<std::string> fs_unlink;
  std::uint64_t tip = 0;
  if (!tip_seq_locked(s, oid, tip, err)) {
    rollback(s);
    return false;
  }
  if (seq == tip && !allow_tip) {
    rollback(s);
    err = "cannot purge tip";
    return false;
  }
  if (!delete_version_row_locked(s, oid, seq, fs_unlink, err)) {
    rollback(s);
    return false;
  }
  if (seq == tip && allow_tip) {
    // Clear tip if we purged it.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(s.db, "UPDATE object_tips SET tip_seq=0 WHERE oid=?1;", -1, &stmt,
                           nullptr) != SQLITE_OK) {
      err = sqlite3_errmsg(s.db);
      rollback(s);
      return false;
    }
    sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      err = sqlite3_errmsg(s.db);
      sqlite3_finalize(stmt);
      rollback(s);
      return false;
    }
    sqlite3_finalize(stmt);
    sqlite3_stmt* pl = nullptr;
    if (sqlite3_prepare_v2(s.db, "DELETE FROM object_placement WHERE oid=?1;", -1, &pl,
                           nullptr) != SQLITE_OK) {
      err = sqlite3_errmsg(s.db);
      rollback(s);
      return false;
    }
    sqlite3_bind_text(pl, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(pl) != SQLITE_DONE) {
      err = sqlite3_errmsg(s.db);
      sqlite3_finalize(pl);
      rollback(s);
      return false;
    }
    sqlite3_finalize(pl);
  }
  if (!commit(s, err)) {
    rollback(s);
    return false;
  }
  for (const auto& rel : fs_unlink) {
    std::string rm_err;
    if (!remove_fs_object(s, rel, rm_err)) {
      AIOS_LOG_WARN("deferred unlink failed: ", rel, ": ", rm_err);
    }
  }
  return true;
}

bool ObjectStore::trim_versions(const std::string& oid, int keep, std::string& err) {
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  if (keep < 1) keep = 1;
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);
  if (!begin(s, err)) return false;
  std::vector<std::string> fs_unlink;

  std::uint64_t tip = 0;
  if (!tip_seq_locked(s, oid, tip, err)) {
    rollback(s);
    return false;
  }

  std::vector<std::uint64_t> seqs;
  sqlite3_stmt* ls = nullptr;
  if (sqlite3_prepare_v2(s.db, "SELECT seq FROM object_versions WHERE oid=?1 ORDER BY seq DESC;",
                         -1, &ls, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    rollback(s);
    return false;
  }
  sqlite3_bind_text(ls, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(ls) == SQLITE_ROW) {
    seqs.push_back(static_cast<std::uint64_t>(sqlite3_column_int64(ls, 0)));
  }
  sqlite3_finalize(ls);

  std::unordered_map<std::uint64_t, bool> keep_set;
  for (int i = 0; i < keep && i < static_cast<int>(seqs.size()); ++i) {
    keep_set[seqs[static_cast<std::size_t>(i)]] = true;
  }
  if (tip > 0) keep_set[tip] = true;

  for (std::uint64_t old : seqs) {
    if (keep_set.count(old)) continue;
    if (!delete_version_row_locked(s, oid, old, fs_unlink, err)) {
      rollback(s);
      return false;
    }
  }
  if (!commit(s, err)) {
    rollback(s);
    return false;
  }
  for (const auto& rel : fs_unlink) {
    std::string rm_err;
    if (!remove_fs_object(s, rel, rm_err)) {
      AIOS_LOG_WARN("deferred unlink failed: ", rel, ": ", rm_err);
    }
  }
  return true;
}

ObjectListResult ObjectStore::list(const std::string& prefix, const std::string& attr_eq_key,
                                  const std::string& attr_eq_value, std::size_t limit,
                                  const std::string& cursor, bool include_attrs,
                                  std::string& err) {
  ObjectListResult out;
  if (!is_open()) {
    err = "store not open";
    return out;
  }
  if (limit == 0) limit = 1000;

  std::uint32_t start_shard = 0;
  std::string start_oid;
  if (!cursor.empty()) {
    const auto pos = cursor.find(':');
    if (pos == std::string::npos) {
      err = "bad cursor";
      return out;
    }
    try {
      start_shard = static_cast<std::uint32_t>(std::stoul(cursor.substr(0, pos), nullptr, 16));
    } catch (...) {
      err = "bad cursor shard";
      return out;
    }
    start_oid = cursor.substr(pos + 1);
  }

  const std::string like = prefix + "%";
  for (std::uint32_t id = start_shard; id < opts_.shard_count && out.objects.size() < limit;
       ++id) {
    if (!open_shard(id, err)) return out;
    Shard& s = *shards_[id];
    std::lock_guard<std::recursive_mutex> guard(s.mu);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT t.oid, v.seq, v.size, v.ctime_ms, v.crc32c, v.is_delete, v.redirect_oid "
        "FROM object_tips t "
        "JOIN object_versions v ON v.oid = t.oid AND v.seq = t.tip_seq "
        "WHERE t.tip_seq > 0 AND v.is_delete = 0 AND t.oid LIKE ?1 "
        "AND (?2 = '' OR t.oid > ?2) ORDER BY t.oid;";
    if (sqlite3_prepare_v2(s.db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      err = sqlite3_errmsg(s.db);
      return out;
    }
    sqlite3_bind_text(stmt, 1, like.c_str(), -1, SQLITE_TRANSIENT);
    const std::string oid_bound = (id == start_shard) ? start_oid : "";
    sqlite3_bind_text(stmt, 2, oid_bound.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW && out.objects.size() < limit) {
      const auto* oidp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      if (!oidp) continue;
      ObjectListEntry e;
      e.oid = oidp;
      e.seq = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
      e.size = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2));
      e.mtime_ms = sqlite3_column_int64(stmt, 3);
      if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
        e.crc32c = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 4));
        e.crc32c_known = true;
      }
      e.is_delete = sqlite3_column_int(stmt, 5) != 0;
      const auto* redir = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
      if (redir) e.redirect_oid = redir;

      if (!attr_eq_key.empty()) {
        std::string aerr;
        auto v = get_attr(e.oid, attr_eq_key, aerr);
        if (!v || *v != attr_eq_value) continue;
      }
      if (include_attrs) {
        std::string aerr;
        e.attrs = list_attrs(e.oid, aerr);
      }
      out.objects.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);

    if (out.objects.size() >= limit) {
      std::ostringstream oss;
      oss << std::hex << id << ':' << out.objects.back().oid;
      out.next_cursor = oss.str();
      break;
    }
  }

  if (out.objects.size() < limit) out.next_cursor.clear();
  err.clear();
  return out;
}

std::vector<std::string> ObjectStore::list_oids(std::size_t max_count, std::string& err) {
  std::vector<std::string> out;
  if (!is_open()) {
    err = "store not open";
    return out;
  }
  err.clear();
  const fs::path shards_root = fs::path(root_) / "shards";
  std::error_code ec;
  if (!fs::exists(shards_root, ec)) return out;

  for (auto it = fs::directory_iterator(shards_root, ec); it != fs::directory_iterator(); ++it) {
    if (!it->is_directory(ec)) continue;
    const auto db_path = it->path() / "meta.sqlite";
    if (!fs::is_regular_file(db_path, ec)) continue;

    std::uint32_t id = 0;
    try {
      id = static_cast<std::uint32_t>(std::stoul(it->path().filename().string(), nullptr, 16));
    } catch (...) {
      continue;
    }
    if (id >= opts_.shard_count) continue;
    if (!open_shard(id, err)) return out;
    Shard& s = *shards_[id];
    std::lock_guard<std::recursive_mutex> guard(s.mu);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(s.db, "SELECT oid FROM object_tips WHERE tip_seq > 0;", -1, &stmt,
                           nullptr) != SQLITE_OK) {
      err = sqlite3_errmsg(s.db);
      return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const auto* oid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      if (oid) out.emplace_back(oid);
      if (max_count > 0 && out.size() >= max_count) {
        sqlite3_finalize(stmt);
        return out;
      }
    }
    sqlite3_finalize(stmt);
  }
  return out;
}

bool ObjectStore::set_placement(const std::string& oid, const std::vector<std::string>& target_keys,
                                bool verified, std::string& err) {
  if (!is_open()) {
    err = "store not open";
    return false;
  }
  if (oid.empty()) {
    err = "empty oid";
    return false;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);
  if (!begin(s, err)) return false;
  sqlite3_stmt* del = nullptr;
  if (sqlite3_prepare_v2(s.db, "DELETE FROM object_placement WHERE oid=?1;", -1, &del, nullptr) !=
      SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    rollback(s);
    return false;
  }
  sqlite3_bind_text(del, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(del) != SQLITE_DONE) {
    err = sqlite3_errmsg(s.db);
    sqlite3_finalize(del);
    rollback(s);
    return false;
  }
  sqlite3_finalize(del);
  sqlite3_stmt* ins = nullptr;
  if (sqlite3_prepare_v2(s.db,
                         "INSERT INTO object_placement(oid, slot, target_key) VALUES(?1,?2,?3);",
                         -1, &ins, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    rollback(s);
    return false;
  }
  for (std::size_t i = 0; i < target_keys.size(); ++i) {
    sqlite3_reset(ins);
    sqlite3_clear_bindings(ins);
    sqlite3_bind_text(ins, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 2, static_cast<sqlite3_int64>(i));
    sqlite3_bind_text(ins, 3, target_keys[i].c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(ins) != SQLITE_DONE) {
      err = sqlite3_errmsg(s.db);
      sqlite3_finalize(ins);
      rollback(s);
      return false;
    }
  }
  sqlite3_finalize(ins);
  sqlite3_stmt* ver = nullptr;
  if (sqlite3_prepare_v2(s.db, "UPDATE object_tips SET place_verified=?1 WHERE oid=?2;", -1, &ver,
                         nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    rollback(s);
    return false;
  }
  sqlite3_bind_int(ver, 1, verified ? 1 : 0);
  sqlite3_bind_text(ver, 2, oid.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(ver) != SQLITE_DONE) {
    err = sqlite3_errmsg(s.db);
    sqlite3_finalize(ver);
    rollback(s);
    return false;
  }
  sqlite3_finalize(ver);
  if (!commit(s, err)) {
    rollback(s);
    return false;
  }
  err.clear();
  return true;
}

std::optional<ObjectStore::ObjectPlacement> ObjectStore::get_placement(const std::string& oid,
                                                                       std::string& err) {
  if (!is_open()) {
    err = "store not open";
    return std::nullopt;
  }
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return std::nullopt;
  }
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);
  ObjectPlacement out;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(s.db,
                         "SELECT slot, target_key FROM object_placement WHERE oid=?1 "
                         "ORDER BY slot ASC;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (key) out.target_keys.emplace_back(key);
  }
  sqlite3_finalize(stmt);
  sqlite3_stmt* ver = nullptr;
  if (sqlite3_prepare_v2(s.db, "SELECT place_verified FROM object_tips WHERE oid=?1;", -1, &ver,
                         nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return std::nullopt;
  }
  sqlite3_bind_text(ver, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  bool have_tip = false;
  if (sqlite3_step(ver) == SQLITE_ROW) {
    have_tip = true;
    out.verified = sqlite3_column_int(ver, 0) != 0;
  }
  sqlite3_finalize(ver);
  err.clear();
  if (!have_tip && out.target_keys.empty()) return std::nullopt;
  return out;
}

std::vector<std::string> ObjectStore::list_oids_unverified(std::size_t max_count,
                                                           std::string& err) {
  std::vector<std::string> out;
  if (!is_open()) {
    err = "store not open";
    return out;
  }
  err.clear();
  const fs::path shards_root = fs::path(root_) / "shards";
  std::error_code ec;
  if (!fs::exists(shards_root, ec)) return out;

  for (auto it = fs::directory_iterator(shards_root, ec); it != fs::directory_iterator(); ++it) {
    if (!it->is_directory(ec)) continue;
    const auto db_path = it->path() / "meta.sqlite";
    if (!fs::is_regular_file(db_path, ec)) continue;
    std::uint32_t id = 0;
    try {
      id = static_cast<std::uint32_t>(std::stoul(it->path().filename().string(), nullptr, 16));
    } catch (...) {
      continue;
    }
    if (id >= opts_.shard_count) continue;
    if (!open_shard(id, err)) return out;
    Shard& s = *shards_[id];
    std::lock_guard<std::recursive_mutex> guard(s.mu);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(s.db,
                           "SELECT oid FROM object_tips WHERE tip_seq > 0 AND "
                           "place_verified = 0 ORDER BY oid;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
      err = sqlite3_errmsg(s.db);
      return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const auto* oid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      if (oid) out.emplace_back(oid);
      if (max_count > 0 && out.size() >= max_count) {
        sqlite3_finalize(stmt);
        return out;
      }
    }
    sqlite3_finalize(stmt);
  }
  return out;
}

std::vector<std::string> ObjectStore::list_oids_for_targets(
    const std::vector<std::string>& target_keys, std::size_t max_count, std::string& err) {
  std::vector<std::string> out;
  if (!is_open()) {
    err = "store not open";
    return out;
  }
  err.clear();
  if (target_keys.empty()) return out;
  const fs::path shards_root = fs::path(root_) / "shards";
  std::error_code ec;
  if (!fs::exists(shards_root, ec)) return out;

  std::string sql =
      "SELECT DISTINCT p.oid FROM object_placement p "
      "INNER JOIN object_tips t ON t.oid = p.oid AND t.tip_seq > 0 "
      "WHERE p.target_key IN (";
  for (std::size_t i = 0; i < target_keys.size(); ++i) {
    if (i) sql += ',';
    sql += '?' + std::to_string(i + 1);
  }
  sql += ") ORDER BY p.oid;";

  for (auto it = fs::directory_iterator(shards_root, ec); it != fs::directory_iterator(); ++it) {
    if (!it->is_directory(ec)) continue;
    const auto db_path = it->path() / "meta.sqlite";
    if (!fs::is_regular_file(db_path, ec)) continue;
    std::uint32_t id = 0;
    try {
      id = static_cast<std::uint32_t>(std::stoul(it->path().filename().string(), nullptr, 16));
    } catch (...) {
      continue;
    }
    if (id >= opts_.shard_count) continue;
    if (!open_shard(id, err)) return out;
    Shard& s = *shards_[id];
    std::lock_guard<std::recursive_mutex> guard(s.mu);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(s.db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      err = sqlite3_errmsg(s.db);
      return out;
    }
    for (std::size_t i = 0; i < target_keys.size(); ++i) {
      sqlite3_bind_text(stmt, static_cast<int>(i + 1), target_keys[i].c_str(), -1,
                        SQLITE_TRANSIENT);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const auto* oid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      if (oid) out.emplace_back(oid);
      if (max_count > 0 && out.size() >= max_count) {
        sqlite3_finalize(stmt);
        return out;
      }
    }
    sqlite3_finalize(stmt);
  }
  return out;
}

std::size_t ObjectStore::scrub_orphans(std::string& err) {
  if (!is_open()) {
    err = "store not open";
    return 0;
  }
  std::size_t removed = 0;
  for (std::uint32_t id = 0; id < opts_.shard_count; ++id) {
    if (!open_shard(id, err)) return removed;
    Shard& s = *shards_[id];
    std::lock_guard<std::recursive_mutex> guard(s.mu);
    // Patch rows whose base file no version references any more.
    exec_db(s.db,
            "DELETE FROM version_deltas WHERE NOT EXISTS (SELECT 1 FROM object_versions v "
            "WHERE v.oid=version_deltas.oid AND v.fs_path=version_deltas.fs_path);",
            err);
    err.clear();
    const fs::path objects = fs::path(s.dir) / "objects";
    std::error_code ec;
    if (!fs::exists(objects, ec)) continue;
    for (auto it = fs::recursive_directory_iterator(objects, ec);
         it != fs::recursive_directory_iterator(); ++it) {
      if (!it->is_regular_file(ec)) continue;
      const auto rel = fs::relative(it->path(), s.dir, ec).generic_string();
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(s.db, "SELECT 1 FROM object_versions WHERE fs_path=?1;", -1, &stmt,
                             nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(s.db);
        return removed;
      }
      sqlite3_bind_text(stmt, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
      const int rc = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      if (rc == SQLITE_DONE) {
        fs::remove(it->path(), ec);
        if (!ec) ++removed;
      }
    }
  }
  return removed;
}

std::size_t ObjectStore::sweep_tmp(std::int64_t max_age_ms, std::string& err) {
  if (!is_open()) {
    err = "store not open";
    return 0;
  }
  std::size_t removed = 0;
  const auto now = fs::file_time_type::clock::now();
  for (std::uint32_t id = 0; id < opts_.shard_count; ++id) {
    if (!open_shard(id, err)) return removed;
    Shard& s = *shards_[id];
    std::lock_guard<std::recursive_mutex> guard(s.mu);
    const fs::path tmp = fs::path(s.dir) / "tmp";
    std::error_code ec;
    if (!fs::exists(tmp, ec)) continue;
    for (auto it = fs::recursive_directory_iterator(tmp, ec);
         it != fs::recursive_directory_iterator(); ++it) {
      if (!it->is_regular_file(ec)) continue;
      const auto mtime = it->last_write_time(ec);
      if (ec) continue;
      const auto age =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - mtime).count();
      if (age < max_age_ms) continue;
      fs::remove(it->path(), ec);
      if (!ec) ++removed;
    }
  }
  return removed;
}

bool ObjectStore::recompute_crc32c(const std::string& oid, std::uint32_t& out_crc,
                                  std::string& err) {
  out_crc = 0;
  auto info = stat(oid, std::nullopt, err);
  if (!info) return false;
  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return false;
  }
  std::lock_guard<std::recursive_mutex> guard(sp->mu);
  std::vector<std::uint32_t> blocks;
  if (info->size == 0) {
    out_crc = crc32c(nullptr, 0);
  } else if (info->inline_body || info->delta) {
    auto data = get(oid, info->seq, err);
    if (!data) return false;
    blocks = crc32c_blocks(data->data(), data->size());
    out_crc = crc32c_from_blocks(blocks, data->size());
  } else {
    // Stream the body file block by block (no whole-object buffer).
    const auto nblocks = block_count(info->size);
    blocks.reserve(static_cast<std::size_t>(nblocks));
    for (std::uint64_t b = 0; b < nblocks; ++b) {
      std::uint32_t c = 0;
      if (!crc_file_range(*sp, info->fs_path, b * kStoreCrcBlockSize, block_len(b, info->size),
                          c, err)) {
        return false;
      }
      blocks.push_back(c);
    }
    out_crc = crc32c_from_blocks(blocks, info->size);
  }

  if (!begin(*sp, err)) return false;
  if (!update_crc_locked(*sp, oid, info->seq, out_crc, blocks, err)) {
    rollback(*sp);
    return false;
  }
  return commit(*sp, err);
}

bool ObjectStore::update_crc_locked(Shard& s, const std::string& oid, std::uint64_t seq,
                                    std::uint32_t crc, const std::vector<std::uint32_t>& blocks,
                                    std::string& err) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(s.db,
                         "UPDATE object_versions SET crc32c=?1, block_crcs=?2 "
                         "WHERE oid=?3 AND seq=?4;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  const auto enc = encode_block_crcs(blocks);
  sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(crc));
  if (enc.empty()) {
    sqlite3_bind_null(stmt, 2);
  } else {
    sqlite3_bind_blob(stmt, 2, enc.data(), static_cast<int>(enc.size()), SQLITE_STATIC);
  }
  sqlite3_bind_text(stmt, 3, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(seq));
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) err = sqlite3_errmsg(s.db);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

}  // namespace aios
