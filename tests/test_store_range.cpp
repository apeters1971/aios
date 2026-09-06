#include "store/object_store.hpp"
#include <gtest/gtest.h>

#include "util/crc32c.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;


TEST(StoreRange, Basic) {
  using namespace aios;
  const auto root = fs::temp_directory_path() / ("aios-range-" + std::to_string(::getpid()));
  fs::create_directories(root);
  ObjectStore store;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.inline_max_bytes = 64;
  std::string err;
  EXPECT_TRUE(store.open(root.string(), opts, err)) << "open";

  // Full small put then ranged overwrite beyond inline → promote.
  EXPECT_TRUE(store.put("o1", std::string("abcd"), {}, true, err)) << "put small";
  const std::string chunk(100, 'Z');
  EXPECT_TRUE(store.put_range("o1", 50, reinterpret_cast<const std::uint8_t*>(chunk.data()),
                         chunk.size(), {{"k", "v"}}, false, err)) << "put_range grow";
  auto st = store.stat("o1", err);
  EXPECT_TRUE(st.has_value()) << "stat";
  EXPECT_TRUE(st->size == 150) << "size 150";
  EXPECT_TRUE(!st->inline_body) << "fs backed";

  auto mid = store.get_range("o1", 50, 100, err);
  EXPECT_TRUE(mid.has_value() && mid->size() == 100) << "get_range len";
  EXPECT_TRUE(std::string(mid->begin(), mid->end()) == chunk) << "get_range data";

  auto head = store.get_range("o1", 0, 4, err);
  EXPECT_TRUE(head.has_value()) << "head range";
  // First 4 bytes from original inline promote.
  EXPECT_TRUE(std::string(head->begin(), head->end()) == "abcd") << "preserved prefix";

  // Preconditions
  std::vector<AttrPrecondition> ok_eq = {
      {AttrPrecondition::Kind::Eq, "k", "v"},
  };
  EXPECT_TRUE(store.check_preconditions("o1", ok_eq, err) == PrecondResult::Ok) << "pred eq";
  std::vector<AttrPrecondition> bad = {
      {AttrPrecondition::Kind::Eq, "k", "nope"},
  };
  EXPECT_TRUE(store.check_preconditions("o1", bad, err) == PrecondResult::Conflict) << "pred conflict";
  std::vector<AttrPrecondition> create_only = {
      {AttrPrecondition::Kind::MustNotExist, {}, {}},
  };
  EXPECT_TRUE(store.check_preconditions("o1", create_only, err) == PrecondResult::Conflict) << "must not exist";

  // LIST
  EXPECT_TRUE(store.put("pref/a", std::string("1"), {}, true, err)) << "put a";
  EXPECT_TRUE(store.put("pref/b", std::string("2"), {}, true, err)) << "put b";
  EXPECT_TRUE(store.put("other", std::string("3"), {}, true, err)) << "put other";
  auto lst = store.list("pref/", "", "", 10, "", false, err);
  EXPECT_TRUE(lst.objects.size() == 2) << "list prefix count";

  // Unsatisfiable range
  auto bad_r = store.get_range("o1", 1000, 10, err);
  EXPECT_TRUE(!bad_r.has_value()) << "unsat range";

  fs::remove_all(root);
}

TEST(StoreRange, NextSeqIsTipPlusOne) {
  using namespace aios;
  const auto root = fs::temp_directory_path() / ("aios-seq-" + std::to_string(::getpid()));
  fs::create_directories(root);
  ObjectStore store;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  std::string err;
  ASSERT_TRUE(store.open(root.string(), opts, err)) << err;

  std::uint64_t seq = 0, tip = 0;
  ASSERT_TRUE(store.peek_next_seq("seq-o", seq, tip, err)) << err;
  EXPECT_EQ(tip, 0u);
  EXPECT_EQ(seq, 1u);

  ASSERT_TRUE(store.put("seq-o", std::string("a"), {}, true, err)) << err;
  ASSERT_TRUE(store.peek_next_seq("seq-o", seq, tip, err)) << err;
  EXPECT_EQ(tip, 1u);
  EXPECT_EQ(seq, 2u);

  ASSERT_TRUE(store.put("seq-o", std::string("bb"), {}, true, err)) << err;
  ASSERT_TRUE(store.peek_next_seq("seq-o", seq, tip, err)) << err;
  EXPECT_EQ(tip, 2u);
  EXPECT_EQ(seq, 3u);

  PreparedVersion pv;
  ASSERT_TRUE(store.prepare_put("seq-o", reinterpret_cast<const std::uint8_t*>("ccc"), 3, {}, true,
                                std::nullopt, pv, err))
      << err;
  EXPECT_EQ(pv.seq, 3u);
  ASSERT_TRUE(store.abort_version("seq-o", pv.seq, err)) << err;
  ASSERT_TRUE(store.peek_next_seq("seq-o", seq, tip, err)) << err;
  EXPECT_EQ(tip, 2u);
  EXPECT_GE(seq, 3u);

  fs::remove_all(root);
}

TEST(StoreRange, DeltaChainHistoricalReadsAndCrc) {
  using namespace aios;
  const auto root = fs::temp_directory_path() / ("aios-delta-" + std::to_string(::getpid()));
  fs::create_directories(root);
  ObjectStore store;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.inline_max_bytes = 64;
  opts.clone_required = false;
  opts.verify_range_crc = true;
  opts.data_fsync = false;
  std::string err;
  ASSERT_TRUE(store.open(root.string(), opts, err)) << err;

  std::vector<std::uint8_t> body(128 * 1024, 'A');
  ASSERT_TRUE(store.put("d", body.data(), body.size(), {}, true, err)) << err;
  auto base = store.stat("d", err);
  ASSERT_TRUE(base && !base->delta && !base->fs_path.empty());
  ASSERT_FALSE(base->block_crcs.empty());
  EXPECT_EQ(crc32c_from_blocks(base->block_crcs, base->size), crc32c(body.data(), body.size()));

  const std::string p1(4096, 'B');
  ASSERT_TRUE(store.put_range("d", 100, reinterpret_cast<const std::uint8_t*>(p1.data()), p1.size(),
                              {}, false, err))
      << err;
  auto v2 = store.stat("d", err);
  ASSERT_TRUE(v2 && v2->delta) << "first range is a delta over the base file";
  EXPECT_EQ(v2->fs_path, base->fs_path);
  EXPECT_FALSE(store.fs_body_path("d", err).has_value());

  std::string p2(2048, 'C');
  ASSERT_TRUE(store.put_range("d", 80 * 1024, reinterpret_cast<const std::uint8_t*>(p2.data()),
                              p2.size(), {}, false, err))
      << err;
  auto v3 = store.stat("d", err);
  ASSERT_TRUE(v3 && v3->delta);
  EXPECT_EQ(v3->fs_path, base->fs_path);

  auto old = store.get("d", base->seq, err);
  ASSERT_TRUE(old && *old == body) << "historical seq still sees the unpatched file";

  std::vector<std::uint8_t> want = body;
  std::copy(p1.begin(), p1.end(), want.begin() + 100);
  auto mid = store.get("d", v2->seq, err);
  ASSERT_TRUE(mid && *mid == want);

  std::copy(p2.begin(), p2.end(), want.begin() + 80 * 1024);
  auto tip = store.get("d", err);
  ASSERT_TRUE(tip && *tip == want);
  EXPECT_EQ(v3->crc32c, crc32c(want.data(), want.size()));
  EXPECT_EQ(crc32c_from_blocks(v3->block_crcs, v3->size), v3->crc32c);

  auto slice = store.get_range("d", 80 * 1024, 2048, err);
  ASSERT_TRUE(slice && std::string(slice->begin(), slice->end()) == p2);

  fs::remove_all(root);
}

TEST(StoreRange, MaterializeWhenChainLimitHit) {
  using namespace aios;
  const auto root = fs::temp_directory_path() / ("aios-mat-" + std::to_string(::getpid()));
  fs::create_directories(root);
  ObjectStore store;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.inline_max_bytes = 64;
  opts.clone_required = false;
  opts.delta_max_chain = 2;
  opts.data_fsync = false;
  std::string err;
  ASSERT_TRUE(store.open(root.string(), opts, err)) << err;

  std::vector<std::uint8_t> body(96 * 1024, 'x');
  ASSERT_TRUE(store.put("m", body.data(), body.size(), {}, true, err)) << err;
  auto base = store.stat("m", err);
  ASSERT_TRUE(base);

  const std::uint8_t one = '1';
  ASSERT_TRUE(store.put_range("m", 0, &one, 1, {}, false, err)) << err;
  auto d1 = store.stat("m", err);
  ASSERT_TRUE(d1 && d1->delta && d1->fs_path == base->fs_path);

  const std::uint8_t two = '2';
  ASSERT_TRUE(store.put_range("m", 1, &two, 1, {}, false, err)) << err;
  auto d2 = store.stat("m", err);
  ASSERT_TRUE(d2 && d2->delta && d2->fs_path == base->fs_path);

  const std::uint8_t three = '3';
  ASSERT_TRUE(store.put_range("m", 2, &three, 1, {}, false, err)) << err;
  auto mat = store.stat("m", err);
  ASSERT_TRUE(mat && !mat->delta) << "third patch materializes a standalone body";
  EXPECT_NE(mat->fs_path, base->fs_path);
  auto got = store.get("m", err);
  ASSERT_TRUE(got && got->size() == body.size());
  EXPECT_EQ((*got)[0], '1');
  EXPECT_EQ((*got)[1], '2');
  EXPECT_EQ((*got)[2], '3');
  EXPECT_EQ((*got)[3], 'x');

  fs::remove_all(root);
}

TEST(StoreRange, SparseHoleAndAbortKeepsSharedBase) {
  using namespace aios;
  const auto root = fs::temp_directory_path() / ("aios-sparse-" + std::to_string(::getpid()));
  fs::create_directories(root);
  ObjectStore store;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.inline_max_bytes = 64;
  opts.clone_required = false;
  opts.verify_range_crc = true;
  opts.data_fsync = false;
  std::string err;
  ASSERT_TRUE(store.open(root.string(), opts, err)) << err;

  std::vector<std::uint8_t> body(8 * 1024, 'Z');
  ASSERT_TRUE(store.put("s", body.data(), body.size(), {}, true, err)) << err;
  auto base = store.stat("s", err);
  ASSERT_TRUE(base);
  const auto base_path = store.fs_body_path("s", err);
  ASSERT_TRUE(base_path);

  const std::string tail(16, 'Q');
  PreparedVersion pv;
  ASSERT_TRUE(store.prepare_put_range("s", 2 * 1024 * 1024,
                                      reinterpret_cast<const std::uint8_t*>(tail.data()),
                                      tail.size(), {}, false, pv, err))
      << err;
  EXPECT_TRUE(pv.delta);
  EXPECT_EQ(pv.size, 2 * 1024 * 1024 + tail.size());

  auto staged = store.get("s", pv.seq, err);
  ASSERT_TRUE(staged && staged->size() == pv.size);
  EXPECT_EQ(std::vector<std::uint8_t>(staged->begin(), staged->begin() + body.size()), body);
  EXPECT_TRUE(std::all_of(staged->begin() + body.size(),
                          staged->end() - static_cast<std::ptrdiff_t>(tail.size()),
                          [](std::uint8_t c) { return c == 0; }));
  EXPECT_EQ(std::string(staged->end() - static_cast<std::ptrdiff_t>(tail.size()), staged->end()),
            tail);

  ASSERT_TRUE(store.abort_version("s", pv.seq, err)) << err;
  auto after = store.stat("s", err);
  ASSERT_TRUE(after && after->seq == base->seq);
  EXPECT_TRUE(fs::exists(*base_path));
  auto restored = store.get("s", err);
  ASSERT_TRUE(restored && *restored == body);

  fs::remove_all(root);
}

TEST(StoreRange, InstallRangeRequiresMatchingTip) {
  using namespace aios;
  const auto root = fs::temp_directory_path() / ("aios-inst-" + std::to_string(::getpid()));
  fs::create_directories(root);
  ObjectStore store;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.inline_max_bytes = 64;
  opts.clone_required = false;
  opts.data_fsync = false;
  std::string err;
  ASSERT_TRUE(store.open(root.string(), opts, err)) << err;

  std::vector<std::uint8_t> body(70 * 1024, 'A');
  ASSERT_TRUE(store.put("i", body.data(), body.size(), {}, true, err)) << err;
  auto tip = store.stat("i", err);
  ASSERT_TRUE(tip);

  std::vector<std::uint8_t> want = body;
  want[10] = 'X';
  const auto expect_crc = crc32c(want.data(), want.size());
  const std::uint8_t x = 'X';
  EXPECT_FALSE(store.install_range_version("i", 99, /*wrong tip*/ 0, 10, &x, 1, want.size(),
                                           expect_crc, {}, err));
  EXPECT_NE(err.find("range base mismatch"), std::string::npos);

  err.clear();
  ASSERT_TRUE(store.install_range_version("i", 2, tip->seq, 10, &x, 1, want.size(), expect_crc, {},
                                         err))
      << err;
  ASSERT_TRUE(store.publish_tip("i", 2, err)) << err;
  auto got = store.get("i", err);
  ASSERT_TRUE(got && *got == want);

  // Idempotent retry.
  ASSERT_TRUE(store.install_range_version("i", 2, tip->seq, 10, &x, 1, want.size(), expect_crc, {},
                                         err))
      << err;

  fs::remove_all(root);
}
