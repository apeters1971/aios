#include "store/body_log.hpp"
#include "store/io_engine.hpp"
#include "store/object_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

TEST(BodyLog, LocatorRoundTrip) {
  aios::BodyLocation loc;
  loc.segment_id = 42;
  loc.offset = 16;
  loc.length = 4096;
  const auto s = aios::format_segment_locator(loc);
  EXPECT_TRUE(aios::is_segment_locator(s));
  aios::BodyLocation got;
  ASSERT_TRUE(aios::parse_segment_locator(s, got));
  EXPECT_EQ(got.segment_id, 42u);
  EXPECT_EQ(got.offset, 16u);
  EXPECT_EQ(got.length, 4096u);
  EXPECT_FALSE(aios::is_segment_locator("objects/ab/cd/hash/v1"));
  EXPECT_FALSE(aios::parse_segment_locator("objects/x", got));
}

TEST(BodyLog, AppendReadAndBatch) {
  const auto root = fs::temp_directory_path() / ("aios-blog-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  aios::PosixIoEngine io;
  aios::BodyLog log;
  std::string err;
  ASSERT_TRUE(log.open(root.string(), &io, /*data_fsync=*/false, 1 << 20, err)) << err;

  const std::string a(32, 'A');
  const std::string b(64, 'B');
  aios::BodyLocation la, lb;
  ASSERT_TRUE(log.append({reinterpret_cast<const std::uint8_t*>(a.data()), a.size()}, la, err))
      << err;
  ASSERT_TRUE(log.append({reinterpret_cast<const std::uint8_t*>(b.data()), b.size()}, lb, err))
      << err;
  EXPECT_EQ(la.length, 32u);
  EXPECT_EQ(lb.length, 64u);
  EXPECT_NE(la.offset, lb.offset);

  std::vector<std::uint8_t> ga(32), gb(64);
  ASSERT_TRUE(log.read(la, 0, ga.size(), ga.data(), err)) << err;
  ASSERT_TRUE(log.read(lb, 0, gb.size(), gb.data(), err)) << err;
  EXPECT_EQ(std::string(ga.begin(), ga.end()), a);
  EXPECT_EQ(std::string(gb.begin(), gb.end()), b);

  std::vector<std::uint8_t> slice(8);
  ASSERT_TRUE(log.read(la, 4, slice.size(), slice.data(), err)) << err;
  EXPECT_EQ(std::string(slice.begin(), slice.end()), std::string(8, 'A'));

  // Past-EOF zeros (delta growth).
  std::vector<std::uint8_t> tail(8, 0xff);
  ASSERT_TRUE(log.read(la, 32, tail.size(), tail.data(), err)) << err;
  EXPECT_EQ(tail, std::vector<std::uint8_t>(8, 0));

  std::vector<std::uint8_t> ba(32), bb(64);
  aios::BodyLog::ReadOp ops[2];
  ops[0].loc = la;
  ops[0].buf = std::span<std::uint8_t>(ba.data(), ba.size());
  ops[1].loc = lb;
  ops[1].buf = std::span<std::uint8_t>(bb.data(), bb.size());
  ASSERT_TRUE(log.read_many(ops, err)) << err;
  EXPECT_EQ(std::string(ba.begin(), ba.end()), a);
  EXPECT_EQ(std::string(bb.begin(), bb.end()), b);

  log.close();
  fs::remove_all(root);
}

TEST(BodyLog, RotateWhenFull) {
  const auto root = fs::temp_directory_path() / ("aios-brot-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  aios::PosixIoEngine io;
  aios::BodyLog log;
  std::string err;
  const std::uint64_t seg = 64 * 1024;
  ASSERT_TRUE(log.open(root.string(), &io, false, seg, err)) << err;
  const std::string chunk(20 * 1024, 'R');
  aios::BodyLocation first, last;
  ASSERT_TRUE(
      log.append({reinterpret_cast<const std::uint8_t*>(chunk.data()), chunk.size()}, first, err))
      << err;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(
        log.append({reinterpret_cast<const std::uint8_t*>(chunk.data()), chunk.size()}, last, err))
        << err;
  }
  EXPECT_GT(last.segment_id, first.segment_id);
  std::vector<std::uint8_t> got(chunk.size());
  ASSERT_TRUE(log.read(last, 0, got.size(), got.data(), err)) << err;
  EXPECT_EQ(got, std::vector<std::uint8_t>(chunk.begin(), chunk.end()));
  log.close();
  fs::remove_all(root);
}

TEST(ObjectStore, SegmentBodiesAndGetMany) {
  using namespace aios;
  const auto root = fs::temp_directory_path() / ("aios-seg-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.data_fsync = false;
  ObjectStore store;
  std::string err;
  ASSERT_TRUE(store.open(root.string(), opts, err)) << err;

  const std::unordered_map<std::string, std::string> attrs{{"k", "v"}};
  ASSERT_TRUE(store.put("s1", std::string(100, '1'), attrs, true, err)) << err;
  ASSERT_TRUE(store.put("s2", std::string(200, '2'), attrs, true, err)) << err;
  ASSERT_TRUE(store.put("s3", std::string(50, '3'), attrs, true, err)) << err;

  auto i1 = store.stat("s1", err);
  ASSERT_TRUE(i1 && i1->segment_body);
  EXPECT_FALSE(store.fs_body_path("s1", err).has_value());

  auto many = store.get_many({"s1", "missing", "s2", "s3"}, err);
  ASSERT_EQ(many.size(), 4u);
  ASSERT_TRUE(many[0] && many[0]->size() == 100);
  EXPECT_EQ((*many[0])[0], '1');
  EXPECT_FALSE(many[1].has_value());
  ASSERT_TRUE(many[2] && many[2]->size() == 200);
  EXPECT_EQ((*many[2])[0], '2');
  ASSERT_TRUE(many[3] && many[3]->size() == 50);

  // Range write keeps SQLite deltas over the packed base.
  const std::string patch(16, 'P');
  ASSERT_TRUE(store.put_range("s1", 10, reinterpret_cast<const std::uint8_t*>(patch.data()),
                              patch.size(), {}, false, err))
      << err;
  auto st = store.stat("s1", err);
  ASSERT_TRUE(st && st->delta && st->segment_body);
  auto got = store.get("s1", err);
  ASSERT_TRUE(got && got->size() == 100);
  EXPECT_EQ(std::string(got->begin() + 10, got->begin() + 26), patch);
  EXPECT_EQ((*got)[0], '1');

  // force_mode=inline still uses a SQLite BLOB.
  store.close();
  const auto inline_root = root / "inline";
  fs::create_directories(inline_root);
  ObjectStoreOptions io;
  io.shard_count = 2;
  io.force_mode = "inline";
  io.data_fsync = false;
  ObjectStore s2;
  ASSERT_TRUE(s2.open(inline_root.string(), io, err)) << err;
  ASSERT_TRUE(s2.put("blob", std::string(8, 'x'), {}, true, err)) << err;
  auto ib = s2.stat("blob", err);
  ASSERT_TRUE(ib && ib->inline_body && !ib->segment_body);

  fs::remove_all(root);
}
