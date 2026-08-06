#include "store/object_store.hpp"
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;


TEST(ObjectStore, Basic) {
  using namespace aios;
  const auto base = fs::temp_directory_path() / "aios-store-test";
  fs::remove_all(base);
  fs::create_directories(base);

  ObjectStoreOptions opts;
  opts.shard_count = 8;
  opts.inline_max_bytes = 1024;

  ObjectStore store;
  std::string err;
  EXPECT_TRUE(store.open(base.string(), opts, err)) << "open store";
  if (!err.empty() && !store.is_open()) {
    std::cerr << "open err: " << err << "\n";
  }
  EXPECT_TRUE(fs::is_regular_file(base / "store.json")) << "store.json written";

  {
    std::unordered_map<std::string, std::string> attrs{{"k", "v"}, {"n", "1"}};
    EXPECT_TRUE(store.put("small-1", std::string(32, 'x'), attrs, true, err)) << "put small";
    auto info = store.stat("small-1", err);
    EXPECT_TRUE(info.has_value()) << "stat small";
    EXPECT_TRUE(info && info->inline_body) << "small is inline";
    EXPECT_TRUE(info && info->size == 32) << "small size";
    EXPECT_TRUE(info && info->shard < 8) << "shard in range";

    auto body = store.get("small-1", err);
    EXPECT_TRUE(body && body->size() == 32) << "get small";
    auto v = store.get_attr("small-1", "k", err);
    EXPECT_TRUE(v && *v == "v") << "attr k";
  }

  {
    const std::string big(2048, 'y');
    EXPECT_TRUE(store.put("big-1", big, {{"kind", "big"}}, true, err)) << "put big";
    auto info = store.stat("big-1", err);
    EXPECT_TRUE(info.has_value() && info && !info->inline_body) << "big on fs";
    EXPECT_TRUE(info && !info->fs_path.empty()) << "fs_path set";
    if (info) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%x", info->shard);
      const auto path = fs::path(store.root()) / "shards" / buf / info->fs_path;
      EXPECT_TRUE(fs::is_regular_file(path)) << "fs body file exists";
    }

    auto body = store.get("big-1", err);
    EXPECT_TRUE(body && body->size() == 2048) << "get big";
  }

  {
    std::unordered_set<std::uint32_t> seen;
    for (int i = 0; i < 64; ++i) {
      const std::string oid = "spread-" + std::to_string(i);
      EXPECT_TRUE(store.put(oid, std::string("z"), {}, true, err)) << "put spread";
      auto info = store.stat(oid, err);
      EXPECT_TRUE(info.has_value()) << "stat spread";
      if (info) seen.insert(info->shard);
    }
    EXPECT_TRUE(seen.size() > 1) << "multiple shards used";
  }

  {
    EXPECT_TRUE(store.del("small-1", err)) << "del small";
    EXPECT_TRUE(store.del("big-1", err)) << "del big";
  }

  {
    store.close();
    const auto forced = base / "forced";
    fs::create_directories(forced);
    ObjectStoreOptions fo;
    fo.shard_count = 4;
    fo.force_mode = "fs";
    fo.inline_max_bytes = 1 << 20;
    ObjectStore s2;
    EXPECT_TRUE(s2.open(forced.string(), fo, err)) << "open forced";
    EXPECT_TRUE(s2.put("tiny", std::string("ab"), {}, true, err)) << "put tiny fs";
    auto info = s2.stat("tiny", err);
    EXPECT_TRUE(info && !info->inline_body) << "forced fs";
  }

  {
    ObjectStore s3;
    ObjectStoreOptions o2;
    o2.shard_count = 64;  // ignored; store.json says 8
    EXPECT_TRUE(s3.open(base.string(), o2, err)) << "reopen";
    EXPECT_TRUE(s3.shard_count() == 8) << "shard_count persisted";
  }

  // shard_of_oid stability
  EXPECT_TRUE(shard_of_oid("abc", 8) == shard_of_oid("abc", 8)) << "stable shard";
  EXPECT_TRUE(shard_of_oid("abc", 8) < 8) << "shard range";

  fs::remove_all(base);
  }
