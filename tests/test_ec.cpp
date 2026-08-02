#include "test_helpers.hpp"

#include "ec/codec_factory.hpp"
#include "ec/ec_attrs.hpp"
#include "ec/xor_parity.hpp"
#include "object/repair.hpp"
#include "util/crc32c.hpp"

#include <algorithm>
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

  explicit TripleStoreFixture(const std::string& ec_codec = "") {
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
    cfg.ec_codec = ec_codec;
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

void expect_roundtrip(const aios::ErasureCodec& codec, std::span<const std::uint8_t> payload,
                      const char* label) {
  using aios::test::expect;
  std::string err;
  std::vector<std::vector<std::uint8_t>> shards;
  expect(codec.encode(payload, shards, err), std::string(label) + " encode");
  expect(static_cast<int>(shards.size()) == codec.shard_count(),
         std::string(label) + " shard count");

  std::vector<std::optional<std::vector<std::uint8_t>>> all(shards.size());
  for (std::size_t i = 0; i < shards.size(); ++i) all[i] = shards[i];
  std::vector<std::uint8_t> out;
  expect(codec.decode(all, payload.size(), out, err), std::string(label) + " decode full");
  expect(out.size() == payload.size() &&
             std::equal(out.begin(), out.end(), payload.begin()),
         std::string(label) + " full match");
}

bool purge_shard_tip(aios::ObjectStore* store, const std::string& oid) {
  std::string err;
  auto st = store->stat(oid, err);
  if (!st) return false;
  return store->purge_version(oid, st->seq, true, err);
}

void test_service_degraded_and_repair(aios::ObjectService& svc, aios::Config& cfg,
                                      aios::ClusterMap& map, aios::LocalStores& stores,
                                      const std::string& oid, const std::uint8_t* body,
                                      std::size_t len, const char* label) {
  using namespace aios;
  using aios::test::expect;

  auto put = svc.api_put(oid, body, len, {{"k", "v"}}, true, {});
  expect(put.ok, std::string(label) + " put");
  expect(put.info && put.info->size == len, std::string(label) + " put size");

  auto got = svc.api_get(oid, std::nullopt, std::nullopt, {});
  expect(got.ok && got.data &&
             std::string(got.data->begin(), got.data->end()) ==
                 std::string(reinterpret_cast<const char*>(body), len),
         std::string(label) + " get");

  auto pl = place(oid, map);
  expect(static_cast<int>(pl.acting_set.size()) >= 3, std::string(label) + " acting");
  auto* victim = stores.get(pl.acting_set[2].aios_path);
  expect(purge_shard_tip(victim, oid), std::string(label) + " purge shard");
  std::string err;
  expect(!victim->stat(oid, err), std::string(label) + " shard gone");

  auto degraded = svc.api_get(oid, std::nullopt, std::nullopt, {});
  expect(degraded.ok && degraded.data &&
             std::string(degraded.data->begin(), degraded.data->end()) ==
                 std::string(reinterpret_cast<const char*>(body), len),
         std::string(label) + " degraded get");

  // Too few shards: drop a second one → GET fails.
  auto* victim2 = stores.get(pl.acting_set[1].aios_path);
  expect(purge_shard_tip(victim2, oid), std::string(label) + " purge second");
  auto fail = svc.api_get(oid, std::nullopt, std::nullopt, {});
  expect(!fail.ok, std::string(label) + " get fails with 2 missing");

  // Restore one shard via repair from the remaining copy, then GET works again.
  // Re-put to re-establish a healthy object for repair path testing below.
  expect(svc.api_put(oid, body, len, {}, true, {}).ok, std::string(label) + " re-put");
  expect(purge_shard_tip(stores.get(pl.acting_set[2].aios_path), oid),
         std::string(label) + " purge for repair");
  auto stats = run_repair(cfg, "127.0.0.1:7400", map, stores, 256);
  expect(stats.under_replicated >= 1, std::string(label) + " under-replicated");
  expect(stats.repaired >= 1, std::string(label) + " repaired");
  int present = 0;
  for (const auto& t : pl.acting_set) {
    if (stores.get(t.aios_path)->stat(oid, err)) ++present;
  }
  expect(present == 3, std::string(label) + " all shards after repair");
}

}  // namespace

int test_ec() {
  using namespace aios;
  using aios::test::expect;
  using aios::test::failures;
  failures() = 0;

  // --- Codec: empty, unaligned, too-many-missing, factory errors ---
  {
    std::string err;
    auto xorc = make_xor_parity_codec(2, err);
    expect(xorc != nullptr, "xor codec");

    // Empty object.
    {
      std::vector<std::uint8_t> empty;
      expect_roundtrip(*xorc, empty, "xor empty");
      std::vector<std::vector<std::uint8_t>> shards;
      expect(xorc->encode(empty, shards, err), "xor empty encode");
      std::vector<std::optional<std::vector<std::uint8_t>>> in(3);
      in[0] = shards[0];
      in[1] = shards[1];
      // parity missing
      std::vector<std::uint8_t> out;
      expect(xorc->decode(in, 0, out, err) && out.empty(), "xor empty degraded");
    }

    // Unaligned length (not divisible by k).
    {
      const auto* p = reinterpret_cast<const std::uint8_t*>("xyz");  // len 3, k=2
      expect_roundtrip(*xorc, std::span<const std::uint8_t>(p, 3), "xor unaligned");
      for (int missing = 0; missing < 3; ++missing) {
        std::vector<std::vector<std::uint8_t>> shards;
        expect(xorc->encode(std::span<const std::uint8_t>(p, 3), shards, err), "enc");
        std::vector<std::optional<std::vector<std::uint8_t>>> in(3);
        for (int i = 0; i < 3; ++i) {
          if (i != missing) in[static_cast<std::size_t>(i)] = shards[static_cast<std::size_t>(i)];
        }
        std::vector<std::uint8_t> out;
        expect(xorc->decode(in, 3, out, err), "xor unaligned miss " + std::to_string(missing));
        expect(std::string(out.begin(), out.end()) == "xyz", "xor unaligned body");
      }
    }

    // Too many missing shards.
    {
      const auto* p = reinterpret_cast<const std::uint8_t*>("abcdef");
      std::vector<std::vector<std::uint8_t>> shards;
      expect(xorc->encode(std::span<const std::uint8_t>(p, 6), shards, err), "enc");
      std::vector<std::optional<std::vector<std::uint8_t>>> in(3);
      in[0] = shards[0];  // only one present
      std::vector<std::uint8_t> out;
      expect(!xorc->decode(in, 6, out, err), "xor too many missing");
    }

    // Factory / config negatives.
    expect(make_erasure_codec(2, 2, "xor", err) == nullptr, "xor rejects m=2");
    expect(make_erasure_codec(0, 1, "", err) == nullptr, "k=0 rejected");
    {
      Config bad;
      bad.durability = "ec";
      bad.ec_k = 2;
      bad.ec_m = 2;
      bad.ec_codec = "xor";
      expect(!normalize_config(bad, err), "normalize xor+m=2 fails");
    }
    {
      Config ok;
      ok.durability = "ec";
      ok.ec_k = 2;
      ok.ec_m = 1;
      expect(normalize_config(ok, err) && ok.ec_codec == "xor" && ok.replica_count == 3,
             "normalize auto xor");
    }
  }

  // --- XOR codec single-loss + service degraded/repair ---
  {
    std::string err;
    auto codec = make_xor_parity_codec(2, err);
    const std::string payload = "erasure-coding-payload-12345";
    std::vector<std::vector<std::uint8_t>> shards;
    expect(codec->encode(std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(payload.data()),
                             payload.size()),
                         shards, err),
           "xor encode");
    for (int missing = 0; missing < 3; ++missing) {
      std::vector<std::optional<std::vector<std::uint8_t>>> in(3);
      for (int i = 0; i < 3; ++i) {
        if (i == missing) continue;
        in[static_cast<std::size_t>(i)] = shards[static_cast<std::size_t>(i)];
      }
      std::vector<std::uint8_t> out;
      expect(codec->decode(in, payload.size(), out, err), "xor miss decode");
      expect(std::string(out.begin(), out.end()) == payload, "xor miss body");
    }

    TripleStoreFixture fx;
    const auto* body = reinterpret_cast<const std::uint8_t*>("hello-ec-world!!");
    test_service_degraded_and_repair(*fx.svc, fx.cfg, fx.map, fx.stores, "ec/obj-xor", body, 16,
                                     "xor-svc");
    expect(!fx.svc->api_put_range("ec/obj-xor", 0, body, 1, {}, false, {}).ok,
           "xor range put rejected");
  }

  // --- ISA-L: 4+2 codec, empty/unaligned, degraded service + repair ---
  if (isal_ec_available()) {
    std::string err;
    auto codec = make_erasure_codec(4, 2, "isal", err);
    expect(codec != nullptr, "isal 4+2");

    std::vector<std::uint8_t> empty;
    expect_roundtrip(*codec, empty, "isal empty");

    const auto* odd = reinterpret_cast<const std::uint8_t*>("12345");  // 5 bytes, k=4
    expect_roundtrip(*codec, std::span<const std::uint8_t>(odd, 5), "isal unaligned");

    const std::string payload =
        "isal-reed-solomon-payload-abcdefghijklmnopqrstuvwxyz-0123456789";
    std::vector<std::vector<std::uint8_t>> shards;
    expect(codec->encode(std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(payload.data()),
                             payload.size()),
                         shards, err),
           "isal encode");
    expect(shards.size() == 6, "isal 6 shards");

    // Two missing (data + parity).
    {
      std::vector<std::optional<std::vector<std::uint8_t>>> in(6);
      for (int i = 0; i < 6; ++i) {
        if (i == 1 || i == 5) continue;
        in[static_cast<std::size_t>(i)] = shards[static_cast<std::size_t>(i)];
      }
      std::vector<std::uint8_t> out;
      expect(codec->decode(in, payload.size(), out, err), "isal decode 2 missing");
      expect(std::string(out.begin(), out.end()) == payload, "isal payload");
    }
    // Three missing → fail.
    {
      std::vector<std::optional<std::vector<std::uint8_t>>> in(6);
      in[0] = shards[0];
      in[2] = shards[2];
      in[3] = shards[3];
      std::vector<std::uint8_t> out;
      expect(!codec->decode(in, payload.size(), out, err), "isal too many missing");
    }

    {
      Config c;
      c.durability = "ec";
      c.ec_k = 4;
      c.ec_m = 2;
      expect(normalize_config(c, err) && c.ec_codec == "isal" && c.replica_count == 6,
             "normalize auto isal 4+2");
    }

    TripleStoreFixture fx("isal");
    expect(fx.cfg.ec_codec == "isal", "fixture isal codec");
    const auto* body = reinterpret_cast<const std::uint8_t*>("isal-two-plus-one!");
    test_service_degraded_and_repair(*fx.svc, fx.cfg, fx.map, fx.stores, "ec/obj-isal", body, 18,
                                     "isal-svc");
    auto got = fx.svc->api_get("ec/obj-isal", std::nullopt, std::nullopt, {});
    expect(got.ok && got.attrs.count("aios.ec.codec") && got.attrs.at("aios.ec.codec") == "isal",
           "isal attr after repair");
  } else {
    std::string err;
    expect(make_erasure_codec(4, 2, "isal", err) == nullptr, "isal unavailable without lib");
    Config c;
    c.durability = "ec";
    c.ec_k = 4;
    c.ec_m = 2;
    expect(!normalize_config(c, err), "normalize isal without lib fails");
  }

  return failures();
}
