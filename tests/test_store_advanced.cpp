#include "test_helpers.hpp"

#include "util/crc32c.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

using aios::test::expect;
using aios::test::failures;

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string body_str(const std::optional<std::vector<std::uint8_t>>& b) {
  if (!b) return {};
  return std::string(b->begin(), b->end());
}

bool seed_legacy_db(const fs::path& db_path, std::string& err) {
  sqlite3* db = nullptr;
  if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
    err = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
    if (db) sqlite3_close(db);
    return false;
  }
  const char* ddl = R"SQL(
CREATE TABLE objects (
  oid TEXT PRIMARY KEY,
  size INTEGER NOT NULL,
  inline BLOB,
  fs_path TEXT,
  ctime_ms INTEGER NOT NULL,
  mtime_ms INTEGER NOT NULL,
  crc32c INTEGER
);
CREATE TABLE attrs (
  oid TEXT NOT NULL,
  key TEXT NOT NULL,
  value BLOB NOT NULL,
  PRIMARY KEY (oid, key)
);
)SQL";
  char* errmsg = nullptr;
  if (sqlite3_exec(db, ddl, nullptr, nullptr, &errmsg) != SQLITE_OK) {
    err = errmsg ? errmsg : "legacy ddl failed";
    sqlite3_free(errmsg);
    sqlite3_close(db);
    return false;
  }
  sqlite3_close(db);
  return true;
}

bool insert_legacy_inline(const fs::path& db_path, const std::string& oid,
                          const std::string& body, const std::string& attr_key,
                          const std::string& attr_val, std::string& err) {
  sqlite3* db = nullptr;
  if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
    err = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
    if (db) sqlite3_close(db);
    return false;
  }
  const auto crc = aios::crc32c(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
  const auto ts = now_ms();
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO objects(oid,size,inline,fs_path,ctime_ms,mtime_ms,crc32c) "
                         "VALUES(?1,?2,?3,NULL,?4,?4,?5);",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(db);
    sqlite3_close(db);
    return false;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(body.size()));
  sqlite3_bind_blob(stmt, 3, body.data(), static_cast<int>(body.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, ts);
  sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(crc));
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    err = sqlite3_errmsg(db);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return false;
  }
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(db, "INSERT INTO attrs(oid,key,value) VALUES(?1,?2,?3);", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(db);
    sqlite3_close(db);
    return false;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, attr_key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 3, attr_val.data(), static_cast<int>(attr_val.size()),
                    SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    err = sqlite3_errmsg(db);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return false;
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return true;
}

bool insert_legacy_fs(const fs::path& db_path, const std::string& oid,
                      const std::string& fs_path, std::uint64_t size, std::uint32_t crc,
                      std::string& err) {
  sqlite3* db = nullptr;
  if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
    err = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
    if (db) sqlite3_close(db);
    return false;
  }
  const auto ts = now_ms();
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO objects(oid,size,inline,fs_path,ctime_ms,mtime_ms,crc32c) "
                         "VALUES(?1,?2,NULL,?3,?4,?4,?5);",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    err = sqlite3_errmsg(db);
    sqlite3_close(db);
    return false;
  }
  sqlite3_bind_text(stmt, 1, oid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(size));
  sqlite3_bind_text(stmt, 3, fs_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, ts);
  sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(crc));
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    err = sqlite3_errmsg(db);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return false;
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return true;
}

}  // namespace

int test_store_advanced() {
  using namespace aios;
  using aios::test::default_opts;
  using aios::test::temp_root;

  failures() = 0;
  std::string err;

  // -------------------------------------------------------------------------
  // 1. purge_version
  // -------------------------------------------------------------------------
  {
    const auto root = temp_root("aios-adv-purge");
    ObjectStore store;
    auto opts = default_opts();
    opts.max_versions = 32;
    expect(store.open(root.string(), opts, err), "purge: open");
    expect(store.put("purged", std::string("v1"), {}, true, err), "purge: put1");
    expect(store.put("purged", std::string("v2"), {}, true, err), "purge: put2");
    expect(store.put("purged", std::string("v3"), {}, true, err), "purge: put3");
    auto tip = store.stat("purged", err);
    expect(tip && tip->seq == 3, "purge: tip seq3");

    expect(store.purge_version("purged", 2, false, err), "purge: middle seq");
    expect(store.get("purged", 2, err) == std::nullopt, "purge: seq2 gone");
    expect(body_str(store.get("purged", 1, err)) == "v1", "purge: seq1 intact");
    expect(body_str(store.get("purged", err)) == "v3", "purge: tip body intact");

    expect(!store.purge_version("purged", 3, false, err), "purge: tip reject allow_tip=false");
    expect(err == "cannot purge tip", "purge: tip reject message");
    tip = store.stat("purged", err);
    expect(tip && tip->seq == 3, "purge: tip still 3 after reject");

    expect(store.purge_version("purged", 3, true, err), "purge: tip with allow_tip");
    expect(store.get("purged", err) == std::nullopt, "purge: get fails after tip cleared");
    expect(store.stat("purged", err) == std::nullopt, "purge: tip stat gone");
  }

  // -------------------------------------------------------------------------
  // 2. trim_versions
  // -------------------------------------------------------------------------
  {
    const auto root = temp_root("aios-adv-trim");
    ObjectStore store;
    auto opts = default_opts();
    opts.max_versions = 64;
    expect(store.open(root.string(), opts, err), "trim: open");
    for (int i = 1; i <= 5; ++i) {
      expect(store.put("trimme", std::string("b") + std::to_string(i), {}, true, err),
             "trim: put " + std::to_string(i));
    }
    auto vers = store.list_versions("trimme", err);
    expect(vers.size() == 5, "trim: 5 versions before");
    expect(store.trim_versions("trimme", 2, err), "trim: trim_versions keep 2");
    vers = store.list_versions("trimme", err);
    expect(vers.size() == 2, "trim: list_versions size==2");
    auto tip = store.stat("trimme", err);
    expect(tip && tip->seq == 5, "trim: tip retained");
    expect(body_str(store.get("trimme", err)) == "b5", "trim: tip body");
    expect(store.get("trimme", 1, err) == std::nullopt, "trim: old seq purged");
    expect(store.get("trimme", 3, err) == std::nullopt, "trim: mid seq purged");
  }

  // -------------------------------------------------------------------------
  // 3. install_version
  // -------------------------------------------------------------------------
  {
    const auto root_a = temp_root("aios-adv-inst-a");
    ObjectStore store_a;
    auto opts = default_opts();
    expect(store_a.open(root_a.string(), opts, err), "install: open A");
    expect(store_a.put("base", std::string("aaa"), {}, true, err), "install: base put");
    auto tip_before = store_a.stat("base", err);
    expect(tip_before && tip_before->seq == 1, "install: tip before prepare");

    PreparedVersion prep;
    expect(store_a.prepare_put("base", reinterpret_cast<const std::uint8_t*>("bbb"), 3, {},
                               true, std::nullopt, prep, err),
           "install: prepare_put on A");
    auto tip_after = store_a.stat("base", err);
    expect(tip_after && tip_after->seq == 1, "install: tip unchanged after prepare");
    expect(body_str(store_a.get("base", err)) == "aaa", "install: tip body unchanged");

    const auto root_b = temp_root("aios-adv-inst-b");
    ObjectStore store_b;
    expect(store_b.open(root_b.string(), opts, err), "install: open B");

    const std::string payload = "hello";
    const auto payload_crc =
        crc32c(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    PreparedVersion crafted;
    crafted.oid = "fresh-oid";
    crafted.seq = 1;
    crafted.prev_tip = 0;
    crafted.size = payload.size();
    crafted.crc32c = payload_crc;
    crafted.inline_body = true;
    crafted.is_delete = false;

    expect(store_b.install_version(crafted,
                                   reinterpret_cast<const std::uint8_t*>(payload.data()),
                                   payload.size(), {{"src", "crafted"}}, err),
           "install: first install");
    expect(store_b.install_version(crafted,
                                   reinterpret_cast<const std::uint8_t*>(payload.data()),
                                   payload.size(), {{"src", "crafted"}}, err),
           "install: retry idempotent");

    PreparedVersion conflict = crafted;
    conflict.size = payload.size() + 1;
    expect(!store_b.install_version(conflict,
                                    reinterpret_cast<const std::uint8_t*>(payload.data()),
                                    payload.size(), {}, err),
           "install: conflicting size fails");
    expect(err == "version already exists", "install: conflict message");
  }

  // -------------------------------------------------------------------------
  // 4. prepare/abort/publish for put_range, delete, redirect
  // -------------------------------------------------------------------------
  {
    const auto root = temp_root("aios-adv-txn");
    ObjectStore store;
    auto opts = default_opts();
    opts.clone_required = false;
    expect(store.open(root.string(), opts, err), "txn: open");
    expect(store.put("range-o", std::string("abcdefghij"), {}, true, err), "txn: seed range");

    // put_range
    {
      auto tip = store.stat("range-o", err);
      expect(tip && tip->seq == 1, "txn: range tip1");
      PreparedVersion pv;
      const char* patch = "ZZ";
      expect(store.prepare_put_range("range-o", 2,
                                     reinterpret_cast<const std::uint8_t*>(patch), 2, {}, false,
                                     pv, err),
             "txn: prepare_put_range");
      tip = store.stat("range-o", err);
      expect(tip && tip->seq == 1, "txn: tip unchanged after prepare_put_range");
      expect(store.abort_version("range-o", pv.seq, err), "txn: abort put_range");
      expect(store.get("range-o", pv.seq, err) == std::nullopt, "txn: aborted range seq gone");
      tip = store.stat("range-o", err);
      expect(tip && tip->seq == 1, "txn: tip still 1 after abort range");

      expect(store.prepare_put_range("range-o", 2,
                                     reinterpret_cast<const std::uint8_t*>(patch), 2, {}, false,
                                     pv, err),
             "txn: prepare_put_range again");
      expect(store.publish_tip("range-o", pv.seq, err), "txn: publish put_range");
      tip = store.stat("range-o", err);
      expect(tip && tip->seq == pv.seq, "txn: tip advanced after publish range");
      auto body = store.get("range-o", err);
      expect(body && body->size() >= 4 && (*body)[2] == 'Z' && (*body)[3] == 'Z',
             "txn: range patch applied");
    }

    // delete
    {
      expect(store.put("del-o", std::string("alive"), {}, true, err), "txn: seed delete");
      auto tip = store.stat("del-o", err);
      const auto tip_seq = tip ? tip->seq : 0;
      expect(tip_seq > 0, "txn: del tip exists");
      PreparedVersion pv;
      expect(store.prepare_delete("del-o", pv, err), "txn: prepare_delete");
      tip = store.stat("del-o", err);
      expect(tip && tip->seq == tip_seq, "txn: tip unchanged after prepare_delete");
      expect(store.abort_version("del-o", pv.seq, err), "txn: abort delete");
      expect(store.get("del-o", pv.seq, err) == std::nullopt, "txn: aborted delete seq gone");
      tip = store.stat("del-o", err);
      expect(tip && tip->seq == tip_seq, "txn: tip after abort delete");

      expect(store.prepare_delete("del-o", pv, err), "txn: prepare_delete again");
      expect(store.publish_tip("del-o", pv.seq, err), "txn: publish delete");
      expect(store.stat("del-o", err) == std::nullopt, "txn: tip hidden after delete publish");
      auto st = store.stat("del-o", pv.seq, err);
      expect(st && st->is_delete, "txn: delete marker via seq");
    }

    // redirect
    {
      expect(store.put("redir-tgt", std::string("target"), {}, true, err), "txn: redirect target");
      PreparedVersion pv;
      expect(store.prepare_redirect("redir-a", "redir-tgt", {}, true, pv, err),
             "txn: prepare_redirect");
      expect(store.stat("redir-a", err) == std::nullopt, "txn: tip unchanged (no tip) after prep");
      expect(store.abort_version("redir-a", pv.seq, err), "txn: abort redirect");
      expect(store.get("redir-a", pv.seq, err) == std::nullopt, "txn: aborted redirect gone");

      expect(store.prepare_redirect("redir-a", "redir-tgt", {}, true, pv, err),
             "txn: prepare_redirect again");
      expect(store.publish_tip("redir-a", pv.seq, err), "txn: publish redirect");
      auto st = store.stat("redir-a", err);
      expect(st && st->redirect_oid == "redir-tgt", "txn: tip advanced to redirect");
      expect(st && st->seq == pv.seq, "txn: redirect tip seq");
    }
  }

  // -------------------------------------------------------------------------
  // 5. attrs
  // -------------------------------------------------------------------------
  {
    const auto root = temp_root("aios-adv-attrs");
    ObjectStore store;
    expect(store.open(root.string(), default_opts(), err), "attrs: open");
    expect(store.put("attr-o", std::string("body"), {{"a", "1"}, {"b", "2"}}, true, err),
           "attrs: put with attrs");
    auto a = store.get_attr("attr-o", "a", err);
    expect(a && *a == "1", "attrs: get_attr a");
    auto listed = store.list_attrs("attr-o", err);
    expect(listed.size() == 2 && listed["a"] == "1" && listed["b"] == "2", "attrs: list_attrs");

    expect(store.put("attr-o", std::string("body2"), {{"c", "3"}}, false, err),
           "attrs: put merge replace_attrs=false");
    listed = store.list_attrs("attr-o", err);
    expect(listed.count("a") && listed.count("b") && listed["c"] == "3",
           "attrs: merged attrs retained");
    auto tip = store.stat("attr-o", err);
    expect(tip && tip->seq == 2, "attrs: seq after merge put");

    expect(store.set_attr("attr-o", "d", "4", err), "attrs: set_attr");
    tip = store.stat("attr-o", err);
    expect(tip && tip->seq == 3, "attrs: set_attr creates new version");
    auto d = store.get_attr("attr-o", "d", err);
    expect(d && *d == "4", "attrs: get_attr d");
    listed = store.list_attrs("attr-o", err);
    expect(listed.count("a") && listed.count("d"), "attrs: list after set_attr");
  }

  // -------------------------------------------------------------------------
  // 6. preconditions
  // -------------------------------------------------------------------------
  {
    const auto root = temp_root("aios-adv-pred");
    ObjectStore store;
    expect(store.open(root.string(), default_opts(), err), "pred: open");
    expect(store.put("pred-o", std::string("x"), {{"color", "red"}, {"n", "1"}}, true, err),
           "pred: put");

    using K = AttrPrecondition::Kind;
    expect(store.check_preconditions("pred-o", {{K::MustExist, {}, {}}}, err) ==
               PrecondResult::Ok,
           "pred: MustExist ok");
    expect(store.check_preconditions("missing", {{K::MustExist, {}, {}}}, err) ==
               PrecondResult::NotFound,
           "pred: MustExist missing");
    expect(store.check_preconditions("missing", {{K::MustNotExist, {}, {}}}, err) ==
               PrecondResult::Ok,
           "pred: MustNotExist ok");
    expect(store.check_preconditions("pred-o", {{K::MustNotExist, {}, {}}}, err) ==
               PrecondResult::Conflict,
           "pred: MustNotExist conflict");

    expect(store.check_preconditions("pred-o", {{K::Eq, "color", "red"}}, err) ==
               PrecondResult::Ok,
           "pred: Eq ok");
    expect(store.check_preconditions("pred-o", {{K::Eq, "color", "blue"}}, err) ==
               PrecondResult::Conflict,
           "pred: Eq conflict");
    expect(store.check_preconditions("pred-o", {{K::Ne, "color", "blue"}}, err) ==
               PrecondResult::Ok,
           "pred: Ne ok");
    expect(store.check_preconditions("pred-o", {{K::Ne, "color", "red"}}, err) ==
               PrecondResult::Conflict,
           "pred: Ne conflict");

    expect(store.check_preconditions("pred-o", {{K::Present, "color", {}}}, err) ==
               PrecondResult::Ok,
           "pred: Present ok");
    expect(store.check_preconditions("pred-o", {{K::Present, "nope", {}}}, err) ==
               PrecondResult::Conflict,
           "pred: Present conflict");
    expect(store.check_preconditions("pred-o", {{K::Absent, "nope", {}}}, err) ==
               PrecondResult::Ok,
           "pred: Absent ok when key missing");
    expect(store.check_preconditions("pred-o", {{K::Absent, "color", {}}}, err) ==
               PrecondResult::Conflict,
           "pred: Absent conflict when key present");
    expect(store.check_preconditions("ghost", {{K::Absent, "any", {}}}, err) == PrecondResult::Ok,
           "pred: Absent when object missing is Ok");
  }

  // -------------------------------------------------------------------------
  // 7. list
  // -------------------------------------------------------------------------
  {
    const auto root = temp_root("aios-adv-list");
    ObjectStore store;
    expect(store.open(root.string(), default_opts(), err), "list: open");
    expect(store.put("list/a", std::string("1"), {{"env", "prod"}}, true, err), "list: put a");
    expect(store.put("list/b", std::string("2"), {{"env", "dev"}}, true, err), "list: put b");
    expect(store.put("list/c", std::string("3"), {{"env", "prod"}}, true, err), "list: put c");
    expect(store.put("other/x", std::string("4"), {{"env", "prod"}}, true, err), "list: put other");

    auto lst = store.list("list/", "", "", 100, "", false, err);
    expect(lst.objects.size() == 3, "list: prefix count");

    lst = store.list("list/", "env", "prod", 100, "", false, err);
    expect(lst.objects.size() == 2, "list: attr_eq filter");
    for (const auto& e : lst.objects) {
      expect(e.oid == "list/a" || e.oid == "list/c", "list: filtered oid");
    }

    lst = store.list("list/", "env", "prod", 100, "", true, err);
    expect(!lst.objects.empty() && lst.objects[0].attrs.count("env"),
           "list: include_attrs");

    lst = store.list("list/", "", "", 1, "", false, err);
    expect(lst.objects.size() == 1, "list: limit=1");
    expect(!lst.next_cursor.empty(), "list: next_cursor set");
    auto page2 = store.list("list/", "", "", 1, lst.next_cursor, false, err);
    expect(page2.objects.size() == 1, "list: page2 size");
    expect(page2.objects[0].oid != lst.objects[0].oid, "list: page2 different oid");

    expect(store.put("list/gone", std::string("z"), {}, true, err), "list: put delete candidate");
    expect(store.del("list/gone", err), "list: del marker");
    lst = store.list("list/", "", "", 100, "", false, err);
    bool found_gone = false;
    for (const auto& e : lst.objects) {
      if (e.oid == "list/gone") found_gone = true;
    }
    expect(!found_gone, "list: delete marker excluded");

    expect(store.put("list-tgt", std::string("t"), {}, true, err), "list: redirect target");
    expect(store.put_redirect("list/alias", "list-tgt", {}, true, nullptr, err),
           "list: put_redirect");
    lst = store.list("list/", "", "", 100, "", false, err);
    bool found_redir = false;
    for (const auto& e : lst.objects) {
      if (e.oid == "list/alias") {
        found_redir = true;
        expect(e.redirect_oid == "list-tgt", "list: redirect_oid in entry");
      }
    }
    expect(found_redir, "list: redirect appears");
  }

  // -------------------------------------------------------------------------
  // 8. scrub_orphans + list_oids + fs_body_path
  // -------------------------------------------------------------------------
  {
    const auto root = temp_root("aios-adv-scrub");
    ObjectStore store;
    auto opts = default_opts();
    opts.force_mode = "fs";
    opts.clone_required = false;
    expect(store.open(root.string(), opts, err), "scrub: open");
    expect(store.put("fs-obj", std::string("filesystem-body"), {}, true, err), "scrub: put fs");

    auto path = store.fs_body_path("fs-obj", err);
    expect(path.has_value() && fs::is_regular_file(*path), "scrub: fs_body_path");

    auto oids = store.list_oids(100, err);
    expect(!oids.empty(), "scrub: list_oids nonempty");
    bool has_fs = false;
    for (const auto& o : oids) {
      if (o == "fs-obj") has_fs = true;
    }
    expect(has_fs, "scrub: list_oids contains fs-obj");

    auto tip = store.stat("fs-obj", err);
    expect(tip.has_value(), "scrub: tip");
    char shard_buf[8];
    std::snprintf(shard_buf, sizeof(shard_buf), "%x", tip->shard);
    const auto orphan =
        fs::path(store.root()) / "shards" / shard_buf / "objects" / "zz" / "yy" / "orphan.bin";
    fs::create_directories(orphan.parent_path());
    {
      std::ofstream out(orphan, std::ios::binary);
      out << "orphan-data";
    }
    expect(fs::is_regular_file(orphan), "scrub: orphan created");
    const auto removed = store.scrub_orphans(err);
    expect(removed >= 1, "scrub: removed >= 1");
    expect(!fs::exists(orphan), "scrub: orphan gone");
    expect(fs::is_regular_file(*path), "scrub: real body kept");
  }

  // -------------------------------------------------------------------------
  // 9. legacy migrate
  // -------------------------------------------------------------------------
  {
    const auto root = temp_root("aios-adv-legacy");
    fs::create_directories(root);
    {
      std::ofstream out(root / "store.json");
      out << R"({"version":1,"shard_count":4,"inline_max_bytes":256})" << '\n';
    }

    const std::uint32_t shard = shard_of_oid("legacy-1", 4);
    expect(shard < 4, "legacy: shard in range");
    char shard_dir_name[8];
    std::snprintf(shard_dir_name, sizeof(shard_dir_name), "%x", shard);
    const auto shard_dir = root / "shards" / shard_dir_name;
    fs::create_directories(shard_dir / "objects" / "aa" / "bb");
    const auto db_path = shard_dir / "meta.sqlite";
    expect(seed_legacy_db(db_path, err), "legacy: seed schema");

    const std::string inline_body = "oldbody";
    expect(insert_legacy_inline(db_path, "legacy-1", inline_body, "k", "v", err),
           "legacy: insert inline");

    // FS object: prefer same shard when possible; otherwise create its own shard DB.
    const std::string fs_oid = "legacy-fs";
    const std::uint32_t fs_shard = shard_of_oid(fs_oid, 4);
    char fs_shard_name[8];
    std::snprintf(fs_shard_name, sizeof(fs_shard_name), "%x", fs_shard);
    const auto fs_shard_dir = root / "shards" / fs_shard_name;
    fs::create_directories(fs_shard_dir / "objects" / "aa" / "bb");
    const auto fs_db = fs_shard_dir / "meta.sqlite";
    if (fs_shard != shard) {
      expect(seed_legacy_db(fs_db, err), "legacy: seed fs shard schema");
    }
    const std::string fs_rel = "objects/aa/bb/deadbeef";
    const std::string fs_body = "fslegacy";
    const auto fs_crc =
        crc32c(reinterpret_cast<const std::uint8_t*>(fs_body.data()), fs_body.size());
    {
      std::ofstream out(fs_shard_dir / fs_rel, std::ios::binary);
      out << fs_body;
    }
    expect(insert_legacy_fs(fs_shard == shard ? db_path : fs_db, fs_oid, fs_rel, fs_body.size(),
                            fs_crc, err),
           "legacy: insert fs object");

    ObjectStore store;
    auto opts = default_opts();
    opts.shard_count = 4;
    expect(store.open(root.string(), opts, err), "legacy: open migrates");
    if (!err.empty() && !store.is_open()) {
      std::cerr << "legacy open err: " << err << "\n";
    }

    auto body = store.get("legacy-1", err);
    expect(body && body_str(body) == "oldbody", "legacy: get body");
    auto st = store.stat("legacy-1", err);
    expect(st && st->seq == 1, "legacy: seq==1");
    expect(st && st->size == inline_body.size(), "legacy: size");
    auto attr = store.get_attr("legacy-1", "k", err);
    expect(attr && *attr == "v", "legacy: attr k=v");
    auto attrs = store.list_attrs("legacy-1", err);
    expect(attrs.count("k") && attrs["k"] == "v", "legacy: list_attrs");

    auto fs_got = store.get(fs_oid, err);
    expect(fs_got && body_str(fs_got) == fs_body, "legacy: fs object get");
    auto fs_st = store.stat(fs_oid, err);
    expect(fs_st && fs_st->seq == 1 && !fs_st->inline_body, "legacy: fs seq==1");
  }

  // -------------------------------------------------------------------------
  // 10. empty put / zero-length
  // -------------------------------------------------------------------------
  {
    const auto root = temp_root("aios-adv-empty");
    ObjectStore store;
    expect(store.open(root.string(), default_opts(), err), "empty: open");
    expect(store.put("empty-o", std::string(""), {}, true, err), "empty: put");
    auto st = store.stat("empty-o", err);
    expect(st && st->size == 0, "empty: size 0");
    auto body = store.get("empty-o", err);
    expect(body.has_value() && body->empty(), "empty: get empty object");
  }

  // -------------------------------------------------------------------------
  // 11. redirect install via prepare_redirect + abort + publish
  // -------------------------------------------------------------------------
  {
    const auto root = temp_root("aios-adv-redir");
    ObjectStore store;
    expect(store.open(root.string(), default_opts(), err), "redir: open");
    expect(store.put("r-target", std::string("payload"), {}, true, err), "redir: target");

    PreparedVersion pv;
    expect(store.prepare_redirect("r-alias", "r-target", {{"kind", "alias"}}, true, pv, err),
           "redir: prepare");
    expect(store.stat("r-alias", err) == std::nullopt, "redir: tip unchanged after prepare");
    expect(store.abort_version("r-alias", pv.seq, err), "redir: abort");
    expect(store.stat("r-alias", pv.seq, err) == std::nullopt, "redir: aborted seq gone");

    expect(store.prepare_redirect("r-alias", "r-target", {{"kind", "alias"}}, true, pv, err),
           "redir: prepare again");
    expect(store.publish_tip("r-alias", pv.seq, err), "redir: publish");
    auto st = store.stat("r-alias", err);
    expect(st && st->redirect_oid == "r-target", "redir: published redirect_oid");
    expect(st && st->seq == pv.seq, "redir: published seq");
    expect(store.get("r-alias", err) == std::nullopt, "redir: get fails");
    expect(err == "object is redirect", "redir: get err");
  }

  // -------------------------------------------------------------------------
  // 12. versioned get of delete marker
  // -------------------------------------------------------------------------
  {
    const auto root = temp_root("aios-adv-delmark");
    ObjectStore store;
    expect(store.open(root.string(), default_opts(), err), "delmark: open");
    expect(store.put("dm", std::string("live"), {}, true, err), "delmark: put");
    auto before = store.stat("dm", err);
    expect(before && before->seq == 1, "delmark: seq1");
    expect(store.del("dm", err), "delmark: del");
    expect(store.stat("dm", err) == std::nullopt, "delmark: tip hidden");
    auto vers = store.list_versions("dm", err);
    expect(!vers.empty() && vers[0].is_delete, "delmark: newest is delete");
    const auto del_seq = vers[0].seq;
    auto st = store.stat("dm", del_seq, err);
    expect(st && st->is_delete, "delmark: versioned stat is_delete");
    expect(store.get("dm", del_seq, err) == std::nullopt, "delmark: get delete marker fails");
    expect(err == "object is delete marker", "delmark: get err");
  }

  // -------------------------------------------------------------------------
  // 13. prepare_put_file / staging stream path
  // -------------------------------------------------------------------------
  {
    const auto root = temp_root("aios-adv-stage");
    ObjectStore store;
    ObjectStoreOptions opts = default_opts();
    opts.force_mode = "fs";
    expect(store.open(root.string(), opts, err), "stage: open");
    std::string staging;
    expect(store.create_staging_file("big", staging, err), "stage: create");
    std::vector<std::uint8_t> payload(300 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i);
    {
      std::ofstream out(staging, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(payload.data()),
                static_cast<std::streamsize>(payload.size()));
    }
    const auto crc = aios::crc32c(payload.data(), payload.size());
    PreparedVersion pv;
    expect(store.prepare_put_file("big", staging, payload.size(), crc, {{"k", "v"}}, true,
                                  crc, pv, err),
           "stage: prepare_put_file");
    expect(store.publish_tip("big", pv.seq, err), "stage: publish");
    auto got = store.get("big", err);
    expect(got && *got == payload, "stage: get matches");
    auto path = store.fs_body_path("big", err);
    expect(path.has_value(), "stage: fs path");
  }

  return failures();
}
