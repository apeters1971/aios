#include "test_helpers.hpp"

#include "ec/codec_factory.hpp"
#include "ec/ec_attrs.hpp"
#include "ec/xor_parity.hpp"
#include "object/repair.hpp"
#include "util/crc32c.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

struct TripleStoreFixture {
  std::filesystem::path root;
  std::string p1, p2, p3;
  aios::MembershipTable membership;
  aios::FsTable fs_table;
  aios::Config cfg;
  aios::ClusterMap map;
  aios::LocalStores stores;
  std::unique_ptr<aios::ObjectService> svc;

  TripleStoreFixture() {
    using namespace aios;
    root = aios::test::temp_root("aios-ec");
    std::filesystem::create_directories(root / "t1" / "aios");
    std::filesystem::create_directories(root / "t2" / "aios");
    std::filesystem::create_directories(root / "t3" / "aios");
    p1 = (root / "t1" / "aios").string();
    p2 = (root / "t2" / "aios").string();
    p3 = (root / "t3" / "aios").string();

    membership.set_local("node-a", "127.0.0.1:7400");
    std::vector<AiosTarget> local;
    for (const auto& path : {p1, p2, p3}) {
      AiosTarget t;
      t.mount = path;
      t.target_path = path;
      t.aios_path = path;
      t.usable = true;
      t.bavail = 1000;
      local.push_back(t);
    }
    fs_table.set_local("node-a", local);

    cfg.node_id = "node-a";
    cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
    cfg.durability = "ec";
    cfg.ec_k = 2;
    cfg.ec_m = 1;
    std::string err;
    aios::test::expect(normalize_config(cfg, err), "normalize ec config");
    cfg.max_versions = 16;
    cfg.clone_required = false;

    map = ClusterMap::build(membership, fs_table, cfg.replica_count);
    ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    stores.sync_paths({p1, p2, p3}, opts);
    svc = std::make_unique<ObjectService>(cfg, map, stores);
    svc->set_advertise("127.0.0.1:7400");
  }

  ~TripleStoreFixture() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

}  // namespace

int test_ec() {
  using namespace aios;
  using aios::test::expect;
  using aios::test::failures;
  failures() = 0;

  // Codec: round-trip and single-shard loss.
  {
    std::string err;
    auto codec = make_xor_parity_codec(2, err);
    expect(codec != nullptr, "make xor codec");
    const std::string payload = "erasure-coding-payload-12345";
    std::vector<std::vector<std::uint8_t>> shards;
    expect(codec->encode(std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(payload.data()),
                             payload.size()),
                         shards, err),
           "encode");
    expect(shards.size() == 3, "3 shards");

    for (int missing = 0; missing < 3; ++missing) {
      std::vector<std::optional<std::vector<std::uint8_t>>> in(3);
      for (int i = 0; i < 3; ++i) {
        if (i == missing) continue;
        in[static_cast<std::size_t>(i)] = shards[static_cast<std::size_t>(i)];
      }
      std::vector<std::uint8_t> out;
      expect(codec->decode(in, payload.size(), out, err),
             "decode missing " + std::to_string(missing));
      expect(std::string(out.begin(), out.end()) == payload,
             "payload match missing " + std::to_string(missing));
    }
  }

  // ISA-L Reed–Solomon 4+2 (skipped when libisal not linked).
  if (isal_ec_available()) {
    std::string err;
    auto codec = make_erasure_codec(4, 2, "isal", err);
    expect(codec != nullptr, "make isal 4+2");
    const std::string payload =
        "isal-reed-solomon-payload-abcdefghijklmnopqrstuvwxyz-0123456789";
    std::vector<std::vector<std::uint8_t>> shards;
    expect(codec->encode(std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(payload.data()),
                             payload.size()),
                         shards, err),
           "isal encode");
    expect(shards.size() == 6, "isal 6 shards");

    // Drop two shards (data + parity).
    {
      std::vector<std::optional<std::vector<std::uint8_t>>> in(6);
      for (int i = 0; i < 6; ++i) {
        if (i == 1 || i == 5) continue;
        in[static_cast<std::size_t>(i)] = shards[static_cast<std::size_t>(i)];
      }
      std::vector<std::uint8_t> out;
      expect(codec->decode(in, payload.size(), out, err), "isal decode 2 missing");
      expect(std::string(out.begin(), out.end()) == payload, "isal payload match");
    }

    // Service path with isal 2+1 on three local targets.
    {
      TripleStoreFixture fx;
      fx.cfg.ec_codec = "isal";
      fx.svc = std::make_unique<ObjectService>(fx.cfg, fx.map, fx.stores);
      fx.svc->set_advertise("127.0.0.1:7400");
      const std::string oid = "ec/isal-2+1";
      const auto* body = reinterpret_cast<const std::uint8_t*>("isal-two-plus-one!");
      const std::size_t len = 18;
      auto put = fx.svc->api_put(oid, body, len, {}, true, {});
      expect(put.ok, "isal service put");
      auto got = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
      expect(got.ok && got.data.has_value(), "isal service get");
      expect(std::string(got.data->begin(), got.data->end()) ==
                 std::string(reinterpret_cast<const char*>(body), len),
             "isal service body");
      expect(got.attrs.count("aios.ec.codec") && got.attrs.at("aios.ec.codec") == "isal",
             "isal attr");
    }
  } else {
    std::string err;
    expect(make_erasure_codec(4, 2, "isal", err) == nullptr, "isal unavailable without lib");
  }

  TripleStoreFixture fx;
  auto& svc = *fx.svc;
  const std::string oid = "ec/obj-1";
  const auto* body = reinterpret_cast<const std::uint8_t*>("hello-ec-world!!");
  const std::size_t len = 16;

  auto put = svc.api_put(oid, body, len, {{"k", "v"}}, true, {});
  expect(put.ok, "ec put");
  expect(put.info && put.info->size == len, "ec put reports full size");
  expect(put.replicas == 3, "ec put all shards");

  // Each acting-set target holds a shard (different sizes/crcs OK).
  {
    auto pl = place(oid, fx.map);
    expect(pl.acting_set.size() == 3, "acting set 3");
    int present = 0;
    std::string err;
    for (const auto& t : pl.acting_set) {
      auto* s = fx.stores.get(t.aios_path);
      expect(s != nullptr, "store");
      if (s->stat(oid, err)) ++present;
      auto attrs = s->list_attrs(oid, err);
      expect(attrs_are_ec(attrs), "shard has ec attrs");
    }
    expect(present == 3, "all shards present after put");
  }

  auto got = svc.api_get(oid, std::nullopt, std::nullopt, {});
  expect(got.ok && got.data.has_value(), "ec get");
  expect(std::string(got.data->begin(), got.data->end()) ==
             std::string(reinterpret_cast<const char*>(body), len),
         "ec get body");
  expect(got.info && got.info->size == len, "ec get full size");

  // Drop one shard and reconstruct via GET.
  {
    auto pl = place(oid, fx.map);
    auto* victim = fx.stores.get(pl.acting_set[2].aios_path);
    std::string err;
    auto st = victim->stat(oid, err);
    expect(st.has_value(), "victim tip");
    expect(victim->purge_version(oid, st->seq, true, err), "purge shard tip");
    expect(!victim->stat(oid, err), "shard gone");

    auto degraded = svc.api_get(oid, std::nullopt, std::nullopt, {});
    expect(degraded.ok && degraded.data.has_value(), "ec get degraded");
    expect(std::string(degraded.data->begin(), degraded.data->end()) ==
               std::string(reinterpret_cast<const char*>(body), len),
           "ec degraded body");
  }

  // Repair reconstructs the missing shard.
  {
    auto stats = run_repair(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 256);
    expect(stats.under_replicated >= 1, "repair saw under-replicated");
    expect(stats.repaired >= 1, "ec repaired");
    auto pl = place(oid, fx.map);
    std::string err;
    int present = 0;
    for (const auto& t : pl.acting_set) {
      if (fx.stores.get(t.aios_path)->stat(oid, err)) ++present;
    }
    expect(present == 3, "all shards after repair");
  }

  // Ranged put rejected on EC.
  expect(!svc.api_put_range(oid, 0, body, 1, {}, false, {}).ok, "ec range put rejected");

  return failures();
}
