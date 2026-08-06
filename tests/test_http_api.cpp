#include "cluster/cluster_map.hpp"
#include <gtest/gtest.h>
#include "config.hpp"
#include "fs/aios_scan.hpp"
#include "fs/fs_table.hpp"
#include "http/http_auth.hpp"
#include "membership.hpp"
#include "object/object_service.hpp"
#include "store/local_stores.hpp"
#include "util/log.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;


TEST(HttpApi, Basic) {
  using namespace aios;
  // Exercise ObjectService HTTP-facing API (range, preds, list) without sockets.
  const auto root = fs::temp_directory_path() / ("aios-http-" + std::to_string(::getpid()));
  fs::create_directories(root / "t1" / "aios");
  fs::create_directories(root / "t2" / "aios");
  const std::string p1 = (root / "t1" / "aios").string();
  const std::string p2 = (root / "t2" / "aios").string();

  MembershipTable membership;
  membership.set_local("node-a", "127.0.0.1:7400");
  FsTable fs_table;
  std::vector<AiosTarget> local;
  for (const auto& path : {p1, p2}) {
    AiosTarget t;
    t.mount = path;
    t.target_path = path;
    t.aios_path = path;
      t.storage_class = "nvme";
    t.usable = true;
    t.bavail = 1000;
    local.push_back(t);
  }
  fs_table.set_local("node-a", local);

  Config cfg;
  cfg.node_id = "node-a";
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.replica_count = 2;
  cfg.write_quorum = 2;

  ClusterMap map = ClusterMap::build(membership, fs_table, cfg.replica_count, PlacementConfig{});
  LocalStores stores;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  stores.sync_paths({p1, p2}, opts);

  ObjectService svc(cfg, map, stores);
  svc.set_advertise("127.0.0.1:7400");

  const std::string oid = "dir/item-1";
  std::vector<AttrPrecondition> create = {
      {AttrPrecondition::Kind::MustNotExist, {}, {}},
  };
  auto put = svc.api_put(oid, reinterpret_cast<const std::uint8_t*>("ABCDEFGH"), 8,
                         {{"ver", "1"}}, true, create);
  EXPECT_TRUE(put.ok) << "api_put";
  EXPECT_TRUE(put.replicas == 2) << "replicated";

  // Partial overwrite
  const std::string patch = "xy";
  std::vector<AttrPrecondition> ver_ok = {
      {AttrPrecondition::Kind::Eq, "ver", "1"},
  };
  auto pr = svc.api_put_range(oid, 2, reinterpret_cast<const std::uint8_t*>(patch.data()),
                              patch.size(), {{"ver", "2"}}, false, ver_ok);
  EXPECT_TRUE(pr.ok) << "api_put_range";

  std::vector<AttrPrecondition> ver_bad = {
      {AttrPrecondition::Kind::Eq, "ver", "1"},
  };
  auto conflict =
      svc.api_put_range(oid, 0, reinterpret_cast<const std::uint8_t*>("Z"), 1, {}, false,
                        ver_bad);
  EXPECT_TRUE(!conflict.ok && conflict.code == "precondition_failed") << "pred 412";

  auto got = svc.api_get(oid, std::nullopt, std::nullopt, {});
  EXPECT_TRUE(got.ok && got.data.has_value()) << "get";
  EXPECT_TRUE(std::string(got.data->begin(), got.data->end()) == "ABxyEFGH") << "patched body";

  auto ranged = svc.api_get(oid, 2, 3, {});
  EXPECT_TRUE(ranged.ok && ranged.data.has_value()) << "range get";
  EXPECT_TRUE(std::string(ranged.data->begin(), ranged.data->end()) == "xy") << "range bytes";

  auto unsat = svc.api_get(oid, 100, 110, {});
  EXPECT_TRUE(!unsat.ok && unsat.code == "range_unsatisfiable") << "416";

  auto lst = svc.api_list("dir/", "", "", 10, "", true);
  EXPECT_TRUE(lst.ok && !lst.list.objects.empty()) << "list";

  // Redirect object → target.
  const std::string alias = "dir/alias";
  auto redir = svc.api_put_redirect(alias, oid, {{"kind", "link"}}, true, {});
  EXPECT_TRUE(redir.ok) << "put_redirect";
  EXPECT_TRUE(redir.redirect_oid == oid) << "redirect target";
  auto redir_get = svc.api_get(alias, std::nullopt, std::nullopt, {});
  EXPECT_TRUE(redir_get.ok && redir_get.code == "redirect") << "get redirect";
  EXPECT_TRUE(redir_get.redirect_oid == oid) << "get redirect oid";
  EXPECT_TRUE(!redir_get.data.has_value()) << "redirect has no body";

  auto self = svc.api_put_redirect(alias, alias, {}, true, {});
  EXPECT_TRUE(!self.ok) << "reject self-redirect";

  auto del = svc.api_del(oid, {});
  EXPECT_TRUE(del.ok) << "del";

  fs::remove_all(root);
  }
