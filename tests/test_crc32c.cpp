#include "store/object_store.hpp"
#include <gtest/gtest.h>
#include "util/crc32c.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;


TEST(Crc32c, Basic) {
  using namespace aios;

  // Known CRC32C("123456789") = 0xe3069283
  const char* s = "123456789";
  EXPECT_TRUE(crc32c(reinterpret_cast<const std::uint8_t*>(s), 9) == 0xe3069283u) << "crc32c known vector";

  // Combine A||B
  const char* a = "12345";
  const char* b = "6789";
  const auto ca = crc32c(reinterpret_cast<const std::uint8_t*>(a), 5);
  const auto cb = crc32c(reinterpret_cast<const std::uint8_t*>(b), 4);
  EXPECT_TRUE(crc32c_combine(ca, cb, 4) == 0xe3069283u) << "crc32c_combine";

  // Zeros update
  std::vector<std::uint8_t> z(100, 0);
  const auto cz = crc32c(z.data(), z.size());
  EXPECT_TRUE(crc32c_update_zeros(0, 100) == cz) << "crc32c_update_zeros";

  // Store: full put + ranged update keep CRC correct
  const auto root = fs::temp_directory_path() / ("aios-crc-" + std::to_string(::getpid()));
  fs::create_directories(root);
  ObjectStore store;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.inline_max_bytes = 16;
  std::string err;
  EXPECT_TRUE(store.open(root.string(), opts, err)) << "open";

  const std::string body = "ABCDEFGH";
  EXPECT_TRUE(store.put("x", body, {}, true, err)) << "put";
  auto st = store.stat("x", err);
  EXPECT_TRUE(st && st->crc32c_known) << "crc known";
  EXPECT_TRUE(st->crc32c == crc32c(reinterpret_cast<const std::uint8_t*>(body.data()), body.size())) << "crc matches body";

  // Reject bad expected crc
  EXPECT_TRUE(!store.put("y", reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), {},
                    true, std::optional<std::uint32_t>(1u), err)) << "reject bad crc";
  EXPECT_TRUE(err == "crc32c mismatch") << "mismatch err";

  // Ranged overwrite then CRC must match full body
  const std::string patch = "zz";
  EXPECT_TRUE(store.put_range("x", 2, reinterpret_cast<const std::uint8_t*>(patch.data()),
                         patch.size(), {}, false, err)) << "put_range";
  st = store.stat("x", err);
  auto got = store.get("x", err);
  EXPECT_TRUE(got.has_value()) << "get after range";
  EXPECT_TRUE(std::string(got->begin(), got->end()) == "ABzzEFGH") << "body after range";
  EXPECT_TRUE(st->crc32c == crc32c(got->data(), got->size())) << "crc after ranged update";

  // Grow with hole
  EXPECT_TRUE(store.put_range("x", 20, reinterpret_cast<const std::uint8_t*>("QQ"), 2, {}, false,
                         err)) << "grow range";
  got = store.get("x", err);
  st = store.stat("x", err);
  EXPECT_TRUE(got && got->size() == 22) << "grown size";
  EXPECT_TRUE(st->crc32c == crc32c(got->data(), got->size())) << "crc after grow";

  std::uint32_t recomputed = 0;
  EXPECT_TRUE(store.recompute_crc32c("x", recomputed, err)) << "recompute";
  EXPECT_TRUE(recomputed == st->crc32c) << "recompute matches";

  // Empty body CRC
  EXPECT_TRUE(crc32c(nullptr, 0) == crc32c_update(0, nullptr, 0)) << "empty crc";
  EXPECT_TRUE(store.put("empty", std::string(), {}, true, err)) << "put empty";
  st = store.stat("empty", err);
  EXPECT_TRUE(st && st->crc32c == crc32c(nullptr, 0)) << "empty object crc";

  // Expected CRC success path
  const std::string okb = "ok-crc";
  const auto okc = crc32c(reinterpret_cast<const std::uint8_t*>(okb.data()), okb.size());
  EXPECT_TRUE(store.put("z", reinterpret_cast<const std::uint8_t*>(okb.data()), okb.size(), {},
                   true, std::optional<std::uint32_t>(okc), err)) << "put with matching crc";

  // Combine empty B
  EXPECT_TRUE(crc32c_combine(ca, 0, 0) == ca) << "combine empty B";

  // Combine with zeros suffix
  const auto c_prefix = crc32c(reinterpret_cast<const std::uint8_t*>("AB"), 2);
  const auto c_full =
      crc32c(reinterpret_cast<const std::uint8_t*>("AB\0\0\0\0"), 6);
  EXPECT_TRUE(crc32c_combine(c_prefix, crc32c_update_zeros(0, 4), 4) == c_full) << "combine zeros suffix";

  // Large zero span
  EXPECT_TRUE(crc32c_update_zeros(0, 100000) == crc32c(std::vector<std::uint8_t>(100000, 0).data(),
                                                  100000)) << "large zeros";

  fs::remove_all(root);
  }
