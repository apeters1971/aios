// Cluster-map consensus (cluster/map_monitor.hpp) on an in-memory network, and
// the object-service gate it drives.

#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "cluster/map_monitor.hpp"
#include "cluster/place.hpp"
#include "object/object_service.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

using aios::ClusterMap;
using aios::MapMonitor;
using aios::StorageTarget;

// Synchronous in-memory transport with per-link partitions.
struct Net {
  std::mutex mu;
  std::map<std::string, MapMonitor*> nodes;
  std::set<std::pair<std::string, std::string>> cut;  // (from, to) dropped
  std::int64_t now{1'000'000};
  std::vector<std::string> append_to;
  std::vector<std::string> pull_from;

  MapMonitor::SendFn sender(const std::string& from) {
    return [this, from](const std::string& to, const nlohmann::json& req)
               -> std::optional<nlohmann::json> {
      MapMonitor* target = nullptr;
      std::int64_t t;
      {
        std::lock_guard lock(mu);
        if (cut.count({from, to}) || cut.count({to, from})) return std::nullopt;
        auto it = nodes.find(to);
        if (it == nodes.end()) return std::nullopt;
        target = it->second;
        t = now;
        if (req.contains("op") && req["op"].is_string() && req["op"] == "append") {
          append_to.push_back(to);
        }
        if (req.contains("op") && req["op"].is_string() && req["op"] == "pull") {
          pull_from.push_back(from);
        }
      }
      return target->handle(req, t);
    };
  }

  void isolate(const std::string& a) {
    std::lock_guard lock(mu);
    for (const auto& [id, _] : nodes) {
      if (id != a) cut.insert({a, id});
    }
  }
  void heal() {
    std::lock_guard lock(mu);
    cut.clear();
  }
  void advance(std::int64_t ms) {
    std::lock_guard lock(mu);
    now += ms;
  }
  std::int64_t time() {
    std::lock_guard lock(mu);
    return now;
  }
};

ClusterMap content_with(const std::vector<std::string>& nodes) {
  ClusterMap m;
  m.replica_count = 1;
  for (const auto& n : nodes) {
    StorageTarget t;
    t.node_id = n;
    t.addr = n;
    t.http_addr = n + "-http";
    t.aios_path = "/" + n + "/aios";
    t.storage_class = "nvme";
    t.rack = n;
    m.targets.push_back(t);
  }
  return m;
}

struct Cluster {
  Net net;
  std::vector<std::string> ids;
  std::vector<std::unique_ptr<MapMonitor>> mons;
  std::vector<std::string> learners;
  int tick_ms{1000};
  int lease_ms{10000};

  Cluster(std::vector<std::string> voters, std::vector<std::string> learn = {},
          std::string state_dir = {}) {
    ids = voters;
    learners = learn;
    std::vector<std::string> all = voters;
    all.insert(all.end(), learn.begin(), learn.end());
    for (const auto& id : all) {
      MapMonitor::Options o;
      o.node_id = id;
      o.self_addr = id;
      o.voters = voters;
      o.tick_ms = tick_ms;
      o.lease_ms = lease_ms;
      o.election_min_ticks = 2;
      o.election_max_ticks = 4;
      if (!state_dir.empty()) o.state_path = state_dir + "/" + id + ".map";
      mons.push_back(std::make_unique<MapMonitor>(o, net.sender(id)));
      net.nodes[id] = mons.back().get();
    }
    ids = all;
  }

  void tick_all() {
    for (auto& m : mons) m->tick(net.time(), learners);
  }
  // One tick of wall clock: everyone ticks, time advances.
  void step(int n = 1) {
    for (int i = 0; i < n; ++i) {
      tick_all();
      net.advance(tick_ms);
    }
  }
  MapMonitor* leader() {
    for (auto& m : mons) {
      if (m->is_leader()) return m.get();
    }
    return nullptr;
  }
  MapMonitor* by_id(const std::string& id) {
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (ids[i] == id) return mons[i].get();
    }
    return nullptr;
  }
  MapMonitor* run_until_leader(int max_steps = 40) {
    for (int i = 0; i < max_steps; ++i) {
      step();
      if (auto* l = leader()) return l;
    }
    return nullptr;
  }
};

}  // namespace

TEST(MapMonitor, ThreeVotersElectOneLeaderAndCommitMonotonicEpochs) {
  Cluster c({"a", "b", "c"});
  auto* leader = c.run_until_leader();
  ASSERT_NE(leader, nullptr);
  int leaders = 0;
  for (auto& m : c.mons) leaders += m->is_leader() ? 1 : 0;
  EXPECT_EQ(leaders, 1);

  // The no-op entry of the new term commits on the next heartbeat round.
  c.step(2);
  const auto v0 = leader->view(c.net.time());
  ASSERT_TRUE(v0.committed.has_value());
  EXPECT_GE(v0.committed_epoch, 1u);
  EXPECT_TRUE(v0.lease_valid);

  // Propose content: one more epoch, identical on every node.
  leader->propose(content_with({"a", "b", "c"}), c.net.time());
  c.step(2);
  const auto v1 = leader->view(c.net.time());
  EXPECT_EQ(v1.committed_epoch, v0.committed_epoch + 1);
  EXPECT_EQ(v1.committed->targets.size(), 3u);
  for (auto& m : c.mons) {
    const auto v = m->view(c.net.time());
    EXPECT_EQ(v.committed_epoch, v1.committed_epoch) << m->options().node_id;
    EXPECT_TRUE(v.lease_valid) << m->options().node_id;
    EXPECT_EQ(v.committed->epoch, v1.committed_epoch);
  }

  // Same content again: no new epoch.
  leader->propose(content_with({"a", "b", "c"}), c.net.time());
  c.step(2);
  EXPECT_EQ(leader->view(c.net.time()).committed_epoch, v1.committed_epoch);

  // Different content: exactly one more.
  leader->propose(content_with({"a", "b"}), c.net.time());
  c.step(2);
  EXPECT_EQ(leader->view(c.net.time()).committed_epoch, v1.committed_epoch + 1);
}

TEST(MapMonitor, IsolatedLeaderLosesLeaseAndMajorityElectsSuccessor) {
  Cluster c({"a", "b", "c"});
  auto* old_leader = c.run_until_leader();
  ASSERT_NE(old_leader, nullptr);
  c.step(2);
  const auto before = old_leader->view(c.net.time());
  ASSERT_TRUE(before.lease_valid);
  const std::string old_id = old_leader->options().node_id;

  c.net.isolate(old_id);
  // Within the lease the old leader still thinks it leads; after it, it must not.
  c.step(c.lease_ms / c.tick_ms + 1);
  const auto stale = old_leader->view(c.net.time());
  EXPECT_FALSE(stale.lease_valid) << "isolated leader kept its lease";

  // The majority side has a new leader with a higher term and a newer epoch.
  MapMonitor* succ = nullptr;
  for (int i = 0; i < 20 && !succ; ++i) {
    for (auto& m : c.mons) {
      if (m->options().node_id != old_id && m->is_leader()) succ = m.get();
    }
    if (!succ) c.step();
  }
  ASSERT_NE(succ, nullptr) << "no successor elected";
  c.step(2);
  const auto sv = succ->view(c.net.time());
  EXPECT_GT(sv.term, before.term);
  EXPECT_GT(sv.committed_epoch, before.committed_epoch);
  EXPECT_TRUE(sv.lease_valid);

  // Heal: the old leader steps down and adopts the successor's map.
  c.net.heal();
  c.step(4);
  EXPECT_FALSE(old_leader->is_leader());
  const auto healed = old_leader->view(c.net.time());
  EXPECT_EQ(healed.committed_epoch, succ->view(c.net.time()).committed_epoch);
  EXPECT_TRUE(healed.lease_valid);
  int leaders = 0;
  for (auto& m : c.mons) leaders += m->is_leader() ? 1 : 0;
  EXPECT_EQ(leaders, 1);
}

TEST(MapMonitor, MinorityCannotElectOrCommit) {
  Cluster c({"a", "b", "c", "d", "e"});
  auto* leader = c.run_until_leader();
  ASSERT_NE(leader, nullptr);
  c.step(2);
  const auto e0 = leader->view(c.net.time()).committed_epoch;

  // Cut a and b off from the other three, whichever side the leader is on.
  c.net.cut = {};
  for (const auto& x : {"a", "b"}) {
    for (const auto& y : {"c", "d", "e"}) c.net.cut.insert({x, y});
  }
  c.step(c.lease_ms / c.tick_ms + 6);
  auto* a = c.by_id("a");
  auto* b = c.by_id("b");
  EXPECT_FALSE(a->view(c.net.time()).lease_valid);
  EXPECT_FALSE(b->view(c.net.time()).lease_valid);
  // Nothing newer than the pre-partition epoch may be committed on the minority.
  EXPECT_LE(a->view(c.net.time()).committed_epoch, e0 + 1);
  bool majority_leader = false;
  for (const auto& id : {"c", "d", "e"}) {
    if (c.by_id(id)->is_leader()) majority_leader = true;
  }
  EXPECT_TRUE(majority_leader);
}

TEST(MapMonitor, LearnerFollowsCommittedMapAndHoldsLease) {
  Cluster c({"a", "b", "c"}, {"x"});
  auto* leader = c.run_until_leader();
  ASSERT_NE(leader, nullptr);
  leader->propose(content_with({"a", "b", "c", "x"}), c.net.time());
  c.net.append_to.clear();
  c.net.pull_from.clear();
  c.step(3);
  auto* x = c.by_id("x");
  EXPECT_FALSE(x->is_voter());
  for (const auto& to : c.net.append_to) {
    EXPECT_NE(to, "x") << "leader must not unicast the map to learners";
  }
  EXPECT_FALSE(c.net.pull_from.empty()) << "learner must pull the committed map";
  const auto xv = x->view(c.net.time());
  EXPECT_EQ(xv.committed_epoch, leader->view(c.net.time()).committed_epoch);
  EXPECT_TRUE(xv.lease_valid);
  EXPECT_EQ(xv.committed->targets.size(), 4u);
  // Everyone in the map acknowledged it: active right away.
  EXPECT_EQ(xv.active_epoch, xv.committed_epoch);

  // Cut the learner off: its lease lapses, the voters are unaffected.
  c.net.isolate("x");
  c.step(c.lease_ms / c.tick_ms + 1);
  EXPECT_FALSE(x->view(c.net.time()).lease_valid);
  EXPECT_TRUE(leader->view(c.net.time()).lease_valid);
}

TEST(MapMonitor, ActivationWaitsForUnreachableMemberUntilLeaseElapses) {
  Cluster c({"a", "b", "c"}, {"x"});
  auto* leader = c.run_until_leader();
  ASSERT_NE(leader, nullptr);
  leader->propose(content_with({"a", "b", "c", "x"}), c.net.time());
  c.step(3);
  const auto base = leader->view(c.net.time());
  ASSERT_EQ(base.active_epoch, base.committed_epoch);

  // x becomes unreachable; a new map (still listing x) commits but must not be
  // active anywhere until either x acks it or the lease period has passed.
  c.net.isolate("x");
  leader->propose(content_with({"a", "b", "x"}), c.net.time());
  c.step(2);
  const auto v = c.by_id("b")->view(c.net.time());
  EXPECT_GT(v.committed_epoch, base.committed_epoch);
  EXPECT_LT(v.active_epoch, v.committed_epoch) << "activated while x had not acknowledged";
  c.step(c.lease_ms / c.tick_ms + 1);
  const auto later = c.by_id("b")->view(c.net.time());
  EXPECT_EQ(later.active_epoch, later.committed_epoch);
}

TEST(MapMonitor, VoterRestartRestoresTermAndCommittedMap) {
  const auto dir = aios::test::temp_root("aios-mapmon");
  std::uint64_t epoch = 0;
  std::uint64_t term = 0;
  {
    Cluster c({"a", "b", "c"}, {}, dir.string());
    auto* leader = c.run_until_leader();
    ASSERT_NE(leader, nullptr);
    leader->propose(content_with({"a", "b", "c"}), c.net.time());
    c.step(3);
    const auto v = c.by_id("a")->view(c.net.time());
    epoch = v.committed_epoch;
    term = v.term;
    ASSERT_GT(epoch, 0u);
  }
  MapMonitor::Options o;
  o.node_id = "a";
  o.self_addr = "a";
  o.voters = {"a", "b", "c"};
  o.state_path = (dir / "a.map").string();
  MapMonitor restarted(o, [](const std::string&, const nlohmann::json&) {
    return std::optional<nlohmann::json>{};
  });
  const auto v = restarted.view(5'000'000);
  EXPECT_EQ(v.term, term);
  EXPECT_EQ(v.committed_epoch, epoch);
  ASSERT_TRUE(v.committed.has_value());
  EXPECT_EQ(v.committed->targets.size(), 3u);
  EXPECT_FALSE(v.lease_valid) << "a restarted node has no lease until the leader speaks";
  std::filesystem::remove_all(dir);
}

TEST(MapMonitor, RestartedVoterCatchesUpWithLeaderAfterNewTermAndEntries) {
  const auto dir = aios::test::temp_root("aios-mapmon-restart");
  Cluster c({"a", "b", "c"}, {}, dir.string());
  auto* leader = c.run_until_leader();
  ASSERT_NE(leader, nullptr);
  leader->propose(content_with({"a", "b", "c"}), c.net.time());
  c.step(3);
  const auto e_before = leader->view(c.net.time()).committed_epoch;

  // Pick a follower to "crash": drop it from the network, keep its state file.
  std::string victim;
  for (const auto& id : c.ids) {
    if (c.by_id(id) != leader) {
      victim = id;
      break;
    }
  }
  c.net.isolate(victim);
  // Leader loses one voter but keeps a majority; it commits more entries.
  leader->propose(content_with({"a", "b"}), c.net.time());
  c.step(3);
  ASSERT_TRUE(leader->view(c.net.time()).lease_valid);
  ASSERT_GT(leader->view(c.net.time()).committed_epoch, e_before);

  // "Restart" the victim from its state file and reconnect it.
  MapMonitor::Options o = c.by_id(victim)->options();
  auto fresh = std::make_unique<MapMonitor>(o, c.net.sender(victim));
  for (std::size_t i = 0; i < c.ids.size(); ++i) {
    if (c.ids[i] == victim) {
      c.mons[i] = std::move(fresh);
      c.net.nodes[victim] = c.mons[i].get();
    }
  }
  c.net.heal();
  c.step(3);
  // And once more, so the restarted node has to take a brand-new entry too.
  leader->propose(content_with({"a", "c"}), c.net.time());
  c.step(3);
  const auto lv = leader->view(c.net.time());
  const auto vv = c.by_id(victim)->view(c.net.time());
  EXPECT_EQ(vv.committed_epoch, lv.committed_epoch);
  EXPECT_EQ(vv.term, lv.term);
  EXPECT_TRUE(vv.lease_valid);
  ASSERT_TRUE(vv.committed.has_value());
  EXPECT_EQ(vv.committed->targets.size(), 2u);
  std::filesystem::remove_all(dir);
}

TEST(MapMonitor, ObjectServiceGateRefusesWithoutLeaseAndDuringTransition) {
  using namespace aios;
  test::DualStoreFixture fx("aios-mapgate", 1, 1);
  const std::string oid = "gate/obj";
  const std::string body = "hello";
  auto put = [&] {
    return fx.svc->api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                           {}, true, {});
  };
  ASSERT_TRUE(put().ok);

  // Consensus on, epoch active, lease valid: unchanged behaviour.
  ObjectService::MapGate g;
  g.consensus = true;
  g.lease_valid = true;
  g.active_epoch = fx.svc->map().epoch;
  fx.svc->set_map_gate(g);
  EXPECT_TRUE(put().ok);

  // No lease from the leader: refuse, retryable.
  g.lease_valid = false;
  fx.svc->set_map_gate(g);
  auto r = put();
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.code, "no_map_lease");

  // Newer epoch not yet active and no previous map (fresh node): refuse.
  g.lease_valid = true;
  g.active_epoch = fx.svc->map().epoch - 1;
  g.previous.reset();
  fx.svc->set_map_gate(g);
  r = put();
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.code, "map_transition");

  // Same node was primary in the previous map: no handover, proceed.
  g.previous = fx.svc->map();
  fx.svc->set_map_gate(g);
  EXPECT_TRUE(put().ok);

  // Previous map had someone else as primary: wait for activation.
  auto prev = fx.svc->map();
  for (auto& t : prev.targets) t.node_id = "someone-else";
  g.previous = prev;
  fx.svc->set_map_gate(g);
  r = put();
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.code, "map_transition");

  // Activated: proceed.
  g.active_epoch = fx.svc->map().epoch;
  fx.svc->set_map_gate(g);
  EXPECT_TRUE(put().ok);
}

TEST(MapMonitor, ReplicaFencesOlderEpochInConsensusMode) {
  using namespace aios;
  test::DualStoreFixture fx("aios-mapfence", 1, 1);
  ClusterMap m = fx.svc->map();
  m.epoch = 10;
  fx.svc->update_cluster_map(m);
  ObjectService::MapGate g;
  g.consensus = true;
  g.lease_valid = true;
  g.active_epoch = 10;
  fx.svc->set_map_gate(g);

  auto stat_with_epoch = [&](std::uint64_t e) {
    Frame req;
    req.type = MsgType::ObjectStat;
    req.body = {{"epoch", e}, {"aios_path", fx.p1}, {"oid", "fence/x"}};
    return fx.svc->handle(req);
  };
  EXPECT_NE(stat_with_epoch(10).body.value("code", ""), "epoch_mismatch");
  // Newer epoch from a primary that is ahead of us: accepted, and it raises the fence.
  EXPECT_NE(stat_with_epoch(12).body.value("code", ""), "epoch_mismatch");
  EXPECT_EQ(stat_with_epoch(11).body.value("code", ""), "epoch_mismatch");
  EXPECT_EQ(stat_with_epoch(9).body.value("code", ""), "epoch_mismatch");
  EXPECT_NE(stat_with_epoch(12).body.value("code", ""), "epoch_mismatch");
}
