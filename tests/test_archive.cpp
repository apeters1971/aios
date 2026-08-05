#include "object/archive_bag.hpp"
#include "object/archive_pack.hpp"
#include "object/archive_tape.hpp"
#include "object/object_layout.hpp"
#include "test_helpers.hpp"
#include "util/compression.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr const char* kTestBagKey =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
}  // namespace

namespace {

void write_executable(const std::filesystem::path& path, const std::string& body) {
  std::filesystem::create_directories(path.parent_path());
  {
    std::ofstream out(path);
    out << body;
  }
  std::filesystem::permissions(
      path, std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                std::filesystem::perms::others_exec);
}

}  // namespace

int test_archive() {
  using namespace aios;
  using namespace aios::test;
  int& failures = aios::test::failures();
  failures = 0;

  // Bag encode/decode round-trip.
  {
    std::vector<ArchiveMember> members(2);
    members[0].oid = "cold/a";
    members[0].data = {'a', 'a', 'a'};
    members[0].attrs["k"] = "v";
    members[1].oid = "cold/b";
    members[1].data = {'b', 'b', 'b', 'b'};
    std::vector<std::uint8_t> bag;
    std::string err;
    expect(encode_archive_bag(members, bag, err), "encode bag");
    ArchiveBag decoded;
    expect(decode_archive_bag(bag.data(), bag.size(), decoded, true, err), "decode bag");
    expect(decoded.members.size() == 2, "member count");
    expect(decoded.members[0].oid == "cold/a" && decoded.members[0].data.size() == 3, "m0");
    expect(decoded.members[1].oid == "cold/b" && decoded.members[1].data.size() == 4, "m1");
    expect(decoded.members[0].sha256_hex.size() == 64, "sha len");

    // Plain transform is identity (no AITF).
    {
      BagTransformOpts opts;
      std::vector<std::uint8_t> stored;
      std::unordered_map<std::string, std::string> attrs;
      expect(transform_bag_for_storage(bag, opts, "", stored, attrs, err), "xf none");
      expect(stored == bag, "plain identity");
      expect(!bag_body_is_transformed(stored.data(), stored.size()), "not AITF");
      std::vector<std::uint8_t> plain;
      expect(untransform_bag_from_storage(stored.data(), stored.size(), "", plain, err),
             "unxf none");
      expect(plain == bag, "plain roundtrip");
    }

    // AES-GCM only.
    {
      BagTransformOpts opts;
      opts.encryption = "aes-256-gcm";
      std::vector<std::uint8_t> stored;
      std::unordered_map<std::string, std::string> attrs;
      expect(transform_bag_for_storage(bag, opts, kTestBagKey, stored, attrs, err), "xf aes");
      expect(bag_body_is_transformed(stored.data(), stored.size()), "AITF aes");
      expect(attrs[kBagEncryptionAttr] == "aes-256-gcm", "enc attr");
      std::vector<std::uint8_t> plain;
      expect(untransform_bag_from_storage(stored.data(), stored.size(), kTestBagKey, plain, err),
             "unxf aes");
      expect(plain == bag, "aes roundtrip");
      std::string bad_err;
      expect(!untransform_bag_from_storage(
                 stored.data(), stored.size(),
                 "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", plain,
                 bad_err),
             "wrong key fails");
    }

#if defined(AIOS_HAVE_ZSTD) && AIOS_HAVE_ZSTD
    // ZSTD only + zstd+aes.
    if (zstd_available()) {
      BagTransformOpts opts;
      opts.compression = "zstd";
      opts.compression_level = 3;
      std::vector<std::uint8_t> stored;
      std::unordered_map<std::string, std::string> attrs;
      expect(transform_bag_for_storage(bag, opts, "", stored, attrs, err), "xf zstd");
      expect(bag_body_is_transformed(stored.data(), stored.size()), "AITF zstd");
      std::vector<std::uint8_t> plain;
      expect(untransform_bag_from_storage(stored.data(), stored.size(), "", plain, err),
             "unxf zstd");
      expect(plain == bag, "zstd roundtrip");

      opts.encryption = "aes-256-gcm";
      expect(transform_bag_for_storage(bag, opts, kTestBagKey, stored, attrs, err), "xf both");
      expect(untransform_bag_from_storage(stored.data(), stored.size(), kTestBagKey, plain, err),
             "unxf both");
      expect(plain == bag, "zstd+aes roundtrip");
    }
#endif
  }

  // Pack tips → bag stubs → GET from bag → recall rehydrate.
  {
    DualStoreFixture fx("archive-pack", 2, 2, "nvme");
    std::filesystem::create_directories(fx.root / "a1" / "aios");
    std::filesystem::create_directories(fx.root / "a2" / "aios");
    const std::string a1 = (fx.root / "a1" / "aios").string();
    const std::string a2 = (fx.root / "a2" / "aios").string();
    std::vector<AiosTarget> local{
        make_target(fx.p1, "nvme"),
        make_target(fx.p2, "nvme"),
        make_target(a1, "archive"),
        make_target(a2, "archive"),
    };
    fx.fs_table.set_local("node-a", local);
    PlacementConfig pc;
    pc.vnodes_per_target = fx.cfg.vnodes_per_target;
    pc.min_vnodes = fx.cfg.min_vnodes;
    pc.max_vnodes = fx.cfg.max_vnodes;
    fx.map = ClusterMap::build(fx.membership, fx.fs_table, fx.cfg.replica_count, pc);
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    fx.stores.sync_paths({fx.p1, fx.p2, a1, a2}, opts);

    ArchiveRule rule;
    rule.prefix = "cold/";
    rule.from = "nvme";
    rule.staging_class = "archive";
    rule.min_bag_bytes = 1;
    rule.max_members = 2;
    rule.max_open_ms = 0;
    fx.cfg.archive_rules.push_back(rule);

    fx.svc = std::make_unique<ObjectService>(fx.cfg, fx.map, fx.stores);
    fx.svc->set_advertise("127.0.0.1:7400");

    LayoutRequest req;
    req.storage_class = "nvme";
    const char* b1 = "hello-archive-1";
    const char* b2 = "hello-archive-2";
    expect(fx.svc
               ->api_put("cold/f1", reinterpret_cast<const std::uint8_t*>(b1), std::strlen(b1),
                         {}, true, {}, std::nullopt, req)
               .ok,
           "put f1");
    expect(fx.svc
               ->api_put("cold/f2", reinterpret_cast<const std::uint8_t*>(b2), std::strlen(b2),
                         {}, true, {}, std::nullopt, req)
               .ok,
           "put f2");

    auto stats = run_archive(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 64);
    expect(stats.bags_sealed >= 1, "bag sealed");
    expect(stats.packed >= 2, "members packed");

    auto g1 = fx.svc->api_get("cold/f1", std::nullopt, std::nullopt, {});
    expect(g1.ok && g1.data &&
               std::string(g1.data->begin(), g1.data->end()) == b1,
           "get frozen f1 from bag");
    expect(attrs_are_frozen(g1.attrs), "f1 frozen attrs");

    auto put_fail =
        fx.svc->api_put("cold/f1", reinterpret_cast<const std::uint8_t*>("x"), 1, {}, true, {},
                        std::nullopt, req);
    expect(!put_fail.ok && put_fail.code == "frozen", "put rejected while frozen");

    std::string err;
    expect(recall_archived_oid(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, "cold/f1", err),
           "recall f1");
    auto g1b = fx.svc->api_get("cold/f1", std::nullopt, std::nullopt, {});
    expect(g1b.ok && g1b.data &&
               std::string(g1b.data->begin(), g1b.data->end()) == b1,
           "get after recall");
    expect(!attrs_are_frozen(g1b.attrs), "not frozen after recall");
  }

  // Pack with AES-GCM (+ optional zstd) → GET frozen member still works.
  {
    DualStoreFixture fx("archive-xf", 2, 2, "nvme");
    std::filesystem::create_directories(fx.root / "a1" / "aios");
    std::filesystem::create_directories(fx.root / "a2" / "aios");
    const std::string a1 = (fx.root / "a1" / "aios").string();
    const std::string a2 = (fx.root / "a2" / "aios").string();
    std::vector<AiosTarget> local{
        make_target(fx.p1, "nvme"),
        make_target(fx.p2, "nvme"),
        make_target(a1, "archive"),
        make_target(a2, "archive"),
    };
    fx.fs_table.set_local("node-a", local);
    PlacementConfig pc;
    pc.vnodes_per_target = fx.cfg.vnodes_per_target;
    pc.min_vnodes = fx.cfg.min_vnodes;
    pc.max_vnodes = fx.cfg.max_vnodes;
    fx.map = ClusterMap::build(fx.membership, fx.fs_table, fx.cfg.replica_count, pc);
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    fx.stores.sync_paths({fx.p1, fx.p2, a1, a2}, opts);

    fx.cfg.bag_encryption_key = kTestBagKey;
    ArchiveRule rule;
    rule.prefix = "cold/";
    rule.from = "nvme";
    rule.staging_class = "archive";
    rule.min_bag_bytes = 1;
    rule.max_members = 2;
    rule.max_open_ms = 0;
    rule.bag_encryption = "aes-256-gcm";
#if defined(AIOS_HAVE_ZSTD) && AIOS_HAVE_ZSTD
    if (zstd_available()) rule.bag_compression = "zstd";
#endif
    fx.cfg.archive_rules.push_back(rule);

    fx.svc = std::make_unique<ObjectService>(fx.cfg, fx.map, fx.stores);
    fx.svc->set_advertise("127.0.0.1:7400");
    LayoutRequest req;
    req.storage_class = "nvme";
    const char* b1 = "secret-payload-one";
    const char* b2 = "secret-payload-two";
    expect(fx.svc
               ->api_put("cold/e1", reinterpret_cast<const std::uint8_t*>(b1), std::strlen(b1),
                         {}, true, {}, std::nullopt, req)
               .ok,
           "put e1");
    expect(fx.svc
               ->api_put("cold/e2", reinterpret_cast<const std::uint8_t*>(b2), std::strlen(b2),
                         {}, true, {}, std::nullopt, req)
               .ok,
           "put e2");

    expect(run_archive(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 64).bags_sealed >= 1,
           "xf bag sealed");
    auto g1 = fx.svc->api_get("cold/e1", std::nullopt, std::nullopt, {});
    expect(g1.ok && g1.data && std::string(g1.data->begin(), g1.data->end()) == b1,
           "get encrypted bag member");
    expect(attrs_are_frozen(g1.attrs), "e1 frozen");
  }

  // Pack → drain to tape_root → GET busy → recall restores bag and rehydrates.
  {
    DualStoreFixture fx("archive-tape", 2, 2, "nvme");
    std::filesystem::create_directories(fx.root / "a1" / "aios");
    std::filesystem::create_directories(fx.root / "a2" / "aios");
    const std::string a1 = (fx.root / "a1" / "aios").string();
    const std::string a2 = (fx.root / "a2" / "aios").string();
    const auto tape_root = fx.root / "tape";
    std::filesystem::create_directories(tape_root);
    std::vector<AiosTarget> local{
        make_target(fx.p1, "nvme"),
        make_target(fx.p2, "nvme"),
        make_target(a1, "archive"),
        make_target(a2, "archive"),
    };
    fx.fs_table.set_local("node-a", local);
    PlacementConfig pc;
    pc.vnodes_per_target = fx.cfg.vnodes_per_target;
    pc.min_vnodes = fx.cfg.min_vnodes;
    pc.max_vnodes = fx.cfg.max_vnodes;
    fx.map = ClusterMap::build(fx.membership, fx.fs_table, fx.cfg.replica_count, pc);
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    fx.stores.sync_paths({fx.p1, fx.p2, a1, a2}, opts);

    ArchiveRule rule;
    rule.prefix = "cold/";
    rule.from = "nvme";
    rule.staging_class = "archive";
    rule.min_bag_bytes = 1;
    rule.max_members = 2;
    rule.max_open_ms = 0;
    rule.tape_sink = "external";
    rule.tape_root = tape_root.string();
    fx.cfg.archive_rules.push_back(rule);

    fx.svc = std::make_unique<ObjectService>(fx.cfg, fx.map, fx.stores);
    fx.svc->set_advertise("127.0.0.1:7400");

    LayoutRequest req;
    req.storage_class = "nvme";
    const char* b1 = "tape-body-1";
    const char* b2 = "tape-body-2";
    expect(fx.svc
               ->api_put("cold/t1", reinterpret_cast<const std::uint8_t*>(b1), std::strlen(b1),
                         {}, true, {}, std::nullopt, req)
               .ok,
           "put t1");
    expect(fx.svc
               ->api_put("cold/t2", reinterpret_cast<const std::uint8_t*>(b2), std::strlen(b2),
                         {}, true, {}, std::nullopt, req)
               .ok,
           "put t2");

    auto stats = run_archive(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 64);
    expect(stats.bags_sealed >= 1, "tape bag sealed");

    std::string err;
    std::string bag_id;
    for (const auto& path : fx.stores.paths()) {
      auto* s = fx.stores.get(path);
      if (!s) continue;
      auto info = s->stat("cold/t1", err);
      if (!info || info->is_delete) continue;
      auto attrs = s->list_attrs("cold/t1", err);
      expect(archive_state_for_attrs(attrs) == kArchiveStateOnTape, "t1 on_tape after seal");
      bag_id = attrs[kBagIdAttr];
      break;
    }
    expect(!bag_id.empty(), "bag id from stub");

    auto drain = run_archive_drain(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 64);
    expect(drain.drained >= 1, "bag drained");

    std::uint64_t bag_size = 0;
    std::string tape_uri;
    for (const auto& path : fx.stores.paths()) {
      auto* s = fx.stores.get(path);
      if (!s) continue;
      auto info = s->stat(bag_id, err);
      if (!info || info->is_delete) continue;
      bag_size = info->size;
      auto attrs = s->list_attrs(bag_id, err);
      tape_uri = attrs[kTapeUriAttr];
      break;
    }
    expect(bag_size == 0, "staging bag body reclaimed");
    expect(!tape_uri.empty(), "tape_uri set");
    expect(std::filesystem::exists(tape_root / tape_uri), "tape file exists");

    auto gbusy = fx.svc->api_get("cold/t1", std::nullopt, std::nullopt, {});
    expect(!gbusy.ok && gbusy.code == "restoring", "get on_tape is restoring");

    expect(recall_archived_oid(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, "cold/t1", err),
           "recall t1 from tape");
    auto g1 = fx.svc->api_get("cold/t1", std::nullopt, std::nullopt, {});
    expect(g1.ok && g1.data && std::string(g1.data->begin(), g1.data->end()) == b1,
           "get t1 after tape recall");
    expect(!attrs_are_frozen(g1.attrs), "t1 unfrozen");

    // Second member still on tape; recall independently.
    expect(recall_archived_oid(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, "cold/t2", err),
           "recall t2");
    auto g2 = fx.svc->api_get("cold/t2", std::nullopt, std::nullopt, {});
    expect(g2.ok && g2.data && std::string(g2.data->begin(), g2.data->end()) == b2,
           "get t2 after tape recall");
  }

  // s3 driver via fake `aws s3 cp`.
  {
    DualStoreFixture fx("archive-s3", 2, 2, "nvme");
    std::filesystem::create_directories(fx.root / "a1" / "aios");
    std::filesystem::create_directories(fx.root / "a2" / "aios");
    const std::string a1 = (fx.root / "a1" / "aios").string();
    const std::string a2 = (fx.root / "a2" / "aios").string();
    const auto fake_root = fx.root / "fake-s3";
    const auto bin = fx.root / "bin" / "fake-aws";
    std::filesystem::create_directories(fake_root);
    write_executable(bin,
                     "#!/bin/sh\n"
                     "set -e\n"
                     "ROOT=\"${AIOS_FAKE_S3_ROOT:?}\"\n"
                     "# argv: s3 cp SRC DST [--endpoint-url URL]\n"
                     "[ \"$1\" = s3 ] && [ \"$2\" = cp ] || exit 3\n"
                     "src=\"$3\"; dst=\"$4\"\n"
                     "if echo \"$dst\" | grep -q '^s3://'; then\n"
                     "  key=$(echo \"$dst\" | sed 's|^s3://||')\n"
                     "  mkdir -p \"$(dirname \"$ROOT/$key\")\"\n"
                     "  cp \"$src\" \"$ROOT/$key\"\n"
                     "elif echo \"$src\" | grep -q '^s3://'; then\n"
                     "  key=$(echo \"$src\" | sed 's|^s3://||')\n"
                     "  cp \"$ROOT/$key\" \"$dst\"\n"
                     "else\n"
                     "  exit 2\n"
                     "fi\n");
    setenv("AIOS_FAKE_S3_ROOT", fake_root.string().c_str(), 1);

    std::vector<AiosTarget> local{
        make_target(fx.p1, "nvme"),
        make_target(fx.p2, "nvme"),
        make_target(a1, "archive"),
        make_target(a2, "archive"),
    };
    fx.fs_table.set_local("node-a", local);
    PlacementConfig pc;
    pc.vnodes_per_target = fx.cfg.vnodes_per_target;
    pc.min_vnodes = fx.cfg.min_vnodes;
    pc.max_vnodes = fx.cfg.max_vnodes;
    fx.map = ClusterMap::build(fx.membership, fx.fs_table, fx.cfg.replica_count, pc);
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    fx.stores.sync_paths({fx.p1, fx.p2, a1, a2}, opts);

    ArchiveRule rule;
    rule.prefix = "cold/";
    rule.from = "nvme";
    rule.staging_class = "archive";
    rule.min_bag_bytes = 1;
    rule.max_members = 1;
    rule.max_open_ms = 0;
    rule.tape_sink = "s3";
    rule.tape_uri_prefix = "s3://cold/bags/";
    rule.tape_bin = bin.string();
    rule.tape_s3_endpoint = "http://127.0.0.1:9";
    rule.tape_root = (fx.root / "scratch").string();
    fx.cfg.archive_rules.push_back(rule);

    fx.svc = std::make_unique<ObjectService>(fx.cfg, fx.map, fx.stores);
    fx.svc->set_advertise("127.0.0.1:7400");
    LayoutRequest req;
    req.storage_class = "nvme";
    const char* body = "s3-bag-payload";
    expect(fx.svc
               ->api_put("cold/s3a", reinterpret_cast<const std::uint8_t*>(body),
                         std::strlen(body), {}, true, {}, std::nullopt, req)
               .ok,
           "put s3a");
    expect(run_archive(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 64).bags_sealed >= 1,
           "s3 bag sealed");
    expect(run_archive_drain(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 64).drained >= 1,
           "s3 drained");

    std::string err;
    std::string tape_uri;
    for (const auto& path : fx.stores.paths()) {
      auto* s = fx.stores.get(path);
      if (!s) continue;
      auto info = s->stat("cold/s3a", err);
      if (!info || info->is_delete) continue;
      auto attrs = s->list_attrs("cold/s3a", err);
      for (const auto& p2 : fx.stores.paths()) {
        auto* bagstore = fx.stores.get(p2);
        if (!bagstore) continue;
        auto bi = bagstore->stat(attrs[kBagIdAttr], err);
        if (!bi || bi->is_delete) continue;
        auto ba = bagstore->list_attrs(attrs[kBagIdAttr], err);
        tape_uri = ba[kTapeUriAttr];
        expect(bi->size == 0, "s3 staging reclaimed");
        break;
      }
      break;
    }
    expect(tape_uri.rfind("s3://cold/bags/", 0) == 0, "s3 tape_uri");
    const std::string key = tape_uri.substr(std::strlen("s3://"));
    expect(std::filesystem::exists(fake_root / key), "fake s3 object");

    expect(recall_archived_oid(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, "cold/s3a", err),
           "recall s3a");
    auto g = fx.svc->api_get("cold/s3a", std::nullopt, std::nullopt, {});
    expect(g.ok && g.data && std::string(g.data->begin(), g.data->end()) == body,
           "get after s3 recall");
  }

  // xrdcp driver via fake binary.
  {
    DualStoreFixture fx("archive-xrdcp", 2, 2, "nvme");
    std::filesystem::create_directories(fx.root / "a1" / "aios");
    std::filesystem::create_directories(fx.root / "a2" / "aios");
    const std::string a1 = (fx.root / "a1" / "aios").string();
    const std::string a2 = (fx.root / "a2" / "aios").string();
    const auto fake_root = fx.root / "fake-xrd";
    const auto bin = fx.root / "bin" / "fake-xrdcp";
    std::filesystem::create_directories(fake_root);
    write_executable(bin,
                     "#!/bin/sh\n"
                     "set -e\n"
                     "ROOT=\"${AIOS_FAKE_XRD_ROOT:?}\"\n"
                     "while [ \"$1\" = -f ] || [ \"$1\" = -s ]; do shift; done\n"
                     "src=\"$1\"; dst=\"$2\"\n"
                     "to_local() { echo \"$1\" | sed -E 's|^x?root://[^/]+/*|/|'; }\n"
                     "if echo \"$dst\" | grep -Eq '^x?root://'; then\n"
                     "  rel=$(to_local \"$dst\")\n"
                     "  mkdir -p \"$(dirname \"$ROOT$rel\")\"\n"
                     "  cp \"$src\" \"$ROOT$rel\"\n"
                     "elif echo \"$src\" | grep -Eq '^x?root://'; then\n"
                     "  rel=$(to_local \"$src\")\n"
                     "  cp \"$ROOT$rel\" \"$dst\"\n"
                     "else\n"
                     "  exit 2\n"
                     "fi\n");
    setenv("AIOS_FAKE_XRD_ROOT", fake_root.string().c_str(), 1);

    std::vector<AiosTarget> local{
        make_target(fx.p1, "nvme"),
        make_target(fx.p2, "nvme"),
        make_target(a1, "archive"),
        make_target(a2, "archive"),
    };
    fx.fs_table.set_local("node-a", local);
    PlacementConfig pc;
    pc.vnodes_per_target = fx.cfg.vnodes_per_target;
    pc.min_vnodes = fx.cfg.min_vnodes;
    pc.max_vnodes = fx.cfg.max_vnodes;
    fx.map = ClusterMap::build(fx.membership, fx.fs_table, fx.cfg.replica_count, pc);
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    fx.stores.sync_paths({fx.p1, fx.p2, a1, a2}, opts);

    ArchiveRule rule;
    rule.prefix = "cold/";
    rule.from = "nvme";
    rule.staging_class = "archive";
    rule.min_bag_bytes = 1;
    rule.max_members = 1;
    rule.max_open_ms = 0;
    rule.tape_sink = "xrdcp";
    rule.tape_uri_prefix = "root://eos.test//eos/archive/bags/";
    rule.tape_bin = bin.string();
    rule.tape_root = (fx.root / "scratch").string();
    fx.cfg.archive_rules.push_back(rule);

    fx.svc = std::make_unique<ObjectService>(fx.cfg, fx.map, fx.stores);
    fx.svc->set_advertise("127.0.0.1:7400");
    LayoutRequest req;
    req.storage_class = "nvme";
    const char* body = "xrd-bag-payload";
    expect(fx.svc
               ->api_put("cold/x1", reinterpret_cast<const std::uint8_t*>(body),
                         std::strlen(body), {}, true, {}, std::nullopt, req)
               .ok,
           "put x1");
    expect(run_archive(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 64).bags_sealed >= 1,
           "xrd bag sealed");
    expect(run_archive_drain(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 64).drained >= 1,
           "xrd drained");

    std::string err;
    std::string tape_uri;
    for (const auto& path : fx.stores.paths()) {
      auto* s = fx.stores.get(path);
      if (!s) continue;
      auto info = s->stat("cold/x1", err);
      if (!info || info->is_delete) continue;
      auto attrs = s->list_attrs("cold/x1", err);
      for (const auto& p2 : fx.stores.paths()) {
        auto* bagstore = fx.stores.get(p2);
        if (!bagstore) continue;
        auto bi = bagstore->stat(attrs[kBagIdAttr], err);
        if (!bi || bi->is_delete) continue;
        auto ba = bagstore->list_attrs(attrs[kBagIdAttr], err);
        tape_uri = ba[kTapeUriAttr];
        break;
      }
      break;
    }
    expect(tape_uri.rfind("root://eos.test//eos/archive/bags/", 0) == 0, "xrd tape_uri");
    // root://eos.test//eos/archive/bags/NAME → eos/archive/bags/NAME under fake root
    const std::string host_end = "root://eos.test/";
    expect(tape_uri.size() > host_end.size(), "xrd uri length");
    std::string rel = tape_uri.substr(host_end.size());
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    expect(std::filesystem::exists(fake_root / rel), "fake xrd object");

    expect(recall_archived_oid(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, "cold/x1", err),
           "recall x1");
    auto g = fx.svc->api_get("cold/x1", std::nullopt, std::nullopt, {});
    expect(g.ok && g.data && std::string(g.data->begin(), g.data->end()) == body,
           "get after xrd recall");
  }

  return failures;
}
