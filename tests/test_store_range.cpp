#include "store/object_store.hpp"
#include <gtest/gtest.h>

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
