#include "store/object_store.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
}

}  // namespace

int test_object_store() {
  using namespace aios;
  const auto base = fs::temp_directory_path() / "aios-store-test";
  fs::remove_all(base);
  fs::create_directories(base);

  ObjectStoreOptions opts;
  opts.shard_count = 8;
  opts.inline_max_bytes = 1024;

  ObjectStore store;
  std::string err;
  expect(store.open(base.string(), opts, err), "open store");
  if (!err.empty() && !store.is_open()) {
    std::cerr << "open err: " << err << "\n";
  }
  expect(fs::is_regular_file(base / "store.json"), "store.json written");

  {
    std::unordered_map<std::string, std::string> attrs{{"k", "v"}, {"n", "1"}};
    expect(store.put("small-1", std::string(32, 'x'), attrs, true, err), "put small");
    auto info = store.stat("small-1", err);
    expect(info.has_value(), "stat small");
    expect(info && info->inline_body, "small is inline");
    expect(info && info->size == 32, "small size");
    expect(info && info->shard < 8, "shard in range");

    auto body = store.get("small-1", err);
    expect(body && body->size() == 32, "get small");
    auto v = store.get_attr("small-1", "k", err);
    expect(v && *v == "v", "attr k");
  }

  {
    const std::string big(2048, 'y');
    expect(store.put("big-1", big, {{"kind", "big"}}, true, err), "put big");
    auto info = store.stat("big-1", err);
    expect(info.has_value() && info && !info->inline_body, "big on fs");
    expect(info && !info->fs_path.empty(), "fs_path set");
    if (info) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%x", info->shard);
      const auto path = fs::path(store.root()) / "shards" / buf / info->fs_path;
      expect(fs::is_regular_file(path), "fs body file exists");
    }

    auto body = store.get("big-1", err);
    expect(body && body->size() == 2048, "get big");
  }

  {
    std::unordered_set<std::uint32_t> seen;
    for (int i = 0; i < 64; ++i) {
      const std::string oid = "spread-" + std::to_string(i);
      expect(store.put(oid, std::string("z"), {}, true, err), "put spread");
      auto info = store.stat(oid, err);
      expect(info.has_value(), "stat spread");
      if (info) seen.insert(info->shard);
    }
    expect(seen.size() > 1, "multiple shards used");
  }

  {
    expect(store.del("small-1", err), "del small");
    expect(store.del("big-1", err), "del big");
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
    expect(s2.open(forced.string(), fo, err), "open forced");
    expect(s2.put("tiny", std::string("ab"), {}, true, err), "put tiny fs");
    auto info = s2.stat("tiny", err);
    expect(info && !info->inline_body, "forced fs");
  }

  {
    ObjectStore s3;
    ObjectStoreOptions o2;
    o2.shard_count = 64;  // ignored; store.json says 8
    expect(s3.open(base.string(), o2, err), "reopen");
    expect(s3.shard_count() == 8, "shard_count persisted");
  }

  // shard_of_oid stability
  expect(shard_of_oid("abc", 8) == shard_of_oid("abc", 8), "stable shard");
  expect(shard_of_oid("abc", 8) < 8, "shard range");

  fs::remove_all(base);
  return failures;
}
