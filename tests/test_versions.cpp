#include "store/fs_clone.hpp"
#include "store/object_store.hpp"
#include "util/crc32c.hpp"

#include <filesystem>
#include <iostream>
#include <string>
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

int test_versions() {
  using namespace aios;
  const auto base = fs::temp_directory_path() / "aios-versions-test";
  fs::remove_all(base);
  fs::create_directories(base);

  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.inline_max_bytes = 256;
  opts.max_versions = 3;
  opts.clone_required = false;  // allow copy fallback in CI

  ObjectStore store;
  std::string err;
  expect(store.open(base.string(), opts, err), "open");

  // Inline multi-version put + list + get-by-seq + tip.
  expect(store.put("vobj", std::string("aaa"), {{"n", "1"}}, true, err), "put1");
  expect(store.put("vobj", std::string("bbb"), {{"n", "2"}}, true, err), "put2");
  expect(store.put("vobj", std::string("ccc"), {{"n", "3"}}, true, err), "put3");

  auto tip = store.stat("vobj", err);
  expect(tip && tip->seq == 3 && tip->size == 3, "tip seq3");
  auto body = store.get("vobj", err);
  expect(body && std::string(body->begin(), body->end()) == "ccc", "tip body");

  auto v1 = store.get("vobj", 1, err);
  expect(v1 && std::string(v1->begin(), v1->end()) == "aaa", "get seq1");
  auto v2 = store.get("vobj", 2, err);
  expect(v2 && std::string(v2->begin(), v2->end()) == "bbb", "get seq2");

  auto vers = store.list_versions("vobj", err);
  expect(vers.size() == 3, "3 versions");
  expect(!vers.empty() && vers[0].seq == 3, "newest first");

  // Trim to N via extra puts (max_versions=3).
  expect(store.put("vobj", std::string("ddd"), {}, true, err), "put4");
  vers = store.list_versions("vobj", err);
  expect(vers.size() == 3, "trimmed to 3");
  expect(store.get("vobj", 1, err) == std::nullopt, "seq1 purged");
  tip = store.stat("vobj", err);
  expect(tip && tip->seq == 4, "tip seq4");

  // Prepare / abort leaves tip unchanged.
  PreparedVersion pv;
  expect(store.prepare_put("vobj", reinterpret_cast<const std::uint8_t*>("zzz"), 3, {},
                           true, std::nullopt, pv, err),
         "prepare");
  expect(pv.seq == 5, "prepared seq5");
  expect(store.abort_version("vobj", pv.seq, err), "abort");
  tip = store.stat("vobj", err);
  expect(tip && tip->seq == 4, "tip unchanged after abort");
  expect(store.get("vobj", 5, err) == std::nullopt, "aborted seq gone");
  body = store.get("vobj", err);
  expect(body && std::string(body->begin(), body->end()) == "ddd", "tip body after abort");

  // Publish-after-prepare.
  expect(store.prepare_put("vobj", reinterpret_cast<const std::uint8_t*>("eee"), 3, {},
                           true, std::nullopt, pv, err),
         "prepare2");
  expect(store.publish_tip("vobj", pv.seq, err), "publish");
  tip = store.stat("vobj", err);
  expect(tip && tip->seq == pv.seq, "tip after publish");

  // Delete marker.
  expect(store.del("vobj", err), "del marker");
  expect(store.stat("vobj", err) == std::nullopt, "tip hidden after delete");
  vers = store.list_versions("vobj", err);
  expect(!vers.empty() && vers[0].is_delete, "newest is delete marker");

  // FS COW range + CRC.
  {
    const auto fsroot = base / "fs";
    fs::create_directories(fsroot);
    ObjectStoreOptions fo = opts;
    fo.force_mode = "fs";
    fo.inline_max_bytes = 1;
    fo.max_versions = 8;
    fo.clone_required = false;
    ObjectStore fsstore;
    expect(fsstore.open(fsroot.string(), fo, err), "open fs");
    const std::string big(1024, 'A');
    expect(fsstore.put("cow", big, {}, true, err), "put big");
    const std::string patch = "ZZZZ";
    expect(fsstore.put_range("cow", 100, reinterpret_cast<const std::uint8_t*>(patch.data()),
                             patch.size(), {}, false, err),
           "put_range");
    auto st = fsstore.stat("cow", err);
    expect(st && st->seq == 2 && st->size == 1024, "cow tip");
    expect(st->crc32c_known, "crc known");
    auto got = fsstore.get("cow", err);
    expect(got && got->size() == 1024, "cow get size");
    expect(got && std::string(got->begin() + 100, got->begin() + 104) == "ZZZZ",
           "cow patch");
    expect(got && (*got)[0] == 'A' && (*got)[1023] == 'A', "cow unchanged ends");
    const auto expect_crc = crc32c(got->data(), got->size());
    expect(st->crc32c == expect_crc, "crc matches body");

    auto old = fsstore.get("cow", 1, err);
    expect(old && std::string(old->begin(), old->end()) == big, "seq1 immutable");
  }

  expect(clone_file_supported() || !opts.clone_required, "clone support note");

  // Redirect versions.
  {
    expect(store.put("target", std::string("hello"), {}, true, err), "put target");
    expect(store.put_redirect("alias", "target", {}, true, nullptr, err), "put_redirect");
    auto st = store.stat("alias", err);
    expect(st && st->redirect_oid == "target", "stat redirect");
    expect(store.get("alias", err) == std::nullopt, "get redirect fails");
    expect(err == "object is redirect", "redirect err");
    expect(!store.put_redirect("alias", "alias", {}, true, nullptr, err), "no self");
    // Replace redirect with real body.
    expect(store.put("alias", std::string("real"), {}, true, err), "replace redirect");
    auto body2 = store.get("alias", err);
    expect(body2 && std::string(body2->begin(), body2->end()) == "real", "real body");
    st = store.stat("alias", err);
    expect(st && st->redirect_oid.empty(), "no longer redirect");
  }

  fs::remove_all(base);
  return failures;
}
