#include "test_helpers.hpp"

#include "cluster/place.hpp"
#include "store/object_store.hpp"
#include "util/crc32c.hpp"

#include <cstdint>
#include <string>
#include <vector>

int test_object_service_advanced() {
  using namespace aios;
  using aios::test::DualStoreFixture;
  using aios::test::expect;
  using aios::test::failures;

  failures() = 0;

  DualStoreFixture fx("aios-svc-adv");
  auto& svc = *fx.svc;

  const std::string oid = "adv/obj-1";
  const auto* body1 = reinterpret_cast<const std::uint8_t*>("version-one");
  const auto* body2 = reinterpret_cast<const std::uint8_t*>("version-two-longer");
  const auto* body3 = reinterpret_cast<const std::uint8_t*>("v3");

  auto p1 = svc.api_put(oid, body1, 11, {{"n", "1"}}, true, {});
  expect(p1.ok, "api_put v1");
  expect(p1.replicas == 2, "put v1 quorum");

  auto vers = svc.api_list_versions(oid);
  expect(vers.ok && vers.versions.size() == 1, "list_versions size 1");
  expect(!vers.versions.empty() && vers.versions[0].seq == 1, "newest first seq1");

  auto p2 = svc.api_put(oid, body2, 17, {{"n", "2"}}, true, {});
  expect(p2.ok, "api_put v2");
  vers = svc.api_list_versions(oid);
  expect(vers.ok && vers.versions.size() == 2, "list_versions size grows to 2");
  expect(!vers.versions.empty() && vers.versions[0].seq == 2, "newest first seq2");
  expect(vers.versions.size() >= 2 && vers.versions[0].size >= vers.versions[1].size,
         "newest size >= older");

  auto p3 = svc.api_put(oid, body3, 2, {{"n", "3"}}, true, {});
  expect(p3.ok, "api_put v3");
  vers = svc.api_list_versions(oid);
  expect(vers.ok && vers.versions.size() == 3, "list_versions size 3");
  expect(!vers.versions.empty() && vers.versions[0].seq == 3, "newest first seq3");

  auto old = svc.api_get(oid, std::nullopt, std::nullopt, {}, /*seq=*/1);
  expect(old.ok && old.data.has_value(), "get seq1");
  expect(old.data && std::string(old.data->begin(), old.data->end()) == "version-one",
         "get seq1 body");

  auto head = svc.api_head(oid, {});
  expect(head.ok && head.info.has_value(), "api_head info");
  expect(head.info && head.info->seq == 3, "head tip seq");
  expect(head.info && head.info->size == 2, "head tip size");

  const std::string alias = "adv/alias";
  auto redir = svc.api_put_redirect(alias, oid, {{"kind", "link"}}, true, {});
  expect(redir.ok, "api_put_redirect");
  auto rget = svc.api_get(alias, std::nullopt, std::nullopt, {});
  expect(rget.ok && rget.code == "redirect", "get redirect code");
  expect(rget.redirect_oid == oid, "get redirect oid");

  // Purge tip rejected when allow_tip=false; purge older ok.
  auto tip_purge = svc.api_purge_version(oid, 3, /*allow_tip=*/false);
  expect(!tip_purge.ok, "purge tip rejected");
  auto old_purge = svc.api_purge_version(oid, 1, /*allow_tip=*/false);
  expect(old_purge.ok, "purge older version");
  vers = svc.api_list_versions(oid);
  expect(vers.ok && vers.versions.size() == 2, "after purge older, 2 versions");

  auto trim = svc.api_trim_versions(oid, 1);
  expect(trim.ok, "api_trim_versions keep=1");
  vers = svc.api_list_versions(oid);
  expect(vers.ok && vers.versions.size() == 1, "trimmed to 1");
  expect(!vers.versions.empty() && vers.versions[0].seq == 3, "kept tip");

  auto del = svc.api_del(oid, {});
  expect(del.ok, "api_del");
  auto tip_get = svc.api_get(oid, std::nullopt, std::nullopt, {});
  expect(!tip_get.ok, "get tip fails after delete marker");

  // Preconditions: MustNotExist on create; conflict on second put.
  const std::string create_oid = "adv/create-once";
  std::vector<AttrPrecondition> must_new = {
      {AttrPrecondition::Kind::MustNotExist, {}, {}},
  };
  auto c1 = svc.api_put(create_oid, body1, 11, {}, true, must_new);
  expect(c1.ok, "create MustNotExist");
  auto c2 = svc.api_put(create_oid, body2, 17, {}, true, must_new);
  expect(!c2.ok && c2.code == "precondition_failed", "second MustNotExist conflicts");

  // api_put_range with quorum.
  const std::string range_oid = "adv/range";
  auto base = svc.api_put(range_oid, reinterpret_cast<const std::uint8_t*>("ABCDEFGH"), 8, {},
                          true, {});
  expect(base.ok && base.replicas == 2, "range base put");
  const std::string patch = "xy";
  auto pr = svc.api_put_range(range_oid, 2, reinterpret_cast<const std::uint8_t*>(patch.data()),
                              patch.size(), {}, false, {});
  expect(pr.ok && pr.replicas == 2, "api_put_range quorum");
  auto ranged = svc.api_get(range_oid, std::nullopt, std::nullopt, {});
  expect(ranged.ok && ranged.data.has_value(), "get after range");
  expect(ranged.data && std::string(ranged.data->begin(), ranged.data->end()) == "ABxyEFGH",
         "range body");

  // Replace redirect with api_put body.
  auto replace = svc.api_put(alias, reinterpret_cast<const std::uint8_t*>("real"), 4, {}, true,
                             {});
  expect(replace.ok, "replace redirect");
  auto real = svc.api_get(alias, std::nullopt, std::nullopt, {});
  expect(real.ok && real.code != "redirect", "no longer redirect");
  expect(real.data && std::string(real.data->begin(), real.data->end()) == "real",
         "real body after replace");

  // Empty oid / bad cases via store.
  auto placement = place(create_oid, fx.map);
  expect(!placement.acting_set.empty(), "placement");
  auto* store = fx.stores.get(placement.acting_set[0].aios_path);
  expect(store != nullptr, "primary store");
  if (store) {
    std::string err;
    expect(!store->put("", std::string("x"), {}, true, err), "empty oid put fails");
    expect(err == "empty oid", "empty oid err");
  }

  // prepare_put + abort leaves tip unchanged.
  {
    const std::string abort_oid = "adv/abort";
    auto put = svc.api_put(abort_oid, reinterpret_cast<const std::uint8_t*>("tip"), 3, {}, true,
                           {});
    expect(put.ok, "abort oid put");
    auto pl = place(abort_oid, fx.map);
    auto* ps = fx.stores.get(pl.acting_set[0].aios_path);
    expect(ps != nullptr, "abort primary store");
    if (ps) {
      std::string err;
      auto tip_before = ps->stat(abort_oid, err);
      expect(tip_before.has_value(), "tip before prepare");
      PreparedVersion pv;
      expect(ps->prepare_put(abort_oid, reinterpret_cast<const std::uint8_t*>("zzz"), 3, {},
                             true, std::nullopt, pv, err),
             "prepare_put");
      expect(ps->abort_version(abort_oid, pv.seq, err), "abort_version");
      auto tip_after = ps->stat(abort_oid, err);
      expect(tip_after && tip_before && tip_after->seq == tip_before->seq,
             "tip unchanged after abort");
      auto body = ps->get(abort_oid, err);
      expect(body && std::string(body->begin(), body->end()) == "tip", "tip body after abort");
    }
  }

  // Attr preconditions via API.
  {
    const std::string pred_oid = "adv/preds";
    auto put = svc.api_put(pred_oid, reinterpret_cast<const std::uint8_t*>("p"), 1,
                           {{"color", "red"}, {"env", "prod"}}, true, {});
    expect(put.ok, "pred put");
    std::vector<AttrPrecondition> eq = {{AttrPrecondition::Kind::Eq, "color", "red"}};
    expect(svc.api_get(pred_oid, std::nullopt, std::nullopt, eq).ok, "pred Eq ok");
    std::vector<AttrPrecondition> ne = {{AttrPrecondition::Kind::Ne, "color", "blue"}};
    expect(svc.api_get(pred_oid, std::nullopt, std::nullopt, ne).ok, "pred Ne ok");
    std::vector<AttrPrecondition> present = {{AttrPrecondition::Kind::Present, "env", {}}};
    expect(svc.api_get(pred_oid, std::nullopt, std::nullopt, present).ok, "pred Present");
    std::vector<AttrPrecondition> absent = {{AttrPrecondition::Kind::Absent, "missing", {}}};
    expect(svc.api_get(pred_oid, std::nullopt, std::nullopt, absent).ok, "pred Absent");
    std::vector<AttrPrecondition> must = {{AttrPrecondition::Kind::MustExist, {}, {}}};
    expect(svc.api_get(pred_oid, std::nullopt, std::nullopt, must).ok, "pred MustExist");
    std::vector<AttrPrecondition> bad_eq = {{AttrPrecondition::Kind::Eq, "color", "green"}};
    expect(svc.api_get(pred_oid, std::nullopt, std::nullopt, bad_eq).code ==
               "precondition_failed",
           "pred Eq fail");
    std::vector<AttrPrecondition> bad_ne = {{AttrPrecondition::Kind::Ne, "color", "red"}};
    expect(svc.api_put(pred_oid, reinterpret_cast<const std::uint8_t*>("q"), 1, {}, true, bad_ne)
               .code == "precondition_failed",
           "pred Ne fail on put");
  }

  // List with prefix + attrs + attr_eq (may see duplicates across local replica stores).
  {
    expect(svc.api_put("list/a", reinterpret_cast<const std::uint8_t*>("1"), 1,
                       {{"team", "x"}}, true, {})
               .ok,
           "list put a");
    expect(svc.api_put("list/b", reinterpret_cast<const std::uint8_t*>("2"), 1,
                       {{"team", "y"}}, true, {})
               .ok,
           "list put b");
    expect(svc.api_put("other/c", reinterpret_cast<const std::uint8_t*>("3"), 1, {}, true, {})
               .ok,
           "list put other");
    auto lst = svc.api_list("list/", "", "", 10, "", true);
    expect(lst.ok && lst.list.objects.size() >= 2, "list prefix");
    bool saw_a = false, saw_b = false, saw_attrs = false;
    for (const auto& o : lst.list.objects) {
      if (o.oid == "list/a") {
        saw_a = true;
        if (!o.attrs.empty()) saw_attrs = true;
      }
      if (o.oid == "list/b") saw_b = true;
    }
    expect(saw_a && saw_b, "list has a and b");
    expect(saw_attrs, "list include attrs");
    auto filtered = svc.api_list("list/", "team", "x", 10, "", false);
    expect(filtered.ok && !filtered.list.objects.empty(), "list attr_eq nonempty");
    bool only_a = true;
    for (const auto& o : filtered.list.objects) {
      if (o.oid != "list/a") only_a = false;
    }
    expect(only_a, "list attr_eq only list/a");
  }

  // Head by version; CRC on put reply; both replicas after put.
  {
    const std::string crc_oid = "adv/crc";
    const auto* data = reinterpret_cast<const std::uint8_t*>("checksum-me");
    auto put = svc.api_put(crc_oid, data, 11, {}, true, {},
                           std::optional<std::uint32_t>(crc32c(data, 11)));
    expect(put.ok && put.info && put.info->crc32c_known, "put with crc");
    auto head1 = svc.api_head(crc_oid, {}, /*seq=*/1);
    expect(head1.ok && head1.info && head1.info->seq == 1, "head by seq");
    std::string err;
    int copies = 0;
    if (fx.stores.get(fx.p1)->stat(crc_oid, err)) ++copies;
    err.clear();
    if (fx.stores.get(fx.p2)->stat(crc_oid, err)) ++copies;
    expect(copies == 2, "both replicas have object");
  }

  // Self-redirect rejected via API.
  expect(!svc.api_put_redirect("adv/self", "adv/self", {}, true, {}).ok, "api self-redirect");

  // Missing object get.
  expect(svc.api_get("adv/nope", std::nullopt, std::nullopt, {}).code == "not_found",
         "missing get");

  return failures();
}
