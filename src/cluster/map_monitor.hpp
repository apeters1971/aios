#pragma once

#include "cluster/cluster_map.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

// Consensus-backed cluster map.
//
// The map used to be whatever each node rebuilt from its own gossip view, with an
// epoch that was a hash of the content: two partitions could each converge on a
// map in which a different node is primary for the same object, and nothing
// stopped both from accepting writes. The monitor replaces that with a Raft-style
// replicated register:
//
//  * A configured set of voters elects a leader (RequestVote with the usual
//    "candidate's last (term, epoch) must be at least mine" rule). Only the
//    leader publishes maps: it builds the content from gossip like every node
//    did before, and when the content changes it appends an entry with the next
//    epoch. Epochs are therefore a monotonic counter, not a hash.
//  * The leader replicates the latest entry to every voter. Storage nodes (learners)
//    pull the committed map on their own tick rather than receiving a unicast from
//    the leader, so a large cluster does not turn the leader into an N-way fan-out.
//    An entry is committed once a majority of voters acknowledged it; the commit
//    index is piggybacked on the next heartbeat. Only the latest entry is kept (a
//    newer map supersedes an older one completely), so the "log" is a single
//    register with Raft's election safety.
//  * Voters persist term / vote / entry / commit so a restart cannot vote twice
//    in one term or forget a committed epoch.
//
// Two derived facts drive the object service:
//
//  * Map lease. A node acts as primary only while it has heard from the leader
//    (or, as leader, from a majority) within lease_ms. A node cut off from the
//    leader stops serving before the leader can drop it from the map
//    (lease_ms < dead_after_ms), so the successor never overlaps with it.
//  * Activation. Epoch E is *active* on a node once the leader reports that
//    every node in map E acknowledged it, or lease_ms passed since the node
//    learned E (any node that has not acknowledged by then has lost its lease).
//    A node that became primary of an object in E waits for activation before
//    accepting writes to it, so the previous primary has provably stopped.
//
// Wire format (JSON body of MsgType::MapRpc, reply in an ObjectReply):
//   {"op":"vote","term":T,"cand":addr,"last_term":lt,"last_epoch":le}
//     -> {"term":T,"granted":bool}
//   {"op":"append","term":T,"leader":addr,"commit_epoch":C,"active_epoch":A,
//    "entry":{"term":t,"epoch":e,"map":{...}}?}
//     -> {"term":T,"ok":bool,"last_term":lt,"last_epoch":le,"node_id":id}
//   {"op":"pull","have_epoch":E,"from":addr,"node_id":id}
//     -> {"term":T,"ok":bool,"leader":addr,"commit_epoch":C,"active_epoch":A,
//         "lease_remaining_ms":R,"entry":{...}?}
//
// tick() sends RPCs synchronously through the injected SendFn and must run off
// the io_context; handle() runs on RPC session workers; view() anywhere.
class MapMonitor {
 public:
  using SendFn = std::function<std::optional<nlohmann::json>(const std::string& addr,
                                                             const nlohmann::json& req)>;

  struct Options {
    std::string node_id;
    std::string self_addr;             // TCP++ advertise address (as listed in voters)
    std::vector<std::string> voters;   // TCP++ addresses; self votes only if listed
    std::string state_path;            // voter persistence; empty = memory only
    int tick_ms{1000};
    int lease_ms{10000};
    int election_min_ticks{3};
    int election_max_ticks{6};
  };

  struct Entry {
    std::uint64_t term{0};
    std::uint64_t epoch{0};  // log index; map.epoch == epoch
    ClusterMap map;
    nlohmann::json to_json() const;
    static bool from_json(const nlohmann::json& j, Entry& out);
  };

  struct View {
    bool enabled{true};
    bool voter{false};
    bool leader{false};
    bool lease_valid{false};
    std::string leader_addr;
    std::uint64_t term{0};
    std::uint64_t committed_epoch{0};
    std::uint64_t active_epoch{0};
    std::optional<ClusterMap> committed;  // latest committed map
    std::optional<ClusterMap> previous;   // committed map before `committed`
    nlohmann::json to_json() const;
  };

  MapMonitor(Options opts, SendFn send);

  bool is_voter() const { return voter_; }
  const Options& options() const { return opts_; }

  // One heartbeat / election / pull step. Learners pull the committed map from
  // voters; the leader heartbeats voters only (the unused `learners` argument is
  // kept so call sites compile while they migrate).
  void tick(std::int64_t now_ms, const std::vector<std::string>& learners = {});

  // Inbound RPC body -> reply body.
  nlohmann::json handle(const nlohmann::json& req, std::int64_t now_ms);

  // Leader: append a new entry when `content` differs from the latest entry's
  // content (epoch and bavail ignored). No-op on followers.
  void propose(const ClusterMap& content, std::int64_t now_ms);

  View view(std::int64_t now_ms) const;

  // Tests / diagnostics.
  std::uint64_t term() const;
  bool is_leader() const;
  std::uint64_t last_epoch() const;

 private:
  enum class Role { Follower, Candidate, Leader };

  struct Persisted {
    std::uint64_t term{0};
    std::string voted_for;
    Entry entry;
    std::uint64_t committed_epoch{0};
  };

  void load_state();
  void save_state_locked() const;
  bool load_state_file(Persisted& out) const;

  void become_follower_locked(std::uint64_t term, std::int64_t now_ms);
  void reset_election_deadline_locked(std::int64_t now_ms);
  std::size_t majority() const { return opts_.voters.size() / 2 + 1; }
  bool same_content(const ClusterMap& a, const ClusterMap& b) const;
  void set_committed_locked(std::uint64_t commit_epoch, std::int64_t now_ms);
  void recompute_active_locked();
  std::uint64_t local_active_locked(std::int64_t now_ms) const;

  void run_election(std::int64_t now_ms);
  void run_heartbeat(std::int64_t now_ms);
  void run_pull(std::int64_t now_ms);
  void apply_pull_reply(const nlohmann::json& reply, std::int64_t now_ms);
  std::vector<std::optional<nlohmann::json>> broadcast(const std::vector<std::string>& peers,
                                                      const nlohmann::json& req);
  bool run_prevote(std::int64_t now_ms);

  nlohmann::json handle_vote(const nlohmann::json& req, std::int64_t now_ms);
  nlohmann::json handle_prevote(const nlohmann::json& req, std::int64_t now_ms);
  nlohmann::json handle_append(const nlohmann::json& req, std::int64_t now_ms);
  nlohmann::json handle_pull(const nlohmann::json& req, std::int64_t now_ms);

  Options opts_;
  SendFn send_;
  bool voter_{false};

  mutable std::mutex mu_;
  Role role_{Role::Follower};
  std::uint64_t term_{0};
  std::string voted_for_;
  std::string leader_addr_;
  Entry entry_;                            // latest entry (may be uncommitted)
  std::optional<ClusterMap> committed_;    // latest committed map
  std::optional<ClusterMap> previous_;     // committed before committed_
  std::uint64_t committed_epoch_{0};
  std::int64_t committed_learned_ms_{0};   // when committed_ was adopted here
  std::uint64_t leader_active_epoch_{0};   // as reported by the leader
  std::uint64_t leader_commit_epoch_{0};   // highest commit index a leader told us
  // Follower: lease expiry granted by the last valid append. Bounded by the
  // leader's own remaining lease, so no follower outlives its leader's mandate.
  std::int64_t lease_until_ms_{0};
  std::int64_t last_majority_ack_ms_{0};   // leader: last round with majority acks
  std::int64_t election_deadline_ms_{0};
  // Leader bookkeeping: last epoch each peer acknowledged (voters + learners).
  std::unordered_map<std::string, std::uint64_t> acked_;
  std::unordered_map<std::string, std::string> node_of_addr_;
  std::mt19937_64 rng_;
};

}  // namespace aios
