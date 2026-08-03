#include "test_helpers.hpp"

#include "cluster/place.hpp"
#include "net/framing.hpp"
#include "store/object_store.hpp"
#include "util/base64.hpp"
#include "util/crc32c.hpp"

#include <cstdint>
#include <string>
#include <vector>

int test_object_rpc_advanced() {
  using namespace aios;
  using aios::test::DualStoreFixture;
  using aios::test::expect;
  using aios::test::failures;

  failures() = 0;

  DualStoreFixture fx("aios-rpc-adv");
  auto& svc = *fx.svc;
  auto& map = fx.map;

  const std::string oid = "rpc-adv-1";
  auto placement = place(oid, map, "nvme");
  expect(placement.acting_set.size() == 2, "acting set 2");
  const auto& primary = placement.acting_set[0];

  // ObjectPut primary → ok + seq
  Frame put;
  put.type = MsgType::ObjectPut;
  put.body = {
      {"epoch", map.epoch},
      {"aios_path", primary.aios_path},
      {"oid", oid},
      {"data_b64", base64_encode(std::string("alpha"))},
      {"attrs", {{"k", "1"}}},
      {"role", "primary"},
      {"crc32c", crc32c(reinterpret_cast<const std::uint8_t*>("alpha"), 5)},
  };
  auto put_reply = svc.handle(put);
  expect(put_reply.type == MsgType::ObjectReply, "put reply type");
  expect(put_reply.body.value("ok", false), "put ok");
  expect(put_reply.body.contains("seq"), "put seq");
  const auto seq1 = put_reply.body.value("seq", static_cast<std::uint64_t>(0));
  expect(seq1 == 1, "put seq1");

  // Second put for multi-version.
  put.body["data_b64"] = base64_encode(std::string("bravo!!"));
  put.body["crc32c"] = crc32c(reinterpret_cast<const std::uint8_t*>("bravo!!"), 7);
  put.body["attrs"] = {{"k", "2"}};
  auto put2 = svc.handle(put);
  expect(put2.body.value("ok", false), "put2 ok");
  const auto seq2 = put2.body.value("seq", static_cast<std::uint64_t>(0));
  expect(seq2 == 2, "put seq2");

  // ObjectStat with seq
  Frame st;
  st.type = MsgType::ObjectStat;
  st.body = {
      {"epoch", map.epoch},
      {"aios_path", primary.aios_path},
      {"oid", oid},
      {"seq", seq1},
  };
  auto st_reply = svc.handle(st);
  expect(st_reply.body.value("ok", false), "stat seq ok");
  expect(st_reply.body.value("size", 0u) == 5, "stat seq size");
  expect(st_reply.body.value("seq", static_cast<std::uint64_t>(0)) == seq1, "stat seq value");

  // ObjectGet with seq
  Frame get;
  get.type = MsgType::ObjectGet;
  get.body = {
      {"epoch", map.epoch},
      {"aios_path", primary.aios_path},
      {"oid", oid},
      {"seq", seq1},
  };
  auto get_reply = svc.handle(get);
  expect(get_reply.body.value("ok", false), "get seq ok");
  std::vector<std::uint8_t> data;
  std::string derr;
  expect(base64_decode(get_reply.body["data_b64"].get<std::string>(), data, derr),
         "get decode");
  expect(std::string(data.begin(), data.end()) == "alpha", "get seq body");

  // Replica install: prepare_put on primary, install on secondary, publish both.
  {
    const std::string roid = "rpc-install";
    auto pl = place(roid, map, "nvme");
    expect(pl.acting_set.size() == 2, "install acting 2");
    const auto& prim = pl.acting_set[0];
    const auto& sec = pl.acting_set[1];
    auto* store = fx.stores.get(prim.aios_path);
    expect(store != nullptr, "install primary store");
    if (store) {
      std::string err;
      expect(store->put(roid, std::string("base"), {}, true, err), "seed tip");
      PreparedVersion pv;
      const std::string payload = "installed";
      expect(store->prepare_put(roid, reinterpret_cast<const std::uint8_t*>(payload.data()),
                                payload.size(), {}, true, std::nullopt, pv, err),
             "prepare_put for install");

      Frame install;
      install.type = MsgType::ObjectPut;
      install.body = {
          {"epoch", map.epoch},
          {"aios_path", sec.aios_path},
          {"oid", roid},
          {"seq", pv.seq},
          {"base_seq", pv.prev_tip},
          {"size", pv.size},
          {"crc32c", pv.crc32c},
          {"inline_body", pv.inline_body},
          {"fs_path", pv.fs_path},
          {"is_delete", false},
          {"data_b64", base64_encode(payload)},
          {"role", "replica"},
      };
      auto inst_reply = svc.handle(install);
      expect(inst_reply.body.value("ok", false), "replica install ok");

      for (const auto& t : {prim, sec}) {
        Frame pub;
        pub.type = MsgType::ObjectPublishTip;
        pub.body = {
            {"epoch", map.epoch},
            {"aios_path", t.aios_path},
            {"oid", roid},
            {"seq", pv.seq},
        };
        auto pub_reply = svc.handle(pub);
        expect(pub_reply.body.value("ok", false), "publish tip ok");
      }

      Frame tip_get;
      tip_get.type = MsgType::ObjectGet;
      tip_get.body = {
          {"epoch", map.epoch},
          {"aios_path", prim.aios_path},
          {"oid", roid},
      };
      auto tip_reply = svc.handle(tip_get);
      expect(tip_reply.body.value("ok", false), "tip readable after publish");
      data.clear();
      expect(base64_decode(tip_reply.body["data_b64"].get<std::string>(), data, derr),
             "tip decode");
      expect(std::string(data.begin(), data.end()) == "installed", "tip body installed");
    }
  }

  // ObjectAbortVersion: prepare on primary, install on secondary, abort both.
  {
    const std::string aoid = "rpc-abort";
    auto pl = place(aoid, map, "nvme");
    const auto& prim = pl.acting_set[0];
    const auto& sec = pl.acting_set[1];

    Frame seed;
    seed.type = MsgType::ObjectPut;
    seed.body = {
        {"epoch", map.epoch},
        {"aios_path", prim.aios_path},
        {"oid", aoid},
        {"data_b64", base64_encode(std::string("keep-me"))},
        {"role", "primary"},
    };
    auto seed_r = svc.handle(seed);
    expect(seed_r.body.value("ok", false), "abort seed put");
    const auto tip_seq = seed_r.body.value("seq", static_cast<std::uint64_t>(0));

    auto* store = fx.stores.get(prim.aios_path);
    expect(store != nullptr, "abort store");
    if (store) {
      std::string err;
      PreparedVersion pv;
      const std::string payload = "doomed";
      expect(store->prepare_put(aoid, reinterpret_cast<const std::uint8_t*>(payload.data()),
                                payload.size(), {}, true, std::nullopt, pv, err),
             "prepare for abort");

      Frame install;
      install.type = MsgType::ObjectPut;
      install.body = {
          {"epoch", map.epoch},
          {"aios_path", sec.aios_path},
          {"oid", aoid},
          {"seq", pv.seq},
          {"base_seq", pv.prev_tip},
          {"size", pv.size},
          {"crc32c", pv.crc32c},
          {"inline_body", pv.inline_body},
          {"fs_path", pv.fs_path},
          {"data_b64", base64_encode(payload)},
          {"role", "replica"},
      };
      expect(svc.handle(install).body.value("ok", false), "abort path install");

      for (const auto& t : {prim, sec}) {
        Frame abort;
        abort.type = MsgType::ObjectAbortVersion;
        abort.body = {
            {"epoch", map.epoch},
            {"aios_path", t.aios_path},
            {"oid", aoid},
            {"seq", pv.seq},
        };
        expect(svc.handle(abort).body.value("ok", false), "abort version ok");
      }

      Frame tip_get;
      tip_get.type = MsgType::ObjectGet;
      tip_get.body = {
          {"epoch", map.epoch},
          {"aios_path", prim.aios_path},
          {"oid", aoid},
      };
      auto tip_reply = svc.handle(tip_get);
      expect(tip_reply.body.value("ok", false), "tip still readable");
      expect(tip_reply.body.value("seq", static_cast<std::uint64_t>(0)) == tip_seq,
             "tip unchanged after abort");
      data.clear();
      expect(base64_decode(tip_reply.body["data_b64"].get<std::string>(), data, derr),
             "abort tip decode");
      expect(std::string(data.begin(), data.end()) == "keep-me", "abort tip body");
    }
  }

  // ObjectListVersions → versions array
  Frame lv;
  lv.type = MsgType::ObjectListVersions;
  lv.body = {
      {"epoch", map.epoch},
      {"aios_path", primary.aios_path},
      {"oid", oid},
  };
  auto lv_reply = svc.handle(lv);
  expect(lv_reply.body.value("ok", false), "list versions ok");
  expect(lv_reply.body.contains("versions") && lv_reply.body["versions"].is_array(),
         "versions array");
  expect(lv_reply.body["versions"].size() >= 2, "at least 2 versions");

  // ObjectPurgeVersions with keep
  Frame purge;
  purge.type = MsgType::ObjectPurgeVersions;
  purge.body = {
      {"epoch", map.epoch},
      {"aios_path", primary.aios_path},
      {"oid", oid},
      {"keep", 1},
  };
  auto purge_reply = svc.handle(purge);
  expect(purge_reply.body.value("ok", false), "purge keep ok");
  lv_reply = svc.handle(lv);
  expect(lv_reply.body.value("ok", false), "list after purge");
  expect(lv_reply.body["versions"].size() == 1, "kept 1 version");

  // ObjectPut with redirect field (no data_b64) as primary
  {
    const std::string target = "rpc-redir-target";
    const std::string alias = "rpc-redir-alias";
    auto tpl = place(target, map, "nvme");
    Frame tput;
    tput.type = MsgType::ObjectPut;
    tput.body = {
        {"epoch", map.epoch},
        {"aios_path", tpl.acting_set[0].aios_path},
        {"oid", target},
        {"data_b64", base64_encode(std::string("tgt"))},
        {"role", "primary"},
    };
    expect(svc.handle(tput).body.value("ok", false), "redirect target put");

    auto apl = place(alias, map, "nvme");
    Frame rput;
    rput.type = MsgType::ObjectPut;
    rput.body = {
        {"epoch", map.epoch},
        {"aios_path", apl.acting_set[0].aios_path},
        {"oid", alias},
        {"redirect", target},
        {"role", "primary"},
    };
    auto rr = svc.handle(rput);
    expect(rr.body.value("ok", false), "redirect put ok");
    expect(rr.body.value("redirect", "") == target, "redirect field in reply");

    Frame rget;
    rget.type = MsgType::ObjectGet;
    rget.body = {
        {"epoch", map.epoch},
        {"aios_path", apl.acting_set[0].aios_path},
        {"oid", alias},
    };
    auto rg = svc.handle(rget);
    expect(rg.body.value("ok", false), "get redirect ok");
    expect(rg.body.value("code", "") == "redirect", "get redirect code");
    expect(rg.body.value("redirect", "") == target, "get redirect field");
  }

  // ObjectPutRange with kFlagRawBody + raw bytes (primary)
  {
    const std::string rid = "rpc-range";
    auto rpl = place(rid, map, "nvme");
    Frame full;
    full.type = MsgType::ObjectPut;
    full.body = {
        {"epoch", map.epoch},
        {"aios_path", rpl.acting_set[0].aios_path},
        {"oid", rid},
        {"data_b64", base64_encode(std::string("0123456789"))},
        {"role", "primary"},
    };
    expect(svc.handle(full).body.value("ok", false), "range base put");

    Frame range;
    range.type = MsgType::ObjectPutRange;
    range.flags = kFlagRawBody;
    range.body = {
        {"epoch", map.epoch},
        {"aios_path", rpl.acting_set[0].aios_path},
        {"oid", rid},
        {"offset", 2},
        {"role", "primary"},
        {"replace_attrs", false},
    };
    const std::string chunk = "XX";
    range.raw.assign(chunk.begin(), chunk.end());
    auto rr = svc.handle(range);
    expect(rr.body.value("ok", false), "put range ok");

    Frame g;
    g.type = MsgType::ObjectGet;
    g.body = {
        {"epoch", map.epoch},
        {"aios_path", rpl.acting_set[0].aios_path},
        {"oid", rid},
    };
    auto gr = svc.handle(g);
    expect(gr.body.value("ok", false), "get after range");
    data.clear();
    expect(base64_decode(gr.body["data_b64"].get<std::string>(), data, derr), "range decode");
    expect(std::string(data.begin(), data.end()) == "01XX456789", "range patched");
  }

  // Epoch mismatch on ObjectListVersions
  Frame bad;
  bad.type = MsgType::ObjectListVersions;
  bad.body = {
      {"epoch", map.epoch + 1},
      {"aios_path", primary.aios_path},
      {"oid", oid},
  };
  auto bad_reply = svc.handle(bad);
  expect(!bad_reply.body.value("ok", true), "epoch mismatch fails");
  expect(bad_reply.body.value("code", "") == "epoch_mismatch", "epoch code");

  return failures();
}
