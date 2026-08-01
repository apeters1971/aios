#include "store/object_store.hpp"
#include "util/crc32c.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

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

int test_crc32c() {
  using namespace aios;

  // Known CRC32C("123456789") = 0xe3069283
  const char* s = "123456789";
  expect(crc32c(reinterpret_cast<const std::uint8_t*>(s), 9) == 0xe3069283u,
         "crc32c known vector");

  // Combine A||B
  const char* a = "12345";
  const char* b = "6789";
  const auto ca = crc32c(reinterpret_cast<const std::uint8_t*>(a), 5);
  const auto cb = crc32c(reinterpret_cast<const std::uint8_t*>(b), 4);
  expect(crc32c_combine(ca, cb, 4) == 0xe3069283u, "crc32c_combine");

  // Zeros update
  std::vector<std::uint8_t> z(100, 0);
  const auto cz = crc32c(z.data(), z.size());
  expect(crc32c_update_zeros(0, 100) == cz, "crc32c_update_zeros");

  // Store: full put + ranged update keep CRC correct
  const auto root = fs::temp_directory_path() / ("aios-crc-" + std::to_string(::getpid()));
  fs::create_directories(root);
  ObjectStore store;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.inline_max_bytes = 16;
  std::string err;
  expect(store.open(root.string(), opts, err), "open");

  const std::string body = "ABCDEFGH";
  expect(store.put("x", body, {}, true, err), "put");
  auto st = store.stat("x", err);
  expect(st && st->crc32c_known, "crc known");
  expect(st->crc32c == crc32c(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()),
         "crc matches body");

  // Reject bad expected crc
  expect(!store.put("y", reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), {},
                    true, std::optional<std::uint32_t>(1u), err),
         "reject bad crc");
  expect(err == "crc32c mismatch", "mismatch err");

  // Ranged overwrite then CRC must match full body
  const std::string patch = "zz";
  expect(store.put_range("x", 2, reinterpret_cast<const std::uint8_t*>(patch.data()),
                         patch.size(), {}, false, err),
         "put_range");
  st = store.stat("x", err);
  auto got = store.get("x", err);
  expect(got.has_value(), "get after range");
  expect(std::string(got->begin(), got->end()) == "ABzzEFGH", "body after range");
  expect(st->crc32c == crc32c(got->data(), got->size()), "crc after ranged update");

  // Grow with hole
  expect(store.put_range("x", 20, reinterpret_cast<const std::uint8_t*>("QQ"), 2, {}, false,
                         err),
         "grow range");
  got = store.get("x", err);
  st = store.stat("x", err);
  expect(got && got->size() == 22, "grown size");
  expect(st->crc32c == crc32c(got->data(), got->size()), "crc after grow");

  std::uint32_t recomputed = 0;
  expect(store.recompute_crc32c("x", recomputed, err), "recompute");
  expect(recomputed == st->crc32c, "recompute matches");

  // Empty body CRC
  expect(crc32c(nullptr, 0) == crc32c_update(0, nullptr, 0), "empty crc");
  expect(store.put("empty", std::string(), {}, true, err), "put empty");
  st = store.stat("empty", err);
  expect(st && st->crc32c == crc32c(nullptr, 0), "empty object crc");

  // Expected CRC success path
  const std::string okb = "ok-crc";
  const auto okc = crc32c(reinterpret_cast<const std::uint8_t*>(okb.data()), okb.size());
  expect(store.put("z", reinterpret_cast<const std::uint8_t*>(okb.data()), okb.size(), {},
                   true, std::optional<std::uint32_t>(okc), err),
         "put with matching crc");

  // Combine empty B
  expect(crc32c_combine(ca, 0, 0) == ca, "combine empty B");

  // Combine with zeros suffix
  const auto c_prefix = crc32c(reinterpret_cast<const std::uint8_t*>("AB"), 2);
  const auto c_full =
      crc32c(reinterpret_cast<const std::uint8_t*>("AB\0\0\0\0"), 6);
  expect(crc32c_combine(c_prefix, crc32c_update_zeros(0, 4), 4) == c_full,
         "combine zeros suffix");

  // Large zero span
  expect(crc32c_update_zeros(0, 100000) == crc32c(std::vector<std::uint8_t>(100000, 0).data(),
                                                  100000),
         "large zeros");

  fs::remove_all(root);
  return failures;
}
