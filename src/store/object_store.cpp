#include "store/object_store.hpp"

#include "store/fs_clone.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
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

bool load_attrs_for_seq(sqlite3* db, const std::string& oid, std::uint64_t seq,
                        std::unordered_map<std::string, std::string>& out, std::string& err) {
  out.clear();
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT key, value FROM version_attrs WHERE oid=?1 AND seq=?2;", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(db);
    return false;
  }
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
  sqlite3_finalize(stmt);
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

}  // namespace

std::uint32_t shard_of_oid(const std::string& oid, std::uint32_t shard_count) {
  if (shard_count == 0) return 0;
  return sha256_u32(oid) & (shard_count - 1);
}

ObjectStore::~ObjectStore() { close(); }

void ObjectStore::close() {
  for (auto& s : shards_) {
    if (s && s->db) {
      sqlite3_close(s->db);
      s->db = nullptr;
    }
  }
  shards_.clear();
  root_.clear();
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

bool ObjectStore::ensure_schema(sqlite3* db, std::string& err) {
  const char* ddl = R"SQL(
CREATE TABLE IF NOT EXISTS object_tips (
  oid TEXT PRIMARY KEY,
  tip_seq INTEGER NOT NULL,
  ctime_ms INTEGER NOT NULL
);
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
  PRIMARY KEY (oid, seq)
);
CREATE TABLE IF NOT EXISTS version_attrs (
  oid TEXT NOT NULL,
  seq INTEGER NOT NULL,
  key TEXT NOT NULL,
  value BLOB NOT NULL,
  PRIMARY KEY (oid, seq, key)
);
CREATE INDEX IF NOT EXISTS idx_object_versions_oid ON object_versions(oid);
CREATE INDEX IF NOT EXISTS idx_version_attrs_oid_seq ON version_attrs(oid, seq);
CREATE INDEX IF NOT EXISTS idx_object_versions_fs_path ON object_versions(fs_path);
)SQL";
  if (!exec_db(db, "PRAGMA foreign_keys = ON;", err)) return false;
  if (!exec_db(db, "PRAGMA journal_mode = WAL;", err)) return false;
  if (!exec_db(db, "PRAGMA synchronous = NORMAL;", err)) return false;
  if (!exec_db(db, ddl, err)) return false;
  // Older versioned DBs may lack redirect_oid (duplicate column errors ignored).
  sqlite3_exec(db, "ALTER TABLE object_versions ADD COLUMN redirect_oid TEXT;", nullptr,
               nullptr, nullptr);
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
  if (sqlite3_open(db_path.c_str(), &shard->db) != SQLITE_OK) {
    err = shard->db ? sqlite3_errmsg(shard->db) : "sqlite3_open failed";
    if (shard->db) sqlite3_close(shard->db);
    return false;
  }
  if (!ensure_schema(shard->db, err)) {
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

bool ObjectStore::write_fs_object(Shard& shard, const std::string& relpath,
                                 const std::uint8_t* data, std::size_t len,
                                 std::string& err) {
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
  if (::fsync(fd) != 0) {
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
  int dir_fd = ::open(final_path.parent_path().c_str(), O_RDONLY);
  if (dir_fd >= 0) {
    if (::fsync(dir_fd) != 0) {
      err = std::string("fsync dir: ") + std::strerror(errno);
      ::close(dir_fd);
      return false;
    }
    ::close(dir_fd);
  }
  return true;
}

bool ObjectStore::remove_fs_object(Shard& shard, const std::string& relpath, std::string& err) {
  if (relpath.empty()) return true;
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
                            const std::uint8_t* data, std::size_t len, std::string& err) {
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
  if (::fsync(fd) != 0) {
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
    const ssize_t n = ::pread(fd, buf, chunk, static_cast<off_t>(offset + done));
    if (n < 0) {
      err = std::string("pread crc: ") + std::strerror(errno);
      ::close(fd);
      return false;
    }
    if (n == 0) {
      err = "short read: file truncated";
      ::close(fd);
      return false;
    }
    crc = crc32c_update(crc, buf, static_cast<std::size_t>(n));
    done += static_cast<std::uint64_t>(n);
  }
  ::close(fd);
  out_crc = crc;
  return true;
}

bool ObjectStore::crc_after_range_update(Shard& shard, const std::string& relpath,
                                        std::uint64_t old_size, std::uint64_t offset,
                                        const std::uint8_t* data, std::size_t len,
                                        std::uint64_t new_size, std::uint32_t& out_crc,
                                        std::string& err) {
  (void)old_size;
  std::uint32_t combined = 0;
  if (offset > 0) {
    std::uint32_t pref = 0;
    if (!crc_file_range(shard, relpath, 0, offset, pref, err)) return false;
    combined = pref;
  }
  if (len > 0) {
    combined = crc32c_combine(combined, crc32c(data, len), len);
  }
  if (offset + static_cast<std::uint64_t>(len) < new_size) {
    std::uint32_t suf = 0;
    const std::uint64_t suf_off = offset + len;
    const std::uint64_t suf_len = new_size - suf_off;
    if (!crc_file_range(shard, relpath, suf_off, suf_len, suf, err)) return false;
    combined = crc32c_combine(combined, suf, static_cast<std::size_t>(suf_len));
  }

  std::uint32_t full = 0;
  if (!crc_file_range(shard, relpath, 0, new_size, full, err)) return false;
  if (combined != full) {
    AIOS_LOG_WARN("crc32c combine mismatch; using full scan");
    out_crc = full;
  } else {
    out_crc = combined;
  }
  return true;
}

bool ObjectStore::tip_seq_locked(Shard& s, const std::string& oid, std::uint64_t& tip,
                                 std::string& err) {
  tip = 0;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(s.db, "SELECT tip_seq FROM object_tips WHERE oid=?1;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    tip = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);
    return true;
  }
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE) return true;
  err = sqlite3_errmsg(s.db);
  return false;
}

bool ObjectStore::next_seq_locked(Shard& s, const std::string& oid, std::uint64_t& seq,
                                  std::string& err) {
  std::uint64_t tip = 0;
  if (!tip_seq_locked(s, oid, tip, err)) return false;
  std::uint64_t max_seq = 0;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(s.db, "SELECT COALESCE(MAX(seq),0) FROM object_versions WHERE oid=?1;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    max_seq = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
  } else if (rc != SQLITE_DONE) {
    err = sqlite3_errmsg(s.db);
    sqlite3_finalize(stmt);
    return false;
  }
  sqlite3_finalize(stmt);
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
                         "(oid,seq,size,inline,fs_path,crc32c,is_delete,ctime_ms,redirect_oid) "
                         "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);",
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
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(s.db,
                         "SELECT size, inline, fs_path, crc32c, is_delete, ctime_ms, "
                         "redirect_oid FROM object_versions WHERE oid=?1 AND seq=?2;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return false;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(seq));
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    sqlite3_finalize(stmt);
    err = "object not found";
    return false;
  }
  if (rc != SQLITE_ROW) {
    err = sqlite3_errmsg(s.db);
    sqlite3_finalize(stmt);
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
  if (!out.fs_path.empty()) out.inline_body = false;
  sqlite3_finalize(stmt);
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
    fs_unlink_out.push_back(info.fs_path);
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

  const std::uint32_t body_crc = crc32c(data, len);
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

  std::unordered_map<std::string, std::string> merged;
  if (replace_attrs) {
    merged = attrs;
  } else if (tip > 0) {
    ObjectInfo tip_info;
    std::string lerr;
    if (load_version_locked(s, oid, tip, tip_info, lerr) && !tip_info.is_delete) {
      if (!load_attrs_for_seq(s.db, oid, tip, merged, err)) {
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
  const auto name = "upload-" + h + "-" + std::to_string(now_ms());
  const fs::path tmp = fs::path(sp->dir) / "tmp" / name;
  std::error_code ec;
  fs::create_directories(tmp.parent_path(), ec);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      err = "cannot create staging file";
      return false;
    }
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
  std::ofstream out(abs_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    err = "cannot truncate staging file";
    return false;
  }
  return true;
}

bool ObjectStore::stage_pwrite(const std::string& abs_path, std::uint64_t offset,
                               const std::uint8_t* data, std::size_t len, std::string& err) {
  FILE* f = std::fopen(abs_path.c_str(), "r+b");
  if (!f) {
    f = std::fopen(abs_path.c_str(), "w+b");
  }
  if (!f) {
    err = "cannot open staging file";
    return false;
  }
  if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0) {
    std::fclose(f);
    err = "seek failed";
    return false;
  }
  if (len > 0 && std::fwrite(data, 1, len, f) != len) {
    std::fclose(f);
    err = "write failed";
    return false;
  }
  std::fflush(f);
  std::fclose(f);
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
  fs::rename(staging_abs_path, final_path, ec);
  if (ec) {
    // Cross-device rename may fail; fall back to copy+remove.
    ec.clear();
    fs::copy_file(staging_abs_path, final_path, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      err = "place staging: " + ec.message();
      return false;
    }
    fs::remove(staging_abs_path, ec);
  }
  {
    FILE* f = std::fopen(final_path.c_str(), "rb");
    if (f) {
      fflush(f);
      fsync(fileno(f));
      std::fclose(f);
    }
  }
  relpath_out = rel;
  return true;
}

bool ObjectStore::prepare_put_file(const std::string& oid, const std::string& staging_abs_path,
                                   std::uint64_t size, std::uint32_t crc32c_val,
                                   const std::unordered_map<std::string, std::string>& attrs,
                                   bool replace_attrs,
                                   std::optional<std::uint32_t> expected_crc32c,
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

  std::unordered_map<std::string, std::string> merged;
  if (replace_attrs) {
    merged = attrs;
  } else if (tip > 0) {
    ObjectInfo tip_info;
    std::string lerr;
    if (load_version_locked(s, oid, tip, tip_info, lerr) && !tip_info.is_delete) {
      if (!load_attrs_for_seq(s.db, oid, tip, merged, err)) {
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
  pv.size = size;
  pv.crc32c = crc32c_val;
  pv.inline_body = false;
  pv.is_delete = false;

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
  Shard& s = *sp;
  std::lock_guard<std::recursive_mutex> guard(s.mu);
  const std::uint64_t end = offset + static_cast<std::uint64_t>(len);

  if (!begin(s, err)) return false;

  std::uint64_t tip = 0;
  if (!tip_seq_locked(s, oid, tip, err)) {
    rollback(s);
    return false;
  }

  std::uint64_t old_size = 0;
  bool tip_live = false;
  ObjectInfo tip_info;
  std::vector<std::uint8_t> tip_inline;
  if (tip > 0) {
    if (!load_version_locked(s, oid, tip, tip_info, err)) {
      rollback(s);
      return false;
    }
    if (!tip_info.is_delete) {
      tip_live = true;
      old_size = tip_info.size;
      if (tip_info.inline_body) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(s.db,
                               "SELECT inline FROM object_versions WHERE oid=?1 AND seq=?2;",
                               -1, &stmt, nullptr) != SQLITE_OK) {
          err = sqlite3_errmsg(s.db);
          rollback(s);
          return false;
        }
        sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(tip));
        if (sqlite3_step(stmt) == SQLITE_ROW) {
          const void* blob = sqlite3_column_blob(stmt, 0);
          const int n = sqlite3_column_bytes(stmt, 0);
          if (n > 0 && blob) {
            tip_inline.assign(reinterpret_cast<const std::uint8_t*>(blob),
                              reinterpret_cast<const std::uint8_t*>(blob) + n);
          }
        }
        sqlite3_finalize(stmt);
      }
    }
  }

  std::unordered_map<std::string, std::string> merged = attrs;
  if (tip_live) {
    if (!replace_attrs) {
      std::unordered_map<std::string, std::string> tip_attrs;
      if (!load_attrs_for_seq(s.db, oid, tip, tip_attrs, err)) {
        rollback(s);
        return false;
      }
      merged = std::move(tip_attrs);
      for (const auto& [k, v] : attrs) merged[k] = v;
    }
  }

  std::uint64_t seq = 0;
  if (!next_seq_locked(s, oid, seq, err)) {
    rollback(s);
    return false;
  }

  const std::string new_rel = version_relpath(oid, seq);
  const fs::path new_abs = fs::path(s.dir) / new_rel;
  std::error_code ec;
  fs::create_directories(new_abs.parent_path(), ec);

  if (tip_live && !tip_info.fs_path.empty()) {
    const fs::path src = fs::path(s.dir) / tip_info.fs_path;
    if (!clone_or_copy_file(src.string(), new_abs.string(), !opts_.clone_required, err)) {
      rollback(s);
      return false;
    }
  } else if (tip_live && tip_info.inline_body) {
    if (!write_fs_object(s, new_rel, tip_inline.data(), tip_inline.size(), err)) {
      rollback(s);
      return false;
    }
  } else {
    // Create from empty (no tip / delete tip).
    if (!ensure_fs_size(s, new_rel, 0, err)) {
      rollback(s);
      return false;
    }
  }

  const std::uint64_t new_size = std::max(old_size, end);
  if (!ensure_fs_size(s, new_rel, new_size, err)) {
    std::string rm_err;
    remove_fs_object(s, new_rel, rm_err);
    rollback(s);
    return false;
  }
  if (len > 0) {
    if (!pwrite_fs(s, new_rel, offset, data, len, err)) {
      std::string rm_err;
      remove_fs_object(s, new_rel, rm_err);
      rollback(s);
      return false;
    }
  }

  std::uint32_t body_crc = 0;
  if (!crc_after_range_update(s, new_rel, old_size, offset, data, len, new_size, body_crc,
                              err)) {
    std::string rm_err;
    remove_fs_object(s, new_rel, rm_err);
    rollback(s);
    return false;
  }

  PreparedVersion pv;
  pv.oid = oid;
  pv.seq = seq;
  pv.prev_tip = tip;
  pv.size = new_size;
  pv.crc32c = body_crc;
  pv.inline_body = false;
  pv.fs_path = new_rel;
  pv.is_delete = false;

  if (!insert_version_locked(s, pv, nullptr, 0, merged, err)) {
    std::string rm_err;
    remove_fs_object(s, new_rel, rm_err);
    rollback(s);
    return false;
  }

  if (!commit(s, err)) {
    std::string rm_err;
    remove_fs_object(s, new_rel, rm_err);
    rollback(s);
    return false;
  }
  out = std::move(pv);
  return true;
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
      if (!load_attrs_for_seq(s.db, oid, tip, merged, err)) {
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
    if (!load_attrs_for_seq(s.db, v.oid, v.seq, existing_attrs, err)) {
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
  } else {
    if (pv.fs_path.empty()) pv.fs_path = version_relpath(pv.oid, pv.seq);
    if (data && len > 0) {
      if (len != pv.size) {
        rollback(s);
        err = "install size mismatch";
        return false;
      }
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
      std::uint32_t got_crc = 0;
      if (!crc_file_range(s, pv.fs_path, 0, pv.size, got_crc, err)) {
        rollback(s);
        return false;
      }
      if (got_crc != pv.crc32c) {
        rollback(s);
        err = "install crc32c mismatch";
        return false;
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
                         "INSERT INTO object_tips(oid, tip_seq, ctime_ms) VALUES(?1,?2,?3) "
                         "ON CONFLICT(oid) DO UPDATE SET tip_seq=excluded.tip_seq, "
                         "ctime_ms=excluded.ctime_ms "
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

  if (!info->fs_path.empty()) {
    std::ifstream in(fs::path(s.dir) / info->fs_path, std::ios::binary);
    if (!in) {
      err = "cannot open fs object: " + info->fs_path;
      return std::nullopt;
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(info->size));
    if (info->size > 0) {
      in.read(reinterpret_cast<char*>(out.data()),
              static_cast<std::streamsize>(info->size));
      if (!in) {
        err = "short read on fs object";
        return std::nullopt;
      }
    }
    return out;
  }

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(s.db, "SELECT inline FROM object_versions WHERE oid=?1 AND seq=?2;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(s.db);
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(info->seq));
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    err = rc == SQLITE_DONE ? "object not found" : sqlite3_errmsg(s.db);
    sqlite3_finalize(stmt);
    return std::nullopt;
  }
  const void* blob = sqlite3_column_blob(stmt, 0);
  const int blob_len = sqlite3_column_bytes(stmt, 0);
  if (static_cast<std::uint64_t>(blob_len) != info->size) {
    err = "inline size mismatch";
    sqlite3_finalize(stmt);
    return std::nullopt;
  }
  std::vector<std::uint8_t> out(static_cast<std::size_t>(blob_len));
  if (blob_len > 0 && blob) {
    std::memcpy(out.data(), blob, static_cast<std::size_t>(blob_len));
  }
  sqlite3_finalize(stmt);
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

  if (info->inline_body) {
    auto full = get(oid, info->seq, err);
    if (!full) return std::nullopt;
    std::vector<std::uint8_t> out(full->begin() + static_cast<std::ptrdiff_t>(offset),
                                  full->begin() + static_cast<std::ptrdiff_t>(offset + want));
    return out;
  }

  Shard* sp = shard_for(oid);
  if (!sp) {
    err = "shard open failed";
    return std::nullopt;
  }
  std::lock_guard<std::recursive_mutex> guard(sp->mu);
  const fs::path path = fs::path(sp->dir) / info->fs_path;
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    err = std::string("open: ") + std::strerror(errno);
    return std::nullopt;
  }
  std::vector<std::uint8_t> out(want);
  std::size_t done = 0;
  while (done < want) {
    const ssize_t n =
        ::pread(fd, out.data() + done, want - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      err = std::string("pread: ") + std::strerror(errno);
      ::close(fd);
      return std::nullopt;
    }
    if (n == 0) break;
    done += static_cast<std::size_t>(n);
  }
  ::close(fd);
  if (done != want) {
    // Short read means the backing file is truncated or was concurrently modified;
    // returning the partial buffer would silently hand truncated data to the client.
    err = "short read: got " + std::to_string(done) + " of " + std::to_string(want) + " bytes";
    return std::nullopt;
  }
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
  if (!load_attrs_for_seq(s.db, oid, tip, attrs, err)) {
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
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(s.db, "SELECT inline FROM object_versions WHERE oid=?1 AND seq=?2;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
      err = sqlite3_errmsg(s.db);
      rollback(s);
      return false;
    }
    sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(tip));
    if (sqlite3_step(stmt) != SQLITE_ROW) {
      err = "tip inline missing";
      sqlite3_finalize(stmt);
      rollback(s);
      return false;
    }
    const void* blob = sqlite3_column_blob(stmt, 0);
    const int n = sqlite3_column_bytes(stmt, 0);
    if (n > 0 && blob) {
      inline_copy.assign(reinterpret_cast<const std::uint8_t*>(blob),
                         reinterpret_cast<const std::uint8_t*>(blob) + n);
    }
    sqlite3_finalize(stmt);
  } else {
    pv.inline_body = false;
    pv.fs_path = version_relpath(oid, seq);
    const fs::path src = fs::path(s.dir) / tip_info.fs_path;
    const fs::path dst = fs::path(s.dir) / pv.fs_path;
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    if (!clone_or_copy_file(src.string(), dst.string(), !opts_.clone_required, err)) {
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
  if (!load_attrs_for_seq(sp->db, oid, info->seq, out, err)) return {};
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
  if (info->size == 0) {
    out_crc = crc32c(nullptr, 0);
  } else if (info->inline_body) {
    auto data = get(oid, info->seq, err);
    if (!data) return false;
    out_crc = crc32c(data->data(), data->size());
  } else if (!crc_file_range(*sp, info->fs_path, 0, info->size, out_crc, err)) {
    return false;
  }

  if (!begin(*sp, err)) return false;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(sp->db,
                         "UPDATE object_versions SET crc32c=?1 WHERE oid=?2 AND seq=?3;", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(sp->db);
    rollback(*sp);
    return false;
  }
  sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(out_crc));
  sqlite3_bind_text(stmt, 2, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(info->seq));
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    err = sqlite3_errmsg(sp->db);
    sqlite3_finalize(stmt);
    rollback(*sp);
    return false;
  }
  sqlite3_finalize(stmt);
  return commit(*sp, err);
}

}  // namespace aios
