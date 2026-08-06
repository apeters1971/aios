// Regression tests for the findings in CODE_REVIEW.md. Each case fails against
// the pre-fix code and names the hazard (P*/E*/C*/U*) it pins down.
// Kernel findings (K*) are not covered here — they need a Linux kernel build.
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "client/session.hpp"
#include "client/stl.hpp"
#include "cluster/place.hpp"
#include "ec/codec_factory.hpp"
#include "ec/ec_attrs.hpp"
#include "ec/xor_parity.hpp"
#include "http/http_server.hpp"
#include "object/repair.hpp"
#include "posix/aios_posix.h"
#include "posix/quota_ledger.hpp"
#include "store/object_store.hpp"
#include "util/auth.hpp"
#include "util/base64.hpp"
#include "util/compression.hpp"
#include "util/crc32c.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <errno.h>
#include <fstream>
#include <limits>
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

  explicit HttpFixture(const char* prefix, int base_port = 19600)
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

void expect_codec_length_guards(aios::ErasureCodec& codec, const char* label) {
  std::string err;
  const std::string payload = "abcdefghijklmnop";  // 16 bytes
  std::vector<std::vector<std::uint8_t>> shards;
  ASSERT_TRUE(codec.encode(std::span<const std::uint8_t>(
                               reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()),
                           shards, err))
      << label << " encode: " << err;

  // E3: shard length must match ceil(full_size / k).
  {
    std::vector<std::optional<std::vector<std::uint8_t>>> in(shards.size());
    for (std::size_t i = 0; i < shards.size(); ++i) in[i] = shards[i];
    in[0]->push_back('x');  // wrong length
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(codec.decode(in, payload.size(), out, err)) << label << " E3 must reject";
    EXPECT_NE(err.find("shard length"), std::string::npos) << label << " E3 err=" << err;
  }

  // E6: a present-but-empty shard must not be treated as "unset" / shard_len=0.
  {
    std::vector<std::optional<std::vector<std::uint8_t>>> in(shards.size());
    in[0] = std::vector<std::uint8_t>{};  // present but empty
    for (std::size_t i = 1; i < shards.size(); ++i) in[i] = shards[i];
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(codec.decode(in, payload.size(), out, err)) << label << " E6 must reject";
    EXPECT_TRUE(err.find("shard length mismatch") != std::string::npos ||
                err.find("does not match full_size") != std::string::npos)
        << label << " E6 err=" << err;
  }

  // All shards present and empty with nonzero full_size → length vs full_size guard.
  {
    std::vector<std::optional<std::vector<std::uint8_t>>> in(shards.size());
    for (auto& slot : in) slot = std::vector<std::uint8_t>{};
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(codec.decode(in, payload.size(), out, err)) << label << " empty present";
    EXPECT_NE(err.find("does not match full_size"), std::string::npos) << label << " err=" << err;
  }
}

bool rewrite_tip_attrs(aios::ObjectStore* store, const std::string& oid,
                       const std::unordered_map<std::string, std::string>& attrs) {
  std::string err;
  auto st = store->stat(oid, err);
  if (!st) return false;
  auto body = store->get(oid, err);
  if (!body) return false;
  aios::PreparedVersion pv;
  pv.oid = oid;
  pv.seq = st->seq;
  pv.size = st->size;
  pv.crc32c = st->crc32c;
  pv.inline_body = st->inline_body;
  pv.fs_path = st->fs_path;
  pv.is_delete = false;
  return store->install_version(pv, body->data(), body->size(), attrs, err);
}

}  // namespace

// ---------------------------------------------------------------------------
// U1 — attacker-controlled full_size must not crash the daemon
// ---------------------------------------------------------------------------

TEST(CodeReviewU1, ZstdDecompressRejectsHugeLogicalSize) {
  using namespace aios;
  if (!zstd_available()) GTEST_SKIP() << "no libzstd";

  std::vector<std::uint8_t> plain(64, 'A');
  std::vector<std::uint8_t> compressed;
  std::string err;
  ASSERT_TRUE(zstd_compress(plain.data(), plain.size(), 3, compressed, err)) << err;

  std::vector<std::uint8_t> out;
  EXPECT_FALSE(zstd_decompress(compressed.data(), compressed.size(),
                               std::numeric_limits<std::uint64_t>::max(), out, err));
  EXPECT_EQ(err, "logical size too large");
  EXPECT_TRUE(out.empty());

  // Service path: poison tip attrs after a legitimate compressed put.
  DualStoreFixture fx("aios-cr-u1");
  fx.cfg.compression = "zstd";
  fx.cfg.compression_level = 3;
  fx.cfg.compression_min_bytes = 16;
  fx.cfg.max_object_bytes = 1024 * 1024;
  fx.svc = std::make_unique<ObjectService>(fx.cfg, fx.map, fx.stores);
  fx.svc->set_advertise("127.0.0.1:7400");

  const std::string oid = "u1/blob";
  auto put = fx.svc->api_put(oid, plain.data(), plain.size(), {}, true, {});
  ASSERT_TRUE(put.ok) << put.error;
  ASSERT_TRUE(attrs_are_compressed(put.attrs));

  auto pl = place(oid, fx.map, fx.cfg.replica_count, fx.cfg.default_storage_class);
  ASSERT_FALSE(pl.acting_set.empty());
  for (const auto& t : pl.acting_set) {
    auto* store = fx.stores.get(t.aios_path);
    ASSERT_NE(store, nullptr);
    auto attrs = store->list_attrs(oid, err);
    attrs[kCompAttrFullSize] = std::to_string(fx.cfg.max_object_bytes + 1);
    ASSERT_TRUE(rewrite_tip_attrs(store, oid, attrs)) << "rewrite full_size";
  }

  auto got = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  EXPECT_FALSE(got.ok) << "poisoned full_size must not decompress";
  EXPECT_NE(got.error.find("max_object_bytes"), std::string::npos)
      << "err=" << got.error << " code=" << got.code;
}

// ---------------------------------------------------------------------------
// E2 / E3 / E6 — codec length guards (XOR always; ISA-L when built)
// ---------------------------------------------------------------------------

TEST(CodeReviewE3E6, XorCodecRejectsWrongAndEmptyShardLengths) {
  using namespace aios;
  std::string err;
  auto codec = make_xor_parity_codec(2, err);
  ASSERT_NE(codec, nullptr) << err;
  expect_codec_length_guards(*codec, "xor");
}

TEST(CodeReviewE2E3E6, IsalCodecRejectsWrongLengths) {
  using namespace aios;
  if (!isal_ec_available()) GTEST_SKIP() << "ISA-L not built";

  std::string err;
  auto codec = make_erasure_codec(2, 2, "isal", err);
  ASSERT_NE(codec, nullptr) << err;
  expect_codec_length_guards(*codec, "isal");
  // E2 (shard_len > INT_MAX) is guarded in isal_rs encode/decode but needs a
  // multi-GiB allocation to exercise from a unit test; covered by code review.
}

// ---------------------------------------------------------------------------
// E7 — repair must not decode mixed generations / missing full_crc
// ---------------------------------------------------------------------------

TEST(CodeReviewE7, RepairAbortsWithoutFullCrc) {
  using namespace aios;
  EcFixture fx("aios-cr-e7");
  const std::string oid = "e7/nocrc";
  std::string body;
  body.reserve(4096);
  for (int i = 0; i < 4096; ++i) body.push_back(static_cast<char>('a' + (i % 26)));

  auto put = fx.svc->api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                             {}, true, {});
  ASSERT_TRUE(put.ok) << put.error;

  auto pl = place(oid, fx.map, fx.cfg.replica_count, fx.cfg.default_storage_class);
  ASSERT_GE(pl.acting_set.size(), 3u);

  std::string err;
  for (const auto& t : pl.acting_set) {
    auto* store = fx.stores.get(t.aios_path);
    if (!store || !store->stat(oid, err)) continue;
    auto attrs = store->list_attrs(oid, err);
    attrs.erase(kEcAttrFullCrc);
    ASSERT_TRUE(rewrite_tip_attrs(store, oid, attrs));
  }

  auto* victim = fx.stores.get(pl.acting_set[2].aios_path);
  ASSERT_NE(victim, nullptr);
  auto st = victim->stat(oid, err);
  ASSERT_TRUE(st.has_value());
  ASSERT_TRUE(victim->purge_version(oid, st->seq, true, err)) << err;

  const auto before = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  ASSERT_TRUE(before.ok && before.data) << before.error;
  EXPECT_EQ(std::string(before.data->begin(), before.data->end()), body);

  auto stats = run_repair(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 256);
  EXPECT_EQ(stats.repaired, 0u) << "must not rewrite shards without full_crc";
  EXPECT_GE(stats.failed + stats.under_replicated, 1u);

  // Healthy shards untouched — degraded get still returns the original body.
  auto after = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  ASSERT_TRUE(after.ok && after.data) << after.error;
  EXPECT_EQ(std::string(after.data->begin(), after.data->end()), body);
}

// ---------------------------------------------------------------------------
// E4 / E10 / E12 — store staging, install CRC, tip monotonicity
// ---------------------------------------------------------------------------

TEST(CodeReviewE4E10E12, StagingInstallCrcAndTipMonotonic) {
  using namespace aios;
  std::string err;
  const auto root = temp_root("aios-cr-store");
  ObjectStore store;
  auto opts = default_opts();
  opts.force_mode = "fs";
  opts.inline_max_bytes = 0;
  ASSERT_TRUE(store.open(root.string(), opts, err)) << err;

  // E4: staging paths for different oids at the same seq must not collide.
  std::string p_a, p_b;
  ASSERT_TRUE(store.stage_path_for("oid-a", 1, p_a, err)) << err;
  ASSERT_TRUE(store.stage_path_for("oid-b", 1, p_b, err)) << err;
  EXPECT_NE(p_a, p_b);
  EXPECT_NE(p_a.find("1"), std::string::npos);

  // Seed tip seq=1, then prepare+publish seq=2.
  const std::string oid = "tip/obj";
  const std::string v1 = "version-one-body";
  ASSERT_TRUE(store.put(oid, v1, {}, true, err)) << err;
  std::uint64_t tip = 0;
  ASSERT_TRUE(store.tip_seq(oid, tip, err)) << err;
  EXPECT_EQ(tip, 1u);

  PreparedVersion prep;
  const std::string v2 = "version-two-body!!";
  ASSERT_TRUE(store.prepare_put(oid, reinterpret_cast<const std::uint8_t*>(v2.data()), v2.size(),
                                {}, true, std::nullopt, prep, err))
      << err;
  ASSERT_TRUE(store.publish_tip(oid, prep.seq, err)) << err;
  ASSERT_TRUE(store.tip_seq(oid, tip, err));
  EXPECT_EQ(tip, prep.seq);
  EXPECT_GT(tip, 1u);

  // E12: publishing an older tip must fail and leave tip unchanged.
  EXPECT_FALSE(store.publish_tip(oid, 1, err));
  EXPECT_EQ(err, "tip seq regression");
  std::uint64_t tip2 = 0;
  ASSERT_TRUE(store.tip_seq(oid, tip2, err));
  EXPECT_EQ(tip2, tip);

  // E10: install refuses a staged fs body whose CRC does not match the claim.
  const std::string fresh = "install/crc";
  std::string staging;
  ASSERT_TRUE(store.stage_path_for(fresh, 1, staging, err)) << err;
  {
    std::ofstream out(staging, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good());
    out << "good-bytes";
  }
  PreparedVersion bad;
  bad.oid = fresh;
  bad.seq = 1;
  bad.prev_tip = 0;
  bad.size = 10;
  bad.crc32c = 0xdeadbeef;  // wrong
  bad.inline_body = false;
  bad.fs_path.clear();
  // Place staging into the version path the install expects.
  std::string rel;
  ASSERT_TRUE(store.place_staging_as_version(fresh, 1, staging, rel, err)) << err;
  bad.fs_path = rel;
  EXPECT_FALSE(store.install_version(bad, nullptr, 0, {}, err));
  EXPECT_EQ(err, "install crc32c mismatch");
}

// ---------------------------------------------------------------------------
// P1 — quota ledger persists usage via aios.posix.cas
// ---------------------------------------------------------------------------

TEST(CodeReviewP1, QuotaLedgerFlushAdvancesPosixCas) {
  using namespace aios;
  using namespace aios::posix;
  HttpFixture http("aios-cr-p1", 19750);

  Session sess(http.session_cfg());
  QuotaLedger ledger(sess, "default");
  ledger.note_delta(/*project*/ 0, /*uid*/ 42, /*gid*/ 7, /*delta*/ 1234);
  ledger.flush();

  auto snap = sess.get_object(quota_usage_oid("default"));
  ASSERT_TRUE(snap.exists);
  auto it = snap.attrs.find("aios.posix.cas");
  ASSERT_TRUE(it != snap.attrs.end()) << "usage object must carry aios.posix.cas";
  const auto cas1 = std::stoull(it->second);
  EXPECT_GE(cas1, 1u);

  // Second flush with another delta must not stick on expected_cas=0.
  ledger.note_delta(0, 42, 7, 10);
  ledger.flush();
  auto snap2 = sess.get_object(quota_usage_oid("default"));
  ASSERT_TRUE(snap2.exists);
  const auto cas2 = std::stoull(snap2.attrs.at("aios.posix.cas"));
  EXPECT_GT(cas2, cas1);

  auto usage = parse_quota_usage(snap2.body, cas2);
  EXPECT_EQ(usage.volume_uids[42], 1244);
}

// ---------------------------------------------------------------------------
// P2 / P3 — concurrent chunk RMW + inode size reapply
// ---------------------------------------------------------------------------

TEST(CodeReviewP2P3, ConcurrentDisjointChunkWritesPreserveBothRegions) {
  HttpFixture http("aios-cr-p2", 19800);

  aios_posix_config cfg{};
  const std::string ep = http.endpoint();
  cfg.endpoint = ep.c_str();
  cfg.cluster_key = http.fx.cfg.cluster_key.c_str();
  cfg.volume = "p2vol";
  cfg.stripe_unit = 4096;
  cfg.stripe_width = 1;
  cfg.uid = 1000;
  cfg.gid = 1000;

  int err = 0;
  aios_posix_fs* fs = aios_posix_mount(&cfg, &err);
  ASSERT_NE(fs, nullptr) << err;

  aios_posix_stat st{};
  ASSERT_EQ(aios_posix_create(fs, 1, "chunky", 0644, &st), 0);
  const uint64_t ino = st.ino;

  const std::string left(2048, 'L');
  const std::string right(2048, 'R');
  std::atomic<int> rc_left{1};
  std::atomic<int> rc_right{1};
  std::thread t1([&] {
    size_t wrote = 0;
    rc_left = aios_posix_write(fs, ino, 0, left.data(), left.size(), &wrote);
  });
  std::thread t2([&] {
    size_t wrote = 0;
    rc_right = aios_posix_write(fs, ino, 2048, right.data(), right.size(), &wrote);
  });
  t1.join();
  t2.join();
  EXPECT_EQ(rc_left.load(), 0);
  EXPECT_EQ(rc_right.load(), 0);

  // P3: size must cover both writers (max end), not last-writer-wins shrink.
  aios_posix_stat gst{};
  ASSERT_EQ(aios_posix_getattr(fs, ino, &gst), 0);
  EXPECT_EQ(gst.size, 4096u);

  std::vector<char> buf(4096);
  size_t got = 0;
  ASSERT_EQ(aios_posix_read(fs, ino, 0, buf.data(), buf.size(), &got), 0);
  ASSERT_EQ(got, 4096u);
  EXPECT_EQ(std::string(buf.data(), 2048), left);
  EXPECT_EQ(std::string(buf.data() + 2048, 2048), right);

  aios_posix_unmount(fs);
}

// ---------------------------------------------------------------------------
// P8 / P11 / P12 — readdir cookies, name length, same-dir rename
// ---------------------------------------------------------------------------

TEST(CodeReviewP8P11P12, ReaddirCookiesNameLimitAndSameDirRename) {
  HttpFixture http("aios-cr-p8", 19850);

  aios_posix_config cfg{};
  const std::string ep = http.endpoint();
  cfg.endpoint = ep.c_str();
  cfg.cluster_key = http.fx.cfg.cluster_key.c_str();
  cfg.volume = "p8vol";
  cfg.stripe_unit = 4096;
  cfg.uid = 1000;
  cfg.gid = 1000;

  int err = 0;
  aios_posix_fs* fs = aios_posix_mount(&cfg, &err);
  ASSERT_NE(fs, nullptr);

  aios_posix_stat st{};
  // P11
  const std::string ok_name(255, 'n');
  const std::string bad_name(256, 'n');
  EXPECT_EQ(aios_posix_create(fs, 1, ok_name.c_str(), 0644, &st), 0);
  EXPECT_EQ(aios_posix_create(fs, 1, bad_name.c_str(), 0644, &st), -ENAMETOOLONG);

  for (int i = 0; i < 8; ++i) {
    ASSERT_EQ(aios_posix_mkdir(fs, 1, ("d" + std::to_string(i)).c_str(), 0755, &st), 0);
  }

  // P8: one-entry batches must advance the cookie each time (no duplicate/skip).
  std::vector<std::string> names;
  uint64_t off = 0;
  std::vector<uint64_t> cookies;
  for (;;) {
    aios_posix_dirent dent{};
    const int n = aios_posix_readdir(fs, 1, &off, &dent, 1);
    ASSERT_GE(n, 0);
    if (n == 0) break;
    names.emplace_back(dent.name);
    cookies.push_back(off);
  }
  ASSERT_FALSE(cookies.empty());
  for (std::size_t i = 1; i < cookies.size(); ++i) {
    EXPECT_GT(cookies[i], cookies[i - 1]) << "readdir cookies must be strictly increasing";
  }
  EXPECT_NE(std::find(names.begin(), names.end(), ok_name), names.end());

  // P12: same-dir rename over existing destination.
  ASSERT_EQ(aios_posix_create(fs, 1, "src", 0644, &st), 0);
  const uint64_t src_ino = st.ino;
  ASSERT_EQ(aios_posix_create(fs, 1, "dst", 0644, &st), 0);
  ASSERT_EQ(aios_posix_rename(fs, 1, "src", 1, "dst"), 0);
  EXPECT_EQ(aios_posix_lookup(fs, 1, "src", &st), -ENOENT);
  ASSERT_EQ(aios_posix_lookup(fs, 1, "dst", &st), 0);
  EXPECT_EQ(st.ino, src_ino);

  aios_posix_unmount(fs);
}

// ---------------------------------------------------------------------------
// C9 / C12 — CRLF attrs rejected; list set_at persists
// ---------------------------------------------------------------------------

TEST(CodeReviewC9, AttributeValuesRejectCRLF) {
  using namespace aios;
  HttpFixture http("aios-cr-c9", 19900);
  Session s(http.session_cfg());
  bool threw = false;
  try {
    s.put_bytes("c9/obj", "body", {{"k", "v\r\nX-Evil: 1"}});
  } catch (const client_error& e) {
    threw = true;
    EXPECT_EQ(e.code(), "bad_request");
    EXPECT_NE(e.what(), nullptr);
    EXPECT_NE(std::string(e.what()).find("invalid characters"), std::string::npos);
  }
  EXPECT_TRUE(threw);
}

TEST(CodeReviewC12, ListSetAtPersistsToPeer) {
  using namespace aios;
  HttpFixture http("aios-cr-c12", 19950);
  Session a(http.session_cfg());
  Session b(http.session_cfg());

  list l1(a, "c12list", sync_mode::async, false);
  l1.push_back("old");
  l1.flush();
  l1.set_at(0, "new");
  l1.flush();

  list l2(b, "c12list", sync_mode::sync, false);
  EXPECT_EQ(l2.size(), 1u);
  EXPECT_EQ(l2.at(0), "new");
}

// ---------------------------------------------------------------------------
// U3 / U5 — auth replay + base64 padding
// ---------------------------------------------------------------------------

TEST(CodeReviewU3, AuthVerifyRejectsReplayWithinSkewWindow) {
  using namespace aios;
  const std::string key = "550e8400-e29b-41d4-a716-446655440000";
  nlohmann::json body = {{"node_id", "n1"}, {"listen", "127.0.0.1:7400"}};
  auth_sign(body, MsgType::Hello, key);
  ASSERT_TRUE(body.contains("nonce"));

  std::string err;
  EXPECT_TRUE(auth_verify(body, MsgType::Hello, key, 60000, err)) << err;
  EXPECT_FALSE(auth_verify(body, MsgType::Hello, key, 60000, err));
  EXPECT_EQ(err, "replay");

  // A freshly signed message (new nonce) still verifies.
  auth_sign(body, MsgType::Hello, key);
  EXPECT_TRUE(auth_verify(body, MsgType::Hello, key, 60000, err)) << err;
}

TEST(CodeReviewU5, Base64RejectsMalformedPadding) {
  using namespace aios;
  std::vector<std::uint8_t> out;
  std::string err;
  EXPECT_FALSE(base64_decode("AB=C", out, err));
  EXPECT_EQ(err, "invalid base64 padding");
  EXPECT_FALSE(base64_decode("ABC", out, err));
  EXPECT_EQ(err, "base64 length not multiple of 4");
  EXPECT_TRUE(base64_decode("YQ==", out, err));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], 'a');
}
