#include "test_helpers.hpp"

#include "cluster/cluster_map.hpp"
#include "cluster/place.hpp"
#include "config.hpp"
#include "fs/aios_scan.hpp"
#include "fs/fs_table.hpp"
#include "membership.hpp"
#include "net/object_client.hpp"
#include "net/server.hpp"
#include "object/object_service.hpp"
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

using aios::test::expect;
using aios::test::failures;
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
    aios::ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    stores.sync_paths({aios_path}, opts);

    svc = std::make_unique<aios::ObjectService>(cfg, map, stores);
    svc->set_advertise(addr);

    std::string host, port;
    expect(aios::split_host_port(addr, host, port), "split listen " + id);

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

  explicit MiniCluster(int n, int replica_count, int write_quorum) {
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
      node->cfg.auth_skew_ms = 60000;
      node->cfg.clone_required = false;
      node->cfg.max_versions = 16;

      if (i == 0) {
        membership.set_local(node->id, node->addr);
        aios::AiosTarget t;
        t.mount = path.string();
        t.target_path = path.string();
        t.aios_path = node->aios_path;
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
        e.bavail = 1000;
        e.usable = true;
        e.updated_ms = aios::now_ms();
        remotes.push_back(e);
      }
      nodes.push_back(std::move(node));
    }
    fs_table.merge(remotes);
    map = aios::ClusterMap::build(membership, fs_table, replica_count);

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
    auto p = aios::place(oid, map);
    if (p.acting_set.empty()) return nullptr;
    return by_id(p.acting_set[0].node_id);
  }

  MiniNode* secondary_for(const std::string& oid) {
    auto p = aios::place(oid, map);
    if (p.acting_set.size() < 2) return nullptr;
    return by_id(p.acting_set[1].node_id);
  }
};

}  // namespace

int test_mini_cluster() {
  using namespace aios;
  failures() = 0;

  // --- 3-node cluster, replica_count=3, write_quorum=2 ---
  {
    MiniCluster cluster(3, /*replica_count=*/3, /*write_quorum=*/2);
    expect(cluster.map.targets.size() == 3, "map has 3 targets");
    expect(cluster.nodes.size() == 3, "3 nodes");

    const std::string oid = "cluster/obj-1";
    auto* primary = cluster.primary_for(oid);
    expect(primary != nullptr, "primary found");
    auto placement = place(oid, cluster.map);
    expect(placement.acting_set.size() == 3, "acting set 3 distinct nodes");
    {
      std::unordered_set<std::string> nodes;
      for (const auto& t : placement.acting_set) nodes.insert(t.node_id);
      expect(nodes.size() == 3, "acting set prefers distinct nodes");
    }

    const auto* payload = reinterpret_cast<const std::uint8_t*>("hello-cluster");
    auto put = primary->svc->api_put(oid, payload, 13, {{"k", "v"}}, true, {});
    expect(put.ok, "cross-node put ok");
    expect(put.replicas >= 2, "quorum replicas");

    // Every acting-set member has the tip locally.
    for (const auto& t : placement.acting_set) {
      auto* n = cluster.by_id(t.node_id);
      expect(n != nullptr, "acting node exists");
      std::string err;
      auto st = n->stores.get(t.aios_path)->stat(oid, err);
      expect(st.has_value() && st->size == 13, "replica has object " + t.node_id);
    }

    // Remote GET via TCP to a secondary (real network hop).
    auto* secondary = cluster.secondary_for(oid);
    expect(secondary != nullptr, "secondary found");
    auto remote_get =
        object_get_remote(secondary->addr, "client", "127.0.0.1:1", cluster.cluster_key,
                          60000, cluster.map.epoch, secondary->aios_path, oid);
    expect(remote_get.ok, "tcp get secondary");
    expect(remote_get.data &&
               std::string(remote_get.data->begin(), remote_get.data->end()) == "hello-cluster",
           "tcp get body");

    // Stat over TCP
    auto remote_stat =
        object_stat_remote(secondary->addr, "client", "127.0.0.1:1", cluster.cluster_key,
                           60000, cluster.map.epoch, secondary->aios_path, oid);
    expect(remote_stat.ok && remote_stat.size == 13, "tcp stat size");
    expect(remote_stat.crc32c_known, "tcp stat crc");

    // Ranged put replicated across nodes.
    const auto* patch = reinterpret_cast<const std::uint8_t*>("XYZ");
    auto pr = primary->svc->api_put_range(oid, 0, patch, 3, {}, false, {});
    expect(pr.ok && pr.replicas >= 2, "cross-node put_range");
    auto got = primary->svc->api_get(oid, std::nullopt, std::nullopt, {});
    expect(got.ok && got.data &&
               std::string(got.data->begin(), got.data->end()) == "XYZlo-cluster",
           "range body on primary");

    // Redirect object on primary, visible via TCP get on secondary.
    const std::string alias = "cluster/alias";
    auto* alias_primary = cluster.primary_for(alias);
    expect(alias_primary != nullptr, "alias primary");
    auto redir = alias_primary->svc->api_put_redirect(alias, oid, {}, true, {});
    expect(redir.ok && redir.redirect_oid == oid, "cluster redirect");
    auto alias_place = place(alias, cluster.map);
    expect(!alias_place.acting_set.empty(), "alias placement");
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
      expect(rget.ok, "tcp get redirect");
      expect(rget.body.value("redirect", "") == oid || rget.body.value("code", "") == "redirect",
             "tcp redirect field");
    }

    // Version list API on primary.
    auto vers = primary->svc->api_list_versions(oid);
    expect(vers.ok && vers.versions.size() >= 2, "versions after put+range");

    // Delete marker replicated.
    auto del = primary->svc->api_del(oid, {});
    expect(del.ok, "cluster del");
    expect(!primary->svc->api_get(oid, std::nullopt, std::nullopt, {}).ok, "tip gone");
  }

  // --- Quorum failure when a secondary is down (write_quorum == replica_count) ---
  {
    MiniCluster cluster(3, /*replica_count=*/3, /*write_quorum=*/3);
    const std::string oid = "cluster/quorum-fail";
    auto placement = place(oid, cluster.map);
    expect(placement.acting_set.size() == 3, "qf acting 3");

    // Stop a non-primary in the acting set.
    auto* primary = cluster.by_id(placement.acting_set[0].node_id);
    auto* victim = cluster.by_id(placement.acting_set[2].node_id);
    expect(primary && victim && primary != victim, "qf nodes");
    victim->stop();

    const auto* payload = reinterpret_cast<const std::uint8_t*>("should-fail");
    auto put = primary->svc->api_put(oid, payload, 11, {}, true, {});
    expect(!put.ok && put.code == "quorum_failed", "quorum_failed when peer down");

    // Tip must not advance (publish-after-quorum aborted).
    std::string err;
    auto* pstore = primary->stores.get(placement.acting_set[0].aios_path);
    expect(pstore != nullptr, "primary store");
    expect(!pstore->stat(oid, err).has_value(), "no tip after quorum fail");
  }

  // --- Quorum still succeeds with write_quorum=2 when one peer is down ---
  {
    MiniCluster cluster(3, /*replica_count=*/3, /*write_quorum=*/2);
    const std::string oid = "cluster/quorum-ok";
    auto placement = place(oid, cluster.map);
    auto* primary = cluster.by_id(placement.acting_set[0].node_id);
    auto* victim = cluster.by_id(placement.acting_set[2].node_id);
    expect(primary && victim, "qo nodes");
    victim->stop();

    const auto* payload = reinterpret_cast<const std::uint8_t*>("still-ok");
    auto put = primary->svc->api_put(oid, payload, 8, {}, true, {});
    expect(put.ok, "put ok with quorum 2");
    expect(put.replicas >= 2, "at least 2 copies");

    auto* live_sec = cluster.by_id(placement.acting_set[1].node_id);
    expect(live_sec != nullptr, "live secondary");
    std::string err;
    expect(live_sec->stores.get(placement.acting_set[1].aios_path)->stat(oid, err).has_value(),
           "live secondary has copy");
  }

  // --- 2-node cluster smoke ---
  {
    MiniCluster cluster(2, /*replica_count=*/2, /*write_quorum=*/2);
    expect(cluster.map.targets.size() == 2, "2-node map");
    const std::string oid = "cluster/two-node";
    auto* primary = cluster.primary_for(oid);
    expect(primary != nullptr, "2n primary");
    auto put = primary->svc->api_put(oid, reinterpret_cast<const std::uint8_t*>("ab"), 2, {},
                                     true, {});
    expect(put.ok && put.replicas == 2, "2-node full quorum");

    auto* secondary = cluster.secondary_for(oid);
    expect(secondary != nullptr, "2n secondary");
    auto g = object_get_remote(secondary->addr, "client", "127.0.0.1:1", cluster.cluster_key,
                               60000, cluster.map.epoch, secondary->aios_path, oid);
    expect(g.ok && g.data && std::string(g.data->begin(), g.data->end()) == "ab",
           "2-node tcp get");
  }

  return failures();
}
