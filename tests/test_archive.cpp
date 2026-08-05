#include "object/archive_bag.hpp"
#include "object/archive_pack.hpp"
#include "object/archive_tape.hpp"
#include "object/object_layout.hpp"
#include "test_helpers.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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

  return failures;
}
