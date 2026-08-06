// Second wave of CODE_REVIEW regression tests — client changelog/session edges,
// store concurrency/unlink/CRC, and remaining posix barriers.
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "client/changelog.hpp"
#include "client/mutex.hpp"
#include "client/session.hpp"
#include "client/stl.hpp"
#include "client/wire.hpp"
#include "cluster/place.hpp"
#include "ec/ec_attrs.hpp"
#include "http/http_server.hpp"
#include "object/repair.hpp"
#include "posix/aios_posix.h"
#include "store/object_store.hpp"
#include "util/crc32c.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <errno.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using aios::test::DualStoreFixture;
using aios::test::EcFixture;
using aios::test::default_opts;
using aios::test::temp_root;

struct HttpFixture {
  DualStoreFixture fx;
  int port_num;
  std::string host{"127.0.0.1"};
  std::string port;
  boost::asio::io_context ioc;
  std::unique_ptr<aios::HttpServer> http;
  std::thread th;

  explicit HttpFixture(const char* prefix, int base_port)
      : fx(prefix, 2, 2, "nvme"), port_num(base_port + static_cast<int>(::getpid() % 200)) {
    port = std::to_string(port_num);
    fx.cfg.http_listen = host + ":" + port;
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership);
    http->start();
    th = std::thread([this] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }

  ~HttpFixture() {
    ioc.stop();
    if (th.joinable()) th.join();
  }

  std::string endpoint() const { return host + ":" + port; }
  aios::SessionConfig session_cfg() const {
    return aios::SessionConfig{.endpoint = endpoint(), .cluster_key = fx.cfg.cluster_key};
  }
};

struct MountedFs {
  std::string ep;
  std::string key;
  std::string volume;
  aios_posix_fs* fs{nullptr};

  MountedFs(HttpFixture& http, const char* vol, uint64_t stripe = 4096) : volume(vol) {
    ep = http.endpoint();
    key = http.fx.cfg.cluster_key;
    aios_posix_config cfg{};
    cfg.endpoint = ep.c_str();
    cfg.cluster_key = key.c_str();
    cfg.volume = volume.c_str();
    cfg.stripe_unit = stripe;
    cfg.stripe_width = 1;
    cfg.uid = 1000;
    cfg.gid = 1000;
    int err = 0;
    fs = aios_posix_mount(&cfg, &err);
    EXPECT_NE(fs, nullptr) << "mount err=" << err;
  }

  ~MountedFs() {
    if (fs) aios_posix_unmount(fs);
  }

  MountedFs(const MountedFs&) = delete;
  MountedFs& operator=(const MountedFs&) = delete;
};

}  // namespace

// ---------------------------------------------------------------------------
// C2 — async flush must pull peer ops before advancing the cursor
// ---------------------------------------------------------------------------

TEST(CodeReviewC2, AsyncFlushPullsPeerOpsBeforeAdvancingCursor) {
  using namespace aios;
  HttpFixture http("aios-crm-c2", 20200);
  Session a(http.session_cfg());
  Session b(http.session_cfg());

  map local(a, "c2map", sync_mode::async, false);
  local.set("local", "1");  // pending only

  map peer(b, "c2map", sync_mode::sync, false);
  peer.set("remote", "2");

  local.flush();

  map check(a, "c2map", sync_mode::sync, false);
  EXPECT_EQ(check.at("local"), "1");
  EXPECT_EQ(check.at("remote"), "2");
}

// ---------------------------------------------------------------------------
// C3 / C10 — out-of-order apply + invalid op_id framing
// ---------------------------------------------------------------------------

TEST(CodeReviewC3, OutOfOrderLogRecordsAppliedByOpId) {
  using namespace aios;
  using namespace aios::changelog;
  HttpFixture http("aios-crm-c3", 20250);
  Session s(http.session_cfg());

  Record late{2, Op::Put, {"b", "2"}};
  Record early{1, Op::Put, {"a", "1"}};
  const std::string log_body = encode_record(late) + encode_record(early);

  const std::string name = "ooo";
  nlohmann::json meta{{"aios_stl", kMetaVersion},
                      {"type", "map"},
                      {"mode_hint", "async"},
                      {"next_op", 3},
                      {"log_bytes", log_body.size()},
                      {"snapshot_op", 0},
                      {"snapshot_oid", snap_oid("map", name)}};
  s.put_object(meta_oid("map", name), meta.dump(), "map", 0);
  s.put_bytes(log_oid("map", name), log_body);

  map m(s, name, sync_mode::sync, false);
  EXPECT_EQ(m.at("a"), "1");
  EXPECT_EQ(m.at("b"), "2");
}

TEST(CodeReviewC10, ChangelogRejectsInvalidOpIdAndBadHeader) {
  using namespace aios;
  using namespace aios::changelog;

  Record zero{0, Op::Put, {"k", "v"}};
  std::vector<Record> out;
  EXPECT_THROW(
      {
        try {
          decode_records(encode_record(zero), out);
        } catch (const client_error& e) {
          EXPECT_EQ(e.code(), "bad_request");
          EXPECT_NE(std::string(e.what()).find("invalid op_id"), std::string::npos);
          throw;
        }
      },
      client_error);

  // Truncated framing: magic+header_len present, payload incomplete → partial consume.
  Record ok{1, Op::Clear, {}};
  auto framed = encode_record(ok);
  framed.resize(framed.size() - 1);
  out.clear();
  const auto n = decode_records(framed, out);
  EXPECT_EQ(n, 0u);
  EXPECT_TRUE(out.empty());

  // Via Log::pull: op_id >= next_op must throw (not wedge applied_op at max).
  HttpFixture http("aios-crm-c10", 20300);
  Session s(http.session_cfg());
  Record huge{100, Op::Put, {"x", "y"}};
  const std::string body = encode_record(huge);
  nlohmann::json meta{{"aios_stl", kMetaVersion},
                      {"type", "map"},
                      {"mode_hint", "async"},
                      {"next_op", 2},
                      {"log_bytes", body.size()},
                      {"snapshot_op", 0},
                      {"snapshot_oid", snap_oid("map", "wedge")}};
  s.put_object(meta_oid("map", "wedge"), meta.dump(), "map", 0);
  s.put_bytes(log_oid("map", "wedge"), body);

  Log log(s, "map", "wedge");
  std::uint64_t applied = 0;
  EXPECT_THROW(
      {
        try {
          log.pull(
              &applied, [](const std::string&) {}, [](const Record&) {});
        } catch (const client_error& e) {
          EXPECT_NE(std::string(e.what()).find("invalid op_id"), std::string::npos);
          throw;
        }
      },
      client_error);
  EXPECT_EQ(applied, 0u);
}

// ---------------------------------------------------------------------------
// C11 — malformed JSON surfaces as client_error
// ---------------------------------------------------------------------------

TEST(CodeReviewC11, MalformedJsonThrowsClientErrorNotNlohmann) {
  using namespace aios;
  try {
    wire::parse_json("{");
    FAIL() << "expected throw";
  } catch (const client_error& e) {
    EXPECT_EQ(e.code(), "bad_request");
  } catch (const nlohmann::json::exception&) {
    FAIL() << "nlohmann exception must not escape";
  }

  try {
    wire::parse_map_doc(R"({"aios_stl":1,"type":"map","entries":[[1,2]]})");
    FAIL() << "expected throw";
  } catch (const client_error& e) {
    EXPECT_EQ(e.code(), "bad_request");
  } catch (const nlohmann::json::exception&) {
    FAIL() << "nlohmann exception must not escape";
  }
}

// ---------------------------------------------------------------------------
// C4 — mutex renew + unlock noexcept
// ---------------------------------------------------------------------------

TEST(CodeReviewC4, MutexRenewAndUnlockNoexcept) {
  using namespace aios;
  HttpFixture http("aios-crm-c4", 20350);
  Session a(http.session_cfg());
  Session b(http.session_cfg());

  mutex m1(a, "c4lock", /*ttl_ms=*/2000);
  ASSERT_TRUE(m1.try_lock());
  EXPECT_TRUE(m1.owns_lock());
  m1.renew();
  EXPECT_TRUE(m1.owns_lock());

  mutex m2(b, "c4lock", 2000);
  EXPECT_FALSE(m2.try_lock());
  EXPECT_THROW(m2.lock(50), client_error);

  m1.unlock();
  m1.unlock();  // must not terminate
  EXPECT_FALSE(m1.owns_lock());
  ASSERT_TRUE(m2.try_lock());
  m2.unlock();
}

// ---------------------------------------------------------------------------
// C1 — compact under lock does not drop concurrent successful appends
// ---------------------------------------------------------------------------

TEST(CodeReviewC1, CompactPreservesFlushedKeys) {
  using namespace aios;
  HttpFixture http("aios-crm-c1", 20400);
  Session s(http.session_cfg());

  // Compact takes the log lock before truncate; a snapshot of already-applied
  // keys must survive. (Concurrent append-during-compact remains a harder race;
  // map::compact still builds the snapshot before acquiring the lock.)
  map m(s, "c1keys", sync_mode::sync, false);
  for (int i = 0; i < 20; ++i) m.set("k" + std::to_string(i), "v");
  m.compact();
  m.set("after", "1");
  m.compact();

  map check(s, "c1keys", sync_mode::sync, false);
  for (int i = 0; i < 20; ++i) {
    EXPECT_TRUE(check.contains("k" + std::to_string(i))) << "missing k" << i;
  }
  EXPECT_EQ(check.at("after"), "1");
}

// ---------------------------------------------------------------------------
// E7′ — repair skips wrong-seq shards when full_crc is present
// ---------------------------------------------------------------------------

TEST(CodeReviewE7b, RepairSkipsWrongSeqShardsWithFullCrc) {
  using namespace aios;
  EcFixture fx("aios-crm-e7b");
  const std::string oid = "e7b/mix";
  std::string body_a;
  std::string body_b;
  body_a.reserve(4096);
  body_b.reserve(4096);
  for (int i = 0; i < 4096; ++i) {
    body_a.push_back(static_cast<char>('a' + (i % 26)));
    body_b.push_back(static_cast<char>('A' + (i % 26)));
  }

  ASSERT_TRUE(fx.svc
                  ->api_put(oid, reinterpret_cast<const std::uint8_t*>(body_a.data()), body_a.size(),
                            {}, true, {})
                  .ok);
  ASSERT_TRUE(fx.svc
                  ->api_put(oid, reinterpret_cast<const std::uint8_t*>(body_b.data()), body_b.size(),
                            {}, true, {})
                  .ok);

  auto pl = place(oid, fx.map, fx.cfg.replica_count, fx.cfg.default_storage_class);
  ASSERT_GE(pl.acting_set.size(), 3u);

  std::string err;
  auto* stale = fx.stores.get(pl.acting_set[1].aios_path);
  ASSERT_NE(stale, nullptr);
  auto tip = stale->stat(oid, err);
  ASSERT_TRUE(tip.has_value());
  const auto auth_seq = tip->seq;
  ASSERT_GT(auth_seq, 1u);
  // Drop tip so tip_seq=0, then republish the previous generation.
  ASSERT_TRUE(stale->purge_version(oid, auth_seq, true, err)) << err;
  ASSERT_TRUE(stale->publish_tip(oid, auth_seq - 1, err)) << err;
  auto stale_tip = stale->stat(oid, err);
  ASSERT_TRUE(stale_tip.has_value());
  EXPECT_EQ(stale_tip->seq, auth_seq - 1);

  auto* missing = fx.stores.get(pl.acting_set[2].aios_path);
  ASSERT_NE(missing, nullptr);
  auto mst = missing->stat(oid, err);
  ASSERT_TRUE(mst.has_value());
  ASSERT_TRUE(missing->purge_version(oid, mst->seq, true, err)) << err;

  auto stats = run_repair(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 256);
  auto got = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  if (got.ok && got.data) {
    EXPECT_EQ(std::string(got.data->begin(), got.data->end()), body_b)
        << "must not decode mixed generations (repaired=" << stats.repaired
        << " failed=" << stats.failed << ")";
  } else {
    // Refusing a mixed-generation reconstruct is correct; never return body_a.
    EXPECT_NE(got.error.find("crc"), std::string::npos) << got.error;
  }
}

// ---------------------------------------------------------------------------
// E5 / E8 / E11 — store concurrency, deferred unlink, short CRC read
// ---------------------------------------------------------------------------

TEST(CodeReviewE5, ConcurrentPutsOnSharedShardSucceed) {
  using namespace aios;
  std::string err;
  const auto root = temp_root("aios-crm-e5");
  ObjectStore store;
  auto opts = default_opts();
  opts.shard_count = 1;
  ASSERT_TRUE(store.open(root.string(), opts, err)) << err;

  constexpr int kThreads = 8;
  constexpr int kPerThread = 20;
  std::atomic<int> fails{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string oid = "e5/" + std::to_string(t) + "/" + std::to_string(i);
        const std::string body = "payload-" + oid;
        std::string local_err;
        if (!store.put(oid, body, {}, true, local_err)) fails.fetch_add(1);
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(fails.load(), 0);

  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPerThread; ++i) {
      const std::string oid = "e5/" + std::to_string(t) + "/" + std::to_string(i);
      auto got = store.get(oid, err);
      ASSERT_TRUE(got.has_value()) << oid << " " << err;
      EXPECT_EQ(std::string(got->begin(), got->end()), "payload-" + oid);
    }
  }
}

TEST(CodeReviewE8, TrimmedFsBodyUnlinkedAfterNewerTip) {
  using namespace aios;
  std::string err;
  const auto root = temp_root("aios-crm-e8");
  ObjectStore store;
  auto opts = default_opts();
  opts.force_mode = "fs";
  opts.inline_max_bytes = 0;
  opts.max_versions = 1;
  ASSERT_TRUE(store.open(root.string(), opts, err)) << err;

  const std::string oid = "e8/obj";
  const std::string v1(512, '1');
  const std::string v2(512, '2');
  ASSERT_TRUE(store.put(oid, v1, {}, true, err)) << err;
  auto path1 = store.fs_body_path(oid, err);
  ASSERT_TRUE(path1.has_value());
  EXPECT_TRUE(std::filesystem::exists(*path1));

  ASSERT_TRUE(store.put(oid, v2, {}, true, err)) << err;
  auto path2 = store.fs_body_path(oid, err);
  ASSERT_TRUE(path2.has_value());
  EXPECT_TRUE(std::filesystem::exists(*path2));
  EXPECT_FALSE(std::filesystem::exists(*path1)) << "old fs body must be unlinked after trim";

  auto got = store.get(oid, err);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(std::string(got->begin(), got->end()), v2);
}

TEST(CodeReviewE11, RecomputeCrcFailsOnTruncatedFsBody) {
  using namespace aios;
  std::string err;
  const auto root = temp_root("aios-crm-e11");
  ObjectStore store;
  auto opts = default_opts();
  opts.force_mode = "fs";
  opts.inline_max_bytes = 0;
  ASSERT_TRUE(store.open(root.string(), opts, err)) << err;

  const std::string oid = "e11/obj";
  const std::string body(1024, 'Z');
  ASSERT_TRUE(store.put(oid, body, {}, true, err)) << err;
  auto path = store.fs_body_path(oid, err);
  ASSERT_TRUE(path.has_value());
  ASSERT_TRUE(std::ofstream(*path, std::ios::binary | std::ios::trunc).good());

  std::uint32_t crc = 0;
  EXPECT_FALSE(store.recompute_crc32c(oid, crc, err));
  EXPECT_NE(err.find("short read"), std::string::npos) << err;
}

// ---------------------------------------------------------------------------
// P5 / P6 / P7 / P10 — posix exception barrier, cross-dir GC, freeze, create race
// ---------------------------------------------------------------------------

TEST(CodeReviewP5, CorruptInodeJsonReturnsEio) {
  HttpFixture http("aios-crm-p5", 20450);
  uint64_t ino = 0;
  {
    MountedFs m(http, "p5vol");
    ASSERT_NE(m.fs, nullptr);
    aios_posix_stat st{};
    ASSERT_EQ(aios_posix_create(m.fs, 1, "f", 0644, &st), 0);
    ino = st.ino;
  }

  aios::Session s(http.session_cfg());
  const std::string oid = "posix/p5vol/ino/" + std::to_string(ino);
  s.put_bytes(oid, "not-json{");
  auto poisoned = s.get_object(oid);
  ASSERT_TRUE(poisoned.exists);
  ASSERT_EQ(poisoned.body, "not-json{");

  // Remount so the inode cache cannot mask the corrupt tip.
  MountedFs m(http, "p5vol");
  ASSERT_NE(m.fs, nullptr);
  aios_posix_stat st{};
  EXPECT_EQ(aios_posix_getattr(m.fs, ino, &st), -EIO);
}

TEST(CodeReviewP7, FrozenSuperRejectsMutationsWithEbusy) {
  HttpFixture http("aios-crm-p7", 20500);
  {
    MountedFs m(http, "p7vol");
    ASSERT_NE(m.fs, nullptr);
  }

  aios::Session s(http.session_cfg());
  auto snap = s.get_object("posix/p7vol/super");
  ASSERT_TRUE(snap.exists);
  auto j = nlohmann::json::parse(snap.body);
  j["frozen"] = true;
  std::uint64_t cas = 0;
  if (auto it = snap.attrs.find("aios.posix.cas"); it != snap.attrs.end()) {
    cas = std::stoull(it->second);
  }
  s.put_bytes("posix/p7vol/super", j.dump(), {}, cas);

  MountedFs m(http, "p7vol");
  ASSERT_NE(m.fs, nullptr);
  aios_posix_stat st{};
  EXPECT_EQ(aios_posix_create(m.fs, 1, "x", 0644, &st), -EBUSY);
}

TEST(CodeReviewP6, CrossDirRenameOverVictimRemovesVictim) {
  HttpFixture http("aios-crm-p6", 20550);
  MountedFs m(http, "p6vol");
  ASSERT_NE(m.fs, nullptr);

  aios_posix_stat st{};
  ASSERT_EQ(aios_posix_mkdir(m.fs, 1, "a", 0755, &st), 0);
  const uint64_t dira = st.ino;
  ASSERT_EQ(aios_posix_mkdir(m.fs, 1, "b", 0755, &st), 0);
  const uint64_t dirb = st.ino;

  ASSERT_EQ(aios_posix_create(m.fs, dira, "src", 0644, &st), 0);
  const uint64_t src_ino = st.ino;
  ASSERT_EQ(aios_posix_create(m.fs, dirb, "victim", 0644, &st), 0);
  const uint64_t victim_ino = st.ino;

  const std::string payload(8192, 'V');
  size_t wrote = 0;
  ASSERT_EQ(aios_posix_write(m.fs, victim_ino, 0, payload.data(), payload.size(), &wrote), 0);
  ASSERT_EQ(aios_posix_write(m.fs, src_ino, 0, "src", 3, &wrote), 0);

  ASSERT_EQ(aios_posix_rename(m.fs, dira, "src", dirb, "victim"), 0);
  EXPECT_EQ(aios_posix_lookup(m.fs, dira, "src", &st), -ENOENT);
  ASSERT_EQ(aios_posix_lookup(m.fs, dirb, "victim", &st), 0);
  EXPECT_EQ(st.ino, src_ino);
  EXPECT_EQ(aios_posix_getattr(m.fs, victim_ino, &st), -ENOENT);

  aios::Session s(http.session_cfg());
  auto chunk = s.get_object("posix/p6vol/data/" + std::to_string(victim_ino) + "/c/0");
  EXPECT_FALSE(chunk.exists) << "victim chunk leaked";
}

TEST(CodeReviewP10, ConcurrentCreateSameNameOneWins) {
  HttpFixture http("aios-crm-p10", 20600);
  MountedFs m(http, "p10vol");
  ASSERT_NE(m.fs, nullptr);

  std::atomic<int> ok{0};
  std::atomic<int> exist{0};
  std::atomic<int> other{0};
  std::uint64_t inos[2]{0, 0};
  std::thread t0([&] {
    aios_posix_stat st{};
    const int rc = aios_posix_create(m.fs, 1, "same", 0644, &st);
    if (rc == 0) {
      ok.fetch_add(1);
      inos[0] = st.ino;
    } else if (rc == -EEXIST) {
      exist.fetch_add(1);
    } else {
      other.fetch_add(1);
    }
  });
  std::thread t1([&] {
    aios_posix_stat st{};
    const int rc = aios_posix_create(m.fs, 1, "same", 0644, &st);
    if (rc == 0) {
      ok.fetch_add(1);
      inos[1] = st.ino;
    } else if (rc == -EEXIST) {
      exist.fetch_add(1);
    } else {
      other.fetch_add(1);
    }
  });
  t0.join();
  t1.join();

  EXPECT_EQ(ok.load() + exist.load(), 2);
  EXPECT_EQ(other.load(), 0);

  aios_posix_stat st{};
  ASSERT_EQ(aios_posix_lookup(m.fs, 1, "same", &st), 0);
  const uint64_t winner = st.ino;
  EXPECT_TRUE(winner == inos[0] || winner == inos[1]);

  // Directory must settle on a single dentry (no duplicate names).
  uint64_t off = 0;
  int named = 0;
  for (;;) {
    aios_posix_dirent dent{};
    const int n = aios_posix_readdir(m.fs, 1, &off, &dent, 1);
    ASSERT_GE(n, 0);
    if (n == 0) break;
    if (std::string(dent.name) == "same") ++named;
  }
  EXPECT_EQ(named, 1);
}
