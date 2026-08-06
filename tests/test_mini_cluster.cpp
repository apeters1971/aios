#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "cluster/cluster_map.hpp"
#include "cluster/place.hpp"
#include "config.hpp"
#include "ec/ec_attrs.hpp"
#include "fs/aios_scan.hpp"
#include "fs/fs_table.hpp"
#include "membership.hpp"
#include "net/object_client.hpp"
#include "net/server.hpp"
#include "object/object_service.hpp"
#include "object/repair.hpp"
#include "store/local_stores.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using aios::test::temp_root;

struct MiniNode {
  std::string id;
  std::string addr;  // host:port
  std::string aios_path;
  aios::Config cfg;
  aios::LocalStores stores;
  std::unique_ptr<aios::ObjectService> svc;
  std::unique_ptr<aios::TcpServer> server;
  boost::asio::io_context ioc;
  std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      work;
  std::thread thr;
  bool running{false};

  void start(aios::ClusterMap& map) {
    if (running) return;
    ioc.restart();

    aios::ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    stores.sync_paths({aios_path}, opts);

    svc = std::make_unique<aios::ObjectService>(cfg, map, stores);
    svc->set_advertise(addr);

    std::string host, port;
    EXPECT_TRUE(aios::split_host_port(addr, host, port)) << "split listen " + id;

    aios::RpcHandlers handlers;
    handlers.local_node_id = id;
    handlers.local_listen = addr;
    handlers.cluster_key = cfg.cluster_key;
    handlers.auth_skew_ms = cfg.auth_skew_ms;
    handlers.on_object = [this](const aios::Frame& req) { return svc->handle(req); };

    server = std::make_unique<aios::TcpServer>(ioc, host, port, std::move(handlers));
    server->start();

    work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(ioc));
    thr = std::thread([this] {
      try {
        ioc.run();
      } catch (...) {
      }
    });
    running = true;
    // Brief settle for accept loop.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  void stop() {
    if (!running) return;
    work.reset();
    ioc.stop();
    if (thr.joinable()) thr.join();
    server.reset();
    svc.reset();
    running = false;
  }
};

struct MiniCluster {
  std::filesystem::path root;
  aios::ClusterMap map;
  std::vector<std::unique_ptr<MiniNode>> nodes;
  std::string cluster_key{"550e8400-e29b-41d4-a716-446655440000"};

  explicit MiniCluster(int n, int replica_count, int write_quorum,
                       const std::string& durability = "replica", int ec_k = 2, int ec_m = 1,
                       const std::string& ec_codec = "") {
    root = temp_root("aios-mini-cluster");
    const int base_port = 19100 + static_cast<int>(::getpid() % 400);

    aios::MembershipTable membership;
    aios::FsTable fs_table;
    std::vector<aios::FsEntry> remotes;

    for (int i = 0; i < n; ++i) {
      auto node = std::make_unique<MiniNode>();
      node->id = "node-" + std::string(1, static_cast<char>('a' + i));
      node->addr = "127.0.0.1:" + std::to_string(base_port + i);
      const auto path = root / node->id / "aios";
      std::filesystem::create_directories(path);
      node->aios_path = path.string();

      node->cfg.node_id = node->id;
      node->cfg.listen = node->addr;
      node->cfg.cluster_key = cluster_key;
      node->cfg.replica_count = replica_count;
      node->cfg.write_quorum = write_quorum;
      node->cfg.durability = durability;
      node->cfg.ec_k = ec_k;
      node->cfg.ec_m = ec_m;
      node->cfg.ec_codec = ec_codec;
      node->cfg.auth_skew_ms = 60000;
      node->cfg.clone_required = false;
      node->cfg.max_versions = 16;
      if (durability == "ec") {
        std::string err;
        EXPECT_TRUE(aios::normalize_config(node->cfg, err)) << "normalize mini ec " + node->id;
      }

      if (i == 0) {
        membership.set_local(node->id, node->addr);
        aios::AiosTarget t;
        t.mount = path.string();
        t.target_path = path.string();
        t.aios_path = node->aios_path;
      t.storage_class = "nvme";
        t.usable = true;
        t.bavail = 1000;
        fs_table.set_local(node->id, {t});
      } else {
        membership.mark_alive(node->id, node->addr, aios::now_ms());
        aios::FsEntry e;
        e.node_id = node->id;
        e.mount = path.string();
        e.target_path = path.string();
        e.aios_path = node->aios_path;
        e.storage_class = "nvme";
        e.weight = 1;
        e.bavail = 1000;
        e.usable = true;
        e.updated_ms = aios::now_ms();
        remotes.push_back(e);
      }
      nodes.push_back(std::move(node));
    }
    fs_table.merge(remotes);
    const int map_replicas =
        (durability == "ec" && !nodes.empty()) ? nodes[0]->cfg.replica_count : replica_count;
    map = aios::ClusterMap::build(membership, fs_table, map_replicas, aios::PlacementConfig{});

    for (auto& node : nodes) node->start(map);
  }

  ~MiniCluster() {
    for (auto& node : nodes) node->stop();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  MiniNode* by_id(const std::string& id) {
    for (auto& n : nodes) {
      if (n->id == id) return n.get();
    }
    return nullptr;
  }

  MiniNode* primary_for(const std::string& oid) {
    auto p = aios::place(oid, map, "nvme");
    if (p.acting_set.empty()) return nullptr;
    return by_id(p.acting_set[0].node_id);
  }

  MiniNode* secondary_for(const std::string& oid) {
    auto p = aios::place(oid, map, "nvme");
    if (p.acting_set.size() < 2) return nullptr;
    return by_id(p.acting_set[1].node_id);
  }
};

}  // namespace

TEST(MiniCluster, Basic) {
  using namespace aios;
  // --- 3-node cluster, replica_count=3, write_quorum=2 ---
  {
    MiniCluster cluster(3, /*replica_count=*/3, /*write_quorum=*/2);
    EXPECT_TRUE(cluster.map.targets.size() == 3) << "map has 3 targets";
    EXPECT_TRUE(cluster.nodes.size() == 3) << "3 nodes";

    const std::string oid = "cluster/obj-1";
    auto* primary = cluster.primary_for(oid);
    EXPECT_TRUE(primary != nullptr) << "primary found";
    auto placement = place(oid, cluster.map, "nvme");
    EXPECT_TRUE(placement.acting_set.size() == 3) << "acting set 3 distinct nodes";
    {
      std::unordered_set<std::string> nodes;
      for (const auto& t : placement.acting_set) nodes.insert(t.node_id);
      EXPECT_TRUE(nodes.size() == 3) << "acting set prefers distinct nodes";
    }

    const auto* payload = reinterpret_cast<const std::uint8_t*>("hello-cluster");
    auto put = primary->svc->api_put(oid, payload, 13, {{"k", "v"}}, true, {});
    EXPECT_TRUE(put.ok) << "cross-node put ok";
    EXPECT_TRUE(put.replicas >= 2) << "quorum replicas";

    // Every acting-set member has the tip locally.
    for (const auto& t : placement.acting_set) {
      auto* n = cluster.by_id(t.node_id);
      EXPECT_TRUE(n != nullptr) << "acting node exists";
      std::string err;
      auto st = n->stores.get(t.aios_path)->stat(oid, err);
      EXPECT_TRUE(st.has_value() && st->size == 13) << "replica has object " + t.node_id;
    }

    // Remote GET via TCP to a secondary (real network hop).
    auto* secondary = cluster.secondary_for(oid);
    EXPECT_TRUE(secondary != nullptr) << "secondary found";
    auto remote_get =
        object_get_remote(secondary->addr, "client", "127.0.0.1:1", cluster.cluster_key,
                          60000, cluster.map.epoch, secondary->aios_path, oid);
    EXPECT_TRUE(remote_get.ok) << "tcp get secondary";
    EXPECT_TRUE(remote_get.data &&
               std::string(remote_get.data->begin(), remote_get.data->end()) == "hello-cluster") << "tcp get body";

    // Stat over TCP
    auto remote_stat =
        object_stat_remote(secondary->addr, "client", "127.0.0.1:1", cluster.cluster_key,
                           60000, cluster.map.epoch, secondary->aios_path, oid);
    EXPECT_TRUE(remote_stat.ok && remote_stat.size == 13) << "tcp stat size";
    EXPECT_TRUE(remote_stat.crc32c_known) << "tcp stat crc";

    // Ranged put replicated across nodes.
    const auto* patch = reinterpret_cast<const std::uint8_t*>("XYZ");
    auto pr = primary->svc->api_put_range(oid, 0, patch, 3, {}, false, {});
    EXPECT_TRUE(pr.ok && pr.replicas >= 2) << "cross-node put_range";
    auto got = primary->svc->api_get(oid, std::nullopt, std::nullopt, {});
    EXPECT_TRUE(got.ok && got.data &&
               std::string(got.data->begin(), got.data->end()) == "XYZlo-cluster") << "range body on primary";

    // Redirect object on primary, visible via TCP get on secondary.
    const std::string alias = "cluster/alias";
    auto* alias_primary = cluster.primary_for(alias);
    EXPECT_TRUE(alias_primary != nullptr) << "alias primary";
    auto redir = alias_primary->svc->api_put_redirect(alias, oid, {}, true, {});
    EXPECT_TRUE(redir.ok && redir.redirect_oid == oid) << "cluster redirect";
    auto alias_place = place(alias, cluster.map, "nvme");
    EXPECT_TRUE(!alias_place.acting_set.empty()) << "alias placement";
    // Find a live replica that is not necessarily primary for TCP get.
    MiniNode* alias_peer = nullptr;
    for (const auto& t : alias_place.acting_set) {
      if (t.node_id != alias_primary->id) {
        alias_peer = cluster.by_id(t.node_id);
        break;
      }
    }
    if (alias_peer) {
      auto rget = object_get_remote(alias_peer->addr, "client", "127.0.0.1:1",
                                    cluster.cluster_key, 60000, cluster.map.epoch,
                                    alias_peer->aios_path, alias);
      EXPECT_TRUE(rget.ok) << "tcp get redirect";
      EXPECT_TRUE(rget.body.value("redirect", "") == oid || rget.body.value("code", "") == "redirect") << "tcp redirect field";
    }

    // Version list API on primary.
    auto vers = primary->svc->api_list_versions(oid);
    EXPECT_TRUE(vers.ok && vers.versions.size() >= 2) << "versions after put+range";

    // Delete marker replicated.
    auto del = primary->svc->api_del(oid, {});
    EXPECT_TRUE(del.ok) << "cluster del";
    EXPECT_TRUE(!primary->svc->api_get(oid, std::nullopt, std::nullopt, {}).ok) << "tip gone";
  }

  // --- Quorum failure when a secondary is down (write_quorum == replica_count) ---
  {
    MiniCluster cluster(3, /*replica_count=*/3, /*write_quorum=*/3);
    const std::string oid = "cluster/quorum-fail";
    auto placement = place(oid, cluster.map, "nvme");
    EXPECT_TRUE(placement.acting_set.size() == 3) << "qf acting 3";

    // Stop a non-primary in the acting set.
    auto* primary = cluster.by_id(placement.acting_set[0].node_id);
    auto* victim = cluster.by_id(placement.acting_set[2].node_id);
    EXPECT_TRUE(primary && victim && primary != victim) << "qf nodes";
    victim->stop();

    const auto* payload = reinterpret_cast<const std::uint8_t*>("should-fail");
    auto put = primary->svc->api_put(oid, payload, 11, {}, true, {});
    EXPECT_TRUE(!put.ok && put.code == "quorum_failed") << "quorum_failed when peer down";

    // Tip must not advance (publish-after-quorum aborted).
    std::string err;
    auto* pstore = primary->stores.get(placement.acting_set[0].aios_path);
    EXPECT_TRUE(pstore != nullptr) << "primary store";
    EXPECT_TRUE(!pstore->stat(oid, err).has_value()) << "no tip after quorum fail";
  }

  // --- Quorum still succeeds with write_quorum=2 when one peer is down ---
  {
    MiniCluster cluster(3, /*replica_count=*/3, /*write_quorum=*/2);
    const std::string oid = "cluster/quorum-ok";
    auto placement = place(oid, cluster.map, "nvme");
    auto* primary = cluster.by_id(placement.acting_set[0].node_id);
    auto* victim = cluster.by_id(placement.acting_set[2].node_id);
    EXPECT_TRUE(primary && victim) << "qo nodes";
    victim->stop();

    const auto* payload = reinterpret_cast<const std::uint8_t*>("still-ok");
    auto put = primary->svc->api_put(oid, payload, 8, {}, true, {});
    EXPECT_TRUE(put.ok) << "put ok with quorum 2";
    EXPECT_TRUE(put.replicas >= 2) << "at least 2 copies";

    auto* live_sec = cluster.by_id(placement.acting_set[1].node_id);
    EXPECT_TRUE(live_sec != nullptr) << "live secondary";
    std::string err;
    EXPECT_TRUE(live_sec->stores.get(placement.acting_set[1].aios_path)->stat(oid, err).has_value()) << "live secondary has copy";
  }

  // --- 2-node cluster smoke ---
  {
    MiniCluster cluster(2, /*replica_count=*/2, /*write_quorum=*/2);
    EXPECT_TRUE(cluster.map.targets.size() == 2) << "2-node map";
    const std::string oid = "cluster/two-node";
    auto* primary = cluster.primary_for(oid);
    EXPECT_TRUE(primary != nullptr) << "2n primary";
    auto put = primary->svc->api_put(oid, reinterpret_cast<const std::uint8_t*>("ab"), 2, {},
                                     true, {});
    EXPECT_TRUE(put.ok && put.replicas == 2) << "2-node full quorum";

    auto* secondary = cluster.secondary_for(oid);
    EXPECT_TRUE(secondary != nullptr) << "2n secondary";
    auto g = object_get_remote(secondary->addr, "client", "127.0.0.1:1", cluster.cluster_key,
                               60000, cluster.map.epoch, secondary->aios_path, oid);
    EXPECT_TRUE(g.ok && g.data && std::string(g.data->begin(), g.data->end()) == "ab") << "2-node tcp get";
  }

  // --- EC XOR 2+1 across 3 nodes: degraded GET + repair after peer kill ---
  {
    MiniCluster cluster(3, /*replica_count=*/3, /*write_quorum=*/3, "ec", 2, 1, "xor");
    const std::string oid = "cluster/ec-xor";
    auto placement = place(oid, cluster.map, "nvme");
    EXPECT_TRUE(placement.acting_set.size() == 3) << "ec acting 3";
    auto* primary = cluster.by_id(placement.acting_set[0].node_id);
    auto* shard1 = cluster.by_id(placement.acting_set[1].node_id);
    auto* shard2 = cluster.by_id(placement.acting_set[2].node_id);
    EXPECT_TRUE(primary && shard1 && shard2) << "ec nodes";

    const auto* payload = reinterpret_cast<const std::uint8_t*>("cross-node-ec!!");
    const std::size_t len = 15;
    auto put = primary->svc->api_put(oid, payload, len, {}, true, {});
    EXPECT_TRUE(put.ok && put.replicas == 3) << "ec cross-node put";

    // All three nodes hold a shard tip.
    std::string err;
    for (const auto& t : placement.acting_set) {
      auto* n = cluster.by_id(t.node_id);
      EXPECT_TRUE(n && n->stores.get(t.aios_path)->stat(oid, err)) << "ec shard present " + t.node_id;
      auto attrs = n->stores.get(t.aios_path)->list_attrs(oid, err);
      EXPECT_TRUE(attrs_are_ec(attrs)) << "ec attrs on " + t.node_id;
    }

    // Kill one shard holder; primary reconstructs via remote GETs.
    shard2->stop();
    auto degraded = primary->svc->api_get(oid, std::nullopt, std::nullopt, {});
    EXPECT_TRUE(degraded.ok && degraded.data &&
               std::string(degraded.data->begin(), degraded.data->end()) ==
                   std::string(reinterpret_cast<const char*>(payload), len)) << "ec degraded get across nodes";

    // Restart victim empty (new store dir already purged by stop leaving data — node still has
    // disk). Clear its local tip then repair from primary.
    shard2->start(cluster.map);
    {
      auto* store = shard2->stores.get(placement.acting_set[2].aios_path);
      EXPECT_TRUE(store != nullptr) << "restarted store";
      auto st = store->stat(oid, err);
      if (st) EXPECT_TRUE(store->purge_version(oid, st->seq, true, err)) << "clear shard2 tip";
      EXPECT_TRUE(!store->stat(oid, err)) << "shard2 empty before repair";
    }
    auto stats =
        run_repair(primary->cfg, primary->addr, cluster.map, primary->stores, 256);
    EXPECT_TRUE(stats.under_replicated >= 1 && stats.repaired >= 1) << "ec cross-node repair";
    EXPECT_TRUE(shard2->stores.get(placement.acting_set[2].aios_path)->stat(oid, err).has_value()) << "shard2 restored";

    auto healthy = primary->svc->api_get(oid, std::nullopt, std::nullopt, {});
    EXPECT_TRUE(healthy.ok && healthy.data &&
               std::string(healthy.data->begin(), healthy.data->end()) ==
                   std::string(reinterpret_cast<const char*>(payload), len)) << "ec get after repair";
  }

  }
