// Regression tests for the 2026-09-02 code review (store slice).
#include <gtest/gtest.h>

#include "cluster/cluster_map.hpp"
#include "cluster/place.hpp"
#include "fs/aios_scan.hpp"
#include "fs/fs_table.hpp"
#include "object/archive_bag.hpp"
#include "object/archive_pack.hpp"
#include "object/archive_tape.hpp"
#include "object/backup.hpp"
#include "object/object_io.hpp"
#include "store/local_stores.hpp"
#include "store/object_store.hpp"
#include "test_helpers.hpp"
#include "util/compression.hpp"
#include "util/crc32c.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace aios;
using namespace aios::test;
namespace fs = std::filesystem;

std::vector<std::uint8_t> bytes_of(const std::string& s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

bool put_str(ObjectService& svc, const std::string& oid, const std::string& body) {
  LayoutRequest req;
  return svc.api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), {},
                     true, {}, std::nullopt, req)
      .ok;
}

void put_u32(std::string& out, std::uint32_t v) {
  out.append(reinterpret_cast<const char*>(&v), 4);
}
void put_u64(std::string& out, std::uint64_t v) {
  out.append(reinterpret_cast<const char*>(&v), 8);
}
void put_le64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}

// One POSIX dir-changelog record (same framing backup.cpp decodes).
std::string dir_log_record(std::uint64_t op_id, std::uint32_t op,
                           const std::vector<std::string>& args) {
  std::string payload;
  for (const auto& a : args) {
    put_u32(payload, static_cast<std::uint32_t>(a.size()));
    payload += a;
  }
  std::string rec;
  put_u32(rec, 0x6b504f41u);
  put_u32(rec, 16);
  put_u64(rec, op_id);
  put_u32(rec, op);
  put_u32(rec, static_cast<std::uint32_t>(payload.size()));
  rec += payload;
  return rec;
}

ObjectStore* primary_store_for(DualStoreFixture& fx, const std::string& oid) {
  auto p = place(oid, fx.map, fx.cfg.replica_count, "nvme");
  if (p.acting_set.empty()) return nullptr;
  return fx.stores.get(p.acting_set[0].aios_path);
}

}  // namespace

// ---------------------------------------------------------------- STO-1

TEST(Review2Store, TapeSinkParametersComeFromRuleNotAttrs) {
  const auto root = temp_root("r2s-tape-rule");
  const auto tape_root = root / "tape";
  const auto evil_root = root / "evil";
  fs::create_directories(tape_root);

  ArchiveRule rule;
  rule.tape_sink = "external";
  rule.tape_root = tape_root.string();

  // Attacker-controlled attrs: a different sink, root, and a binary that always fails.
  std::unordered_map<std::string, std::string> attrs;
  attrs[kTapeSinkAttr] = "s3";
  attrs[kTapeRootAttr] = evil_root.string();
  attrs[kTapeBinAttr] = "/bin/false";
  attrs[kTapeUriPrefixAttr] = "s3://evil/";
  attrs[kTapeS3EndpointAttr] = "http://127.0.0.1:1";

  const auto body = bytes_of("tape-body");
  std::string uri, err;
  ASSERT_TRUE(tape_put_bag(attrs, "archive/bag/abc", body, &rule, uri, err)) << err;
  EXPECT_FALSE(fs::path(uri).is_absolute());
  EXPECT_TRUE(fs::exists(tape_root / uri)) << uri;
  EXPECT_FALSE(fs::exists(evil_root));

  attrs[kTapeUriAttr] = uri;
  std::vector<std::uint8_t> back;
  ASSERT_TRUE(tape_get_bag(attrs, "archive/bag/abc", &rule, back, err)) << err;
  EXPECT_EQ(back, body);

  // No rule at all: refuse rather than fall back to attrs.
  std::string uri2;
  EXPECT_FALSE(tape_put_bag(attrs, "archive/bag/abc", body, nullptr, uri2, err));
  EXPECT_FALSE(tape_get_bag(attrs, "archive/bag/abc", nullptr, back, err));

  fs::remove_all(root);
}

TEST(Review2Store, TapeUriMustStayUnderTapeRoot) {
  const auto root = temp_root("r2s-tape-uri");
  const auto tape_root = root / "tape";
  fs::create_directories(tape_root / "bags");
  {
    std::ofstream(root / "x") << "outside";
    std::ofstream(tape_root / "bags" / "ok") << "inside";
  }
  ArchiveRule rule;
  rule.tape_sink = "external";
  rule.tape_root = tape_root.string();

  std::unordered_map<std::string, std::string> attrs;
  attrs[kTapeSinkAttr] = "external";
  std::vector<std::uint8_t> out;
  std::string err;
  for (const char* bad : {"../../x", "../x", "/etc/hosts", "bags/../../x", "..", ".", "/"}) {
    attrs[kTapeUriAttr] = bad;
    EXPECT_FALSE(tape_get_bag(attrs, "archive/bag/abc", &rule, out, err)) << bad;
  }
  attrs[kTapeUriAttr] = "bags/ok";
  ASSERT_TRUE(tape_get_bag(attrs, "archive/bag/abc", &rule, out, err)) << err;
  EXPECT_EQ(std::string(out.begin(), out.end()), "inside");
  // "bags/./ok" normalizes inside the root and is fine.
  attrs[kTapeUriAttr] = "bags/./ok";
  EXPECT_TRUE(tape_get_bag(attrs, "archive/bag/abc", &rule, out, err)) << err;

  // Size cap: the rule's max bag size bounds what recall will read back.
  rule.max_bag_bytes = 3;
  attrs[kTapeUriAttr] = "bags/ok";
  EXPECT_FALSE(tape_get_bag(attrs, "archive/bag/abc", &rule, out, err));
  EXPECT_NE(err.find("maximum"), std::string::npos) << err;

  // s3 sink: a URI outside the rule's prefix is refused before any binary runs.
  ArchiveRule s3;
  s3.tape_sink = "s3";
  s3.tape_uri_prefix = "s3://good/bags/";
  s3.tape_bin = "/bin/false";
  s3.tape_root = (root / "scratch").string();
  attrs[kTapeUriAttr] = "/etc/hosts";
  EXPECT_FALSE(tape_get_bag(attrs, "archive/bag/abc", &s3, out, err));
  EXPECT_NE(err.find("prefix"), std::string::npos) << err;

  fs::remove_all(root);
}

// ---------------------------------------------------------------- STO-2

TEST(Review2Store, InstallReplicaVersionRejectsMovedTip) {
  DualStoreFixture fx("r2s-tip-cas");
  const std::string oid = "cold/cas";
  ASSERT_TRUE(put_str(*fx.svc, oid, "original"));

  auto* primary = primary_store_for(fx, oid);
  ASSERT_NE(primary, nullptr);
  std::string err;
  auto info = primary->stat(oid, err);
  ASSERT_TRUE(info.has_value()) << err;
  const std::uint64_t old_tip = info->seq;

  // A client writes in between "read body" and "install stub".
  ASSERT_TRUE(put_str(*fx.svc, oid, "client-wrote-this"));

  auto dest = place(oid, fx.map, fx.cfg.replica_count, "nvme");
  std::unordered_map<std::string, std::string> stub;
  apply_frozen_stub_attrs(stub, "archive/bag/x", 0, 8, std::string(64, 'a'));
  std::vector<std::uint8_t> empty;
  bool moved = false;
  std::uint64_t new_seq = 0;
  EXPECT_FALSE(install_replica_version(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, dest, oid,
                                       empty, stub, old_tip, &new_seq, &moved));
  EXPECT_TRUE(moved);

  auto g = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  ASSERT_TRUE(g.ok && g.data);
  EXPECT_EQ(std::string(g.data->begin(), g.data->end()), "client-wrote-this");
  EXPECT_FALSE(attrs_are_frozen(g.attrs));

  // With the current tip the install goes through and reports the new seq.
  auto now_info = primary->stat(oid, err);
  ASSERT_TRUE(now_info.has_value());
  EXPECT_TRUE(install_replica_version(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, dest, oid,
                                      empty, stub, now_info->seq, &new_seq, &moved));
  EXPECT_FALSE(moved);
  EXPECT_GT(new_seq, now_info->seq);
}

TEST(Review2Store, PreparePutExpectedTipMismatchIsRejected) {
  const auto root = temp_root("r2s-prep-cas");
  ObjectStore st;
  std::string err;
  ASSERT_TRUE(st.open(root.string(), default_opts(), err)) << err;
  ASSERT_TRUE(st.put("o", "v1", {}, true, err)) << err;
  std::uint64_t tip = 0;
  ASSERT_TRUE(st.tip_seq("o", tip, err));
  PreparedVersion pv;
  const auto v2 = bytes_of("v2");
  EXPECT_FALSE(st.prepare_put("o", v2.data(), v2.size(), {}, true, std::nullopt, tip + 7, pv, err));
  EXPECT_EQ(err, "tip changed during upload");
  EXPECT_TRUE(st.prepare_put("o", v2.data(), v2.size(), {}, true, std::nullopt, tip, pv, err))
      << err;
  st.close();
  fs::remove_all(root);
}

// ---------------------------------------------------------------- STO-3

TEST(Review2Store, LocalStoresRetiresLateAndKeepsRawPointersValid) {
  const auto root = temp_root("r2s-localstores");
  const std::string p = (root / "aios").string();
  fs::create_directories(p);

  LocalStores ls;
  ls.set_retire_policy(3, 60 * 1000);
  ls.sync_paths({p}, default_opts());
  ObjectStore* raw = ls.get(p);
  ASSERT_NE(raw, nullptr);
  std::shared_ptr<ObjectStore> shared = ls.get_shared(p);
  ASSERT_EQ(shared.get(), raw);
  std::weak_ptr<ObjectStore> weak = shared;
  shared.reset();

  std::string err;
  ASSERT_TRUE(raw->put("k", "v", {}, true, err)) << err;

  // Two scans without the path: still served (transient scan glitch).
  ls.sync_paths({}, default_opts());
  EXPECT_EQ(ls.get(p), raw);
  ls.sync_paths({}, default_opts());
  EXPECT_EQ(ls.get(p), raw);
  EXPECT_EQ(ls.retired_count(), 0u);

  // Third consecutive miss retires it, but the object stays alive for the grace period.
  ls.sync_paths({}, default_opts());
  EXPECT_EQ(ls.get(p), nullptr);
  EXPECT_EQ(ls.retired_count(), 1u);
  EXPECT_FALSE(weak.expired());
  auto got = raw->get("k", err);
  ASSERT_TRUE(got.has_value()) << err;
  EXPECT_EQ(std::string(got->begin(), got->end()), "v");
  ls.sync_paths({}, default_opts());
  EXPECT_EQ(ls.retired_count(), 1u);
  EXPECT_FALSE(weak.expired());

  // The path coming back inside the grace period revives the same store.
  ls.sync_paths({p}, default_opts());
  EXPECT_EQ(ls.get(p), raw);
  EXPECT_EQ(ls.retired_count(), 0u);

  // Grace elapsed: released on a later sync.
  ls.set_retire_policy(1, 0);
  ls.sync_paths({}, default_opts());
  EXPECT_EQ(ls.retired_count(), 1u);
  ls.sync_paths({}, default_opts());
  EXPECT_EQ(ls.retired_count(), 0u);
  EXPECT_TRUE(weak.expired());

  fs::remove_all(root);
}

// ---------------------------------------------------------------- STO-4

#if defined(AIOS_HAVE_ZSTD) && AIOS_HAVE_ZSTD
TEST(Review2Store, ZstdDecompressRejectsBogusLogicalSizeWithoutAllocating) {
  ASSERT_TRUE(zstd_available());
  std::vector<std::uint8_t> plain(100, 'z');
  std::vector<std::uint8_t> frame;
  std::string err;
  ASSERT_TRUE(zstd_compress(plain.data(), plain.size(), 3, frame, err)) << err;
  ASSERT_LE(frame.size(), 100u);

  const std::uint64_t k64g = 64ull * 1024ull * 1024ull * 1024ull;
  std::vector<std::uint8_t> out;
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(zstd_decompress(frame.data(), frame.size(), k64g, out, err));
  EXPECT_TRUE(out.empty());
  EXPECT_FALSE(zstd_decompress(frame.data(), frame.size(), 200, out, err));  // header says 100
  EXPECT_NE(err.find("mismatch"), std::string::npos) << err;

  // Crafted 100-byte frame whose header itself claims 64 GiB: ratio check catches it.
  std::vector<std::uint8_t> crafted{0x28, 0xB5, 0x2F, 0xFD, 0xE0};
  put_le64(crafted, 1ull << 36);
  crafted.resize(100, 0);
  EXPECT_FALSE(zstd_decompress(crafted.data(), crafted.size(), 1ull << 36, out, err));
  EXPECT_NE(err.find("implausible"), std::string::npos) << err;
  EXPECT_TRUE(out.empty());
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);

  // Legit round trips: one-shot (small) and streaming (> 16 MiB) paths.
  ASSERT_TRUE(zstd_decompress(frame.data(), frame.size(), plain.size(), out, err)) << err;
  EXPECT_EQ(out, plain);

  std::vector<std::uint8_t> big(20u * 1024u * 1024u + 12345u);
  for (std::size_t i = 0; i < big.size(); ++i) {
    big[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 13);
  }
  std::vector<std::uint8_t> big_frame;
  ASSERT_TRUE(zstd_compress(big.data(), big.size(), 1, big_frame, err)) << err;
  ASSERT_TRUE(zstd_decompress(big_frame.data(), big_frame.size(), big.size(), out, err)) << err;
  EXPECT_EQ(out, big);
  // Highly compressible data (all zeros) must still round-trip through the ratio check.
  std::vector<std::uint8_t> zeros(32u * 1024u * 1024u, 0);
  ASSERT_TRUE(zstd_compress(zeros.data(), zeros.size(), 1, big_frame, err)) << err;
  ASSERT_TRUE(zstd_decompress(big_frame.data(), big_frame.size(), zeros.size(), out, err)) << err;
  EXPECT_EQ(out, zeros);
}
#endif

// ---------------------------------------------------------------- STO-5

TEST(Review2Store, BackupReturnsErrorOnMalformedPosixMetadata) {
  DualStoreFixture fx("r2s-backup-bad");
  auto& svc = *fx.svc;
  const std::string super_body =
      nlohmann::json{{"aios_posix_super", 1}, {"next_ino", 3}, {"frozen", false}}.dump();

  // Case A: dir log with a non-numeric inode.
  {
    const std::string vol = "va";
    ASSERT_TRUE(put_str(svc, "posix/" + vol + "/super", super_body));
    const std::string log = dir_log_record(1, 1, {"sub", "not-a-number"});
    ASSERT_TRUE(put_str(svc, "posix/" + vol + "/dir/1/log", log));
    ASSERT_TRUE(put_str(svc, "posix/" + vol + "/dir/1/meta",
                        nlohmann::json{{"log_bytes", log.size()}, {"snapshot_op", 0}}.dump()));
    std::string snap_id, err;
    EXPECT_FALSE(backup_snapshot_posix(svc, vol, "/sub", snap_id, err));
    EXPECT_NE(err.find("bad dir log"), std::string::npos) << err;
    // Volume is unfrozen again afterwards.
    auto g = svc.api_get("posix/" + vol + "/super", std::nullopt, std::nullopt, {});
    ASSERT_TRUE(g.ok && g.data);
    EXPECT_FALSE(nlohmann::json::parse(std::string(g.data->begin(), g.data->end()))
                     .value("frozen", true));
  }

  // Case B: inode JSON with "mode": "x".
  {
    const std::string vol = "vb";
    ASSERT_TRUE(put_str(svc, "posix/" + vol + "/super", super_body));
    ASSERT_TRUE(put_str(svc, "posix/" + vol + "/dir/1/meta",
                        nlohmann::json{{"log_bytes", 0}, {"snapshot_op", 1}}.dump()));
    ASSERT_TRUE(put_str(svc, "posix/" + vol + "/dir/1/snap",
                        nlohmann::json{{"entries", {{"sub", 2}}}}.dump()));
    ASSERT_TRUE(put_str(svc, "posix/" + vol + "/ino/2", nlohmann::json{{"mode", "x"}}.dump()));
    std::string snap_id, err;
    EXPECT_FALSE(backup_snapshot_posix(svc, vol, "/sub", snap_id, err));
    EXPECT_NE(err.find("bad inode mode"), std::string::npos) << err;
  }

  // Case C: dir snapshot entry with a string inode.
  {
    const std::string vol = "vc";
    ASSERT_TRUE(put_str(svc, "posix/" + vol + "/super", super_body));
    ASSERT_TRUE(put_str(svc, "posix/" + vol + "/dir/1/meta",
                        nlohmann::json{{"log_bytes", 0}, {"snapshot_op", 1}}.dump()));
    ASSERT_TRUE(put_str(svc, "posix/" + vol + "/dir/1/snap",
                        nlohmann::json{{"entries", {{"sub", "2"}}}}.dump()));
    std::string snap_id, err;
    EXPECT_FALSE(backup_snapshot_posix(svc, vol, "/sub", snap_id, err));
    EXPECT_NE(err.find("bad dir snapshot"), std::string::npos) << err;
  }
}

// ---------------------------------------------------------------- STO-6

TEST(Review2Store, PipelinedPutAndAttrCloneProduceReadableVersions) {
  const auto root = temp_root("r2s-durable");
  ObjectStore st;
  std::string err;
  auto opts = default_opts();
  opts.data_fsync = true;
  ASSERT_TRUE(st.open(root.string(), opts, err)) << err;

  std::vector<std::uint8_t> body(4096);
  for (std::size_t i = 0; i < body.size(); ++i) body[i] = static_cast<std::uint8_t>(i * 7);
  std::string staging;
  ASSERT_TRUE(st.create_staging_file("big", staging, err)) << err;
  ASSERT_TRUE(st.stage_pwrite(staging, 0, body.data(), body.size(), err)) << err;
  PreparedVersion pv;
  ASSERT_TRUE(st.prepare_put_file("big", staging, body.size(), crc32c(body.data(), body.size()),
                                  {{"k", "v"}}, true, std::nullopt, pv, err))
      << err;
  EXPECT_FALSE(fs::exists(staging));
  ASSERT_TRUE(st.publish_tip("big", pv.seq, err)) << err;
  auto got = st.get("big", err);
  ASSERT_TRUE(got.has_value()) << err;
  EXPECT_EQ(*got, body);
  auto path = st.fs_body_path("big", err);
  ASSERT_TRUE(path.has_value()) << err;
  EXPECT_TRUE(fs::is_regular_file(*path));
  EXPECT_FALSE(fs::exists(*path + ".tmp"));

  // set_attr clones the FS body into a new version; it must be complete and readable.
  ASSERT_TRUE(st.set_attr("big", "k2", "v2", err)) << err;
  auto got2 = st.get("big", err);
  ASSERT_TRUE(got2.has_value()) << err;
  EXPECT_EQ(*got2, body);
  auto attrs = st.list_attrs("big", err);
  EXPECT_EQ(attrs["k"], "v");
  EXPECT_EQ(attrs["k2"], "v2");
  st.close();
  fs::remove_all(root);
}

// ---------------------------------------------------------------- STO-8

TEST(Review2Store, BagDecodeRejectsHostileHeader) {
  std::vector<ArchiveMember> members(1);
  members[0].oid = "cold/a";
  members[0].data = {'a', 'b', 'c'};
  std::vector<std::uint8_t> bag;
  std::string err;
  ASSERT_TRUE(encode_archive_bag(members, bag, err)) << err;

  // count = 0xFFFFFFFF: refused before reserve() can throw.
  auto hostile = bag;
  for (int i = 0; i < 4; ++i) hostile[8 + i] = 0xff;
  ArchiveBag out;
  EXPECT_FALSE(decode_archive_bag(hostile.data(), hostile.size(), out, false, err));
  EXPECT_EQ(err, "bad member count");

  // Member offset near 2^64 would wrap the old offset+length check.
  hostile = bag;
  std::uint64_t index_off = 0;
  for (int i = 0; i < 8; ++i) index_off |= static_cast<std::uint64_t>(hostile[12 + i]) << (8 * i);
  const std::size_t off_pos = static_cast<std::size_t>(index_off) + 2 + members[0].oid.size();
  const std::uint64_t wrap = ~0ull - 1;
  for (int i = 0; i < 8; ++i) hostile[off_pos + i] = static_cast<std::uint8_t>((wrap >> (8 * i)) & 0xff);
  EXPECT_FALSE(decode_archive_bag(hostile.data(), hostile.size(), out, true, err));
  EXPECT_EQ(err, "member out of range");

  // Sanity: the untouched bag still decodes.
  ASSERT_TRUE(decode_archive_bag(bag.data(), bag.size(), out, true, err)) << err;
  ASSERT_EQ(out.members.size(), 1u);
  EXPECT_EQ(out.members[0].data, members[0].data);
}

// ---------------------------------------------------------------- STO-9

TEST(Review2Store, CachedStatementsAreResetAfterEveryUse) {
  const auto root = temp_root("r2s-stmt");
  ObjectStore st;
  std::string err;
  ASSERT_TRUE(st.open(root.string(), default_opts(), err)) << err;
  std::vector<std::uint8_t> big(2048, 'b');
  ASSERT_TRUE(st.put("small", "inline-body", {{"a", "1"}}, true, err)) << err;
  ASSERT_TRUE(st.put("big", big.data(), big.size(), {{"a", "2"}}, true, err)) << err;
  EXPECT_FALSE(st.debug_any_stmt_busy());
  EXPECT_TRUE(st.get("small", err).has_value());
  EXPECT_FALSE(st.debug_any_stmt_busy());
  EXPECT_TRUE(st.get("big", err).has_value());
  EXPECT_TRUE(st.stat("small", err).has_value());
  EXPECT_FALSE(st.list_attrs("big", err).empty());
  EXPECT_TRUE(st.put_range("small", 3, big.data(), 5, {}, false, err)) << err;
  EXPECT_TRUE(st.set_attr("small", "z", "9", err)) << err;
  EXPECT_FALSE(st.stat("missing", err).has_value());
  EXPECT_FALSE(st.debug_any_stmt_busy());
  st.close();
  fs::remove_all(root);
}

// ---------------------------------------------------------------- STO-10

TEST(Review2Store, StagingFilesAreUniquePerCall) {
  const auto root = temp_root("r2s-staging");
  ObjectStore st;
  std::string err;
  ASSERT_TRUE(st.open(root.string(), default_opts(), err)) << err;
  std::vector<std::string> paths;
  for (int i = 0; i < 16; ++i) {
    std::string p;
    ASSERT_TRUE(st.create_staging_file("same-oid", p, err)) << err;
    for (const auto& prev : paths) EXPECT_NE(prev, p);
    EXPECT_TRUE(fs::exists(p));
    paths.push_back(p);
  }
  st.close();
  fs::remove_all(root);
}

// ---------------------------------------------------------------- STO-11

TEST(Review2Store, AiosMarkerUpdateLeavesNoTempFile) {
  const auto root = temp_root("r2s-marker");
  const auto marker = root / ".aios";
  {
    std::ofstream out(marker);
    out << "storage_class: nvme\nweight: 2\n";
  }
  std::string err;
  ASSERT_TRUE(update_aios_marker_file(marker.string(), std::string("drain"), 5, err)) << err;
  EXPECT_FALSE(fs::exists(root / ".aios.tmp"));
  std::ifstream in(marker);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  AiosMarker parsed;
  ASSERT_TRUE(parse_aios_marker(text, root.string(), parsed, err)) << err;
  EXPECT_EQ(parsed.storage_class, "nvme");
  EXPECT_EQ(parsed.weight, 5);
  EXPECT_EQ(parsed.state, LifecycleState::Drain);
  fs::remove_all(root);
}

TEST(Review2Store, PlacementIsStableAcrossRepeatedCallsAndShareRing) {
  DualStoreFixture fx("r2s-place");
  const auto a = place("some/oid", fx.map, 2, "nvme");
  const auto b = place("some/oid", fx.map, 2, "nvme");
  ASSERT_EQ(a.acting_set.size(), 2u);
  ASSERT_EQ(a.acting_set.size(), b.acting_set.size());
  for (std::size_t i = 0; i < a.acting_set.size(); ++i) {
    EXPECT_EQ(target_key(a.acting_set[i]), target_key(b.acting_set[i]));
  }
  EXPECT_TRUE(place("x", fx.map, 3, "nvme").acting_set.empty());
  EXPECT_TRUE(place("x", fx.map, 1, "archive").acting_set.empty());
}

// ---------------------------------------------------------------- OBJ-2 / OBJ-13

TEST(Review2Store, InstallVersionIgnoresEscapingFsPath) {
  const auto root = temp_root("r2s-fspath");
  ObjectStore st;
  std::string err;
  ASSERT_TRUE(st.open(root.string(), default_opts(), err)) << err;
  std::vector<std::uint8_t> body(1024, 'q');
  const auto canon = fs::weakly_canonical(root);

  for (const char* evil : {"../../../../tmp/r2s-escape", "/tmp/r2s-escape-abs", "objects/../../x"}) {
    const std::string oid = std::string("o-") + evil;
    PreparedVersion pv;
    pv.oid = oid;
    pv.seq = 1;
    pv.size = body.size();
    pv.crc32c = crc32c(body.data(), body.size());
    pv.inline_body = false;
    pv.fs_path = evil;
    ASSERT_TRUE(st.install_version(pv, body.data(), body.size(), {}, err)) << evil << ": " << err;
    ASSERT_TRUE(st.publish_tip(oid, 1, err)) << err;
    auto info = st.stat(oid, err);
    ASSERT_TRUE(info.has_value()) << err;
    EXPECT_EQ(info->fs_path.rfind("objects/", 0), 0u) << info->fs_path;
    auto abs = st.fs_body_path(oid, err);
    ASSERT_TRUE(abs.has_value()) << err;
    const auto abs_canon = fs::weakly_canonical(*abs).string();
    EXPECT_EQ(abs_canon.rfind(canon.string(), 0), 0u) << abs_canon;
    auto got = st.get(oid, err);
    ASSERT_TRUE(got.has_value()) << err;
    EXPECT_EQ(*got, body);
  }
  EXPECT_FALSE(fs::exists("/tmp/r2s-escape"));
  EXPECT_FALSE(fs::exists("/tmp/r2s-escape-abs"));
  st.close();
  fs::remove_all(root);
}

TEST(Review2Store, InstallVersionRejectsCrcMismatch) {
  const auto root = temp_root("r2s-crc");
  ObjectStore st;
  std::string err;
  ASSERT_TRUE(st.open(root.string(), default_opts(), err)) << err;
  const auto small = bytes_of("hello");
  const std::vector<std::uint8_t> big(1024, 'h');
  for (const auto* body : {&small, &big}) {
    PreparedVersion pv;
    pv.oid = body == &small ? "inl" : "fsb";
    pv.seq = 1;
    pv.size = body->size();
    pv.crc32c = crc32c(body->data(), body->size()) ^ 0x1u;
    pv.inline_body = body == &small;
    EXPECT_FALSE(st.install_version(pv, body->data(), body->size(), {}, err));
    EXPECT_EQ(err, "install crc32c mismatch");
    pv.crc32c ^= 0x1u;
    EXPECT_TRUE(st.install_version(pv, body->data(), body->size(), {}, err)) << err;
    // A caller that already verified the CRC while staging is trusted.
    PreparedVersion pv2 = pv;
    pv2.seq = 2;
    pv2.crc32c ^= 0x1u;
    pv2.crc_verified = true;
    EXPECT_TRUE(st.install_version(pv2, body->data(), body->size(), {}, err)) << err;
  }
  st.close();
  fs::remove_all(root);
}

// ---------------------------------------------------------------- OBJ-1-adjacent

TEST(Review2Store, FsTableFromJsonSkipsMalformedEntries) {
  nlohmann::json good = {{"node_id", "n1"}, {"aios_path", "/a"}, {"storage_class", "nvme"},
                         {"weight", 3}, {"usable", true}, {"updated_ms", 5}};
  nlohmann::json bad_weight = good;
  bad_weight["weight"] = "heavy";
  nlohmann::json bad_node = good;
  bad_node["node_id"] = 42;
  nlohmann::json bad_usable = good;
  bad_usable["usable"] = "yes";
  nlohmann::json j = {{"entries", nlohmann::json::array({good, bad_weight, 7, "str",
                                                          bad_node, bad_usable, good})}};
  std::vector<FsEntry> out;
  EXPECT_NO_THROW(out = FsTable::from_json(j));
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].node_id, "n1");
  EXPECT_EQ(out[0].weight, 3);
  EXPECT_TRUE(out[0].usable);

  EXPECT_NO_THROW(out = FsTable::from_json(nlohmann::json("nope")));
  EXPECT_TRUE(out.empty());
  EXPECT_NO_THROW(out = FsTable::from_json(nlohmann::json{{"entries", 12}}));
  EXPECT_TRUE(out.empty());
  EXPECT_NO_THROW(out = FsTable::from_json(nlohmann::json(nullptr)));
  EXPECT_TRUE(out.empty());
}

TEST(Review2Store, ClusterMapFromJsonToleratesWrongTypes) {
  nlohmann::json good = {{"node_id", "n1"}, {"addr", "h:1"}, {"aios_path", "/a"},
                         {"storage_class", "nvme"}, {"weight", 2}};
  nlohmann::json bad = good;
  bad["weight"] = "two";
  nlohmann::json bad2 = good;
  bad2["bavail"] = -5;
  nlohmann::json j = {{"epoch", "not-a-number"},
                      {"replica_count", 2},
                      {"placement", {{"vnodes_per_target", "x"}, {"min_vnodes", 4}}},
                      {"targets", nlohmann::json::array({good, bad, 3, bad2})}};
  ClusterMap m;
  EXPECT_NO_THROW(m = ClusterMap::from_json(j));
  EXPECT_EQ(m.epoch, 0u);
  EXPECT_EQ(m.replica_count, 2);
  EXPECT_EQ(m.placement.vnodes_per_target, 128);
  EXPECT_EQ(m.placement.min_vnodes, 4);
  ASSERT_EQ(m.targets.size(), 1u);
  EXPECT_EQ(m.targets[0].weight, 2);

  EXPECT_NO_THROW(m = ClusterMap::from_json(nlohmann::json::array({1, 2})));
  EXPECT_TRUE(m.targets.empty());
  EXPECT_NO_THROW(m = ClusterMap::from_json(nlohmann::json{{"targets", "x"}}));
  EXPECT_TRUE(m.targets.empty());
}
