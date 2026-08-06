#include "store/fs_clone.hpp"
#include <gtest/gtest.h>
#include "store/object_store.hpp"
#include "util/crc32c.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;


TEST(Versions, Basic) {
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
  EXPECT_TRUE(store.open(base.string(), opts, err)) << "open";

  // Inline multi-version put + list + get-by-seq + tip.
  EXPECT_TRUE(store.put("vobj", std::string("aaa"), {{"n", "1"}}, true, err)) << "put1";
  EXPECT_TRUE(store.put("vobj", std::string("bbb"), {{"n", "2"}}, true, err)) << "put2";
  EXPECT_TRUE(store.put("vobj", std::string("ccc"), {{"n", "3"}}, true, err)) << "put3";

  auto tip = store.stat("vobj", err);
  EXPECT_TRUE(tip && tip->seq == 3 && tip->size == 3) << "tip seq3";
  auto body = store.get("vobj", err);
  EXPECT_TRUE(body && std::string(body->begin(), body->end()) == "ccc") << "tip body";

  auto v1 = store.get("vobj", 1, err);
  EXPECT_TRUE(v1 && std::string(v1->begin(), v1->end()) == "aaa") << "get seq1";
  auto v2 = store.get("vobj", 2, err);
  EXPECT_TRUE(v2 && std::string(v2->begin(), v2->end()) == "bbb") << "get seq2";

  auto vers = store.list_versions("vobj", err);
  EXPECT_TRUE(vers.size() == 3) << "3 versions";
  EXPECT_TRUE(!vers.empty() && vers[0].seq == 3) << "newest first";

  // Trim to N via extra puts (max_versions=3).
  EXPECT_TRUE(store.put("vobj", std::string("ddd"), {}, true, err)) << "put4";
  vers = store.list_versions("vobj", err);
  EXPECT_TRUE(vers.size() == 3) << "trimmed to 3";
  EXPECT_TRUE(store.get("vobj", 1, err) == std::nullopt) << "seq1 purged";
  tip = store.stat("vobj", err);
  EXPECT_TRUE(tip && tip->seq == 4) << "tip seq4";

  // Prepare / abort leaves tip unchanged.
  PreparedVersion pv;
  EXPECT_TRUE(store.prepare_put("vobj", reinterpret_cast<const std::uint8_t*>("zzz"), 3, {},
                           true, std::nullopt, pv, err)) << "prepare";
  EXPECT_TRUE(pv.seq == 5) << "prepared seq5";
  EXPECT_TRUE(store.abort_version("vobj", pv.seq, err)) << "abort";
  tip = store.stat("vobj", err);
  EXPECT_TRUE(tip && tip->seq == 4) << "tip unchanged after abort";
  EXPECT_TRUE(store.get("vobj", 5, err) == std::nullopt) << "aborted seq gone";
  body = store.get("vobj", err);
  EXPECT_TRUE(body && std::string(body->begin(), body->end()) == "ddd") << "tip body after abort";

  // Publish-after-prepare.
  EXPECT_TRUE(store.prepare_put("vobj", reinterpret_cast<const std::uint8_t*>("eee"), 3, {},
                           true, std::nullopt, pv, err)) << "prepare2";
  EXPECT_TRUE(store.publish_tip("vobj", pv.seq, err)) << "publish";
  tip = store.stat("vobj", err);
  EXPECT_TRUE(tip && tip->seq == pv.seq) << "tip after publish";

  // Delete marker.
  EXPECT_TRUE(store.del("vobj", err)) << "del marker";
  EXPECT_TRUE(store.stat("vobj", err) == std::nullopt) << "tip hidden after delete";
  vers = store.list_versions("vobj", err);
  EXPECT_TRUE(!vers.empty() && vers[0].is_delete) << "newest is delete marker";

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
    EXPECT_TRUE(fsstore.open(fsroot.string(), fo, err)) << "open fs";
    const std::string big(1024, 'A');
    EXPECT_TRUE(fsstore.put("cow", big, {}, true, err)) << "put big";
    const std::string patch = "ZZZZ";
    EXPECT_TRUE(fsstore.put_range("cow", 100, reinterpret_cast<const std::uint8_t*>(patch.data()),
                             patch.size(), {}, false, err)) << "put_range";
    auto st = fsstore.stat("cow", err);
    EXPECT_TRUE(st && st->seq == 2 && st->size == 1024) << "cow tip";
    EXPECT_TRUE(st->crc32c_known) << "crc known";
    auto got = fsstore.get("cow", err);
    EXPECT_TRUE(got && got->size() == 1024) << "cow get size";
    EXPECT_TRUE(got && std::string(got->begin() + 100, got->begin() + 104) == "ZZZZ") << "cow patch";
    EXPECT_TRUE(got && (*got)[0] == 'A' && (*got)[1023] == 'A') << "cow unchanged ends";
    const auto expect_crc = crc32c(got->data(), got->size());
    EXPECT_TRUE(st->crc32c == expect_crc) << "crc matches body";

    auto old = fsstore.get("cow", 1, err);
    EXPECT_TRUE(old && std::string(old->begin(), old->end()) == big) << "seq1 immutable";
  }

  EXPECT_TRUE(clone_file_supported() || !opts.clone_required) << "clone support note";

  // Redirect versions.
  {
    EXPECT_TRUE(store.put("target", std::string("hello"), {}, true, err)) << "put target";
    EXPECT_TRUE(store.put_redirect("alias", "target", {}, true, nullptr, err)) << "put_redirect";
    auto st = store.stat("alias", err);
    EXPECT_TRUE(st && st->redirect_oid == "target") << "stat redirect";
    EXPECT_TRUE(store.get("alias", err) == std::nullopt) << "get redirect fails";
    EXPECT_TRUE(err == "object is redirect") << "redirect err";
    EXPECT_TRUE(!store.put_redirect("alias", "alias", {}, true, nullptr, err)) << "no self";
    // Replace redirect with real body.
    EXPECT_TRUE(store.put("alias", std::string("real"), {}, true, err)) << "replace redirect";
    auto body2 = store.get("alias", err);
    EXPECT_TRUE(body2 && std::string(body2->begin(), body2->end()) == "real") << "real body";
    st = store.stat("alias", err);
    EXPECT_TRUE(st && st->redirect_oid.empty()) << "no longer redirect";
  }

  fs::remove_all(base);
  }
