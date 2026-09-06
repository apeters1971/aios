#include "cluster/map_monitor.hpp"

#include "util/log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <unordered_set>

namespace aios {

namespace {

std::uint64_t jul(const nlohmann::json& j, const char* key, std::uint64_t def = 0) {
  auto it = j.find(key);
  if (it == j.end()) return def;
  if (it->is_number_unsigned()) return it->get<std::uint64_t>();
  if (it->is_number_integer() && it->get<std::int64_t>() >= 0) {
    return static_cast<std::uint64_t>(it->get<std::int64_t>());
  }
  return def;
}

std::string jstr(const nlohmann::json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

// Raft's log comparison on the single register: (term, epoch) lexicographic.
bool at_least(std::uint64_t term_a, std::uint64_t epoch_a, std::uint64_t term_b,
              std::uint64_t epoch_b) {
  if (term_a != term_b) return term_a > term_b;
  return epoch_a >= epoch_b;
}

}  // namespace

// ---- Entry ------------------------------------------------------------------

nlohmann::json MapMonitor::Entry::to_json() const {
  return {{"term", term}, {"epoch", epoch}, {"map", map.to_json()}};
}

bool MapMonitor::Entry::from_json(const nlohmann::json& j, Entry& out) {
  if (!j.is_object()) return false;
  auto m = j.find("map");
  if (m == j.end() || !m->is_object()) return false;
  out.term = jul(j, "term");
  out.epoch = jul(j, "epoch");
  out.map = ClusterMap::from_json(*m);
  out.map.epoch = out.epoch;
  return out.epoch != 0;
}

nlohmann::json MapMonitor::View::to_json() const {
  return {{"enabled", enabled},
          {"voter", voter},
          {"leader", leader},
          {"leader_addr", leader_addr},
          {"term", term},
          {"lease_valid", lease_valid},
          {"committed_epoch", committed_epoch},
          {"active_epoch", active_epoch}};
}

// ---- Construction / persistence -------------------------------------------------

MapMonitor::MapMonitor(Options opts, SendFn send)
    : opts_(std::move(opts)), send_(std::move(send)), rng_(std::random_device{}()) {
  std::sort(opts_.voters.begin(), opts_.voters.end());
  opts_.voters.erase(std::unique(opts_.voters.begin(), opts_.voters.end()), opts_.voters.end());
  voter_ = std::find(opts_.voters.begin(), opts_.voters.end(), opts_.self_addr) !=
           opts_.voters.end();
  if (opts_.tick_ms <= 0) opts_.tick_ms = 1000;
  if (opts_.lease_ms <= 0) opts_.lease_ms = 10000;
  if (opts_.election_min_ticks < 1) opts_.election_min_ticks = 1;
  if (opts_.election_max_ticks < opts_.election_min_ticks) {
    opts_.election_max_ticks = opts_.election_min_ticks;
  }
  load_state();
}

bool MapMonitor::load_state_file(Persisted& out) const {
  if (opts_.state_path.empty()) return false;
  std::ifstream in(opts_.state_path);
  if (!in) return false;
  try {
    nlohmann::json j;
    in >> j;
    if (!j.is_object()) return false;
    out.term = jul(j, "term");
    out.voted_for = jstr(j, "voted_for");
    out.committed_epoch = jul(j, "committed_epoch");
    if (auto e = j.find("entry"); e != j.end()) {
      Entry ent;
      if (Entry::from_json(*e, ent)) out.entry = std::move(ent);
    }
    return true;
  } catch (const std::exception& e) {
    AIOS_LOG_WARN("map monitor: ignoring unreadable state ", opts_.state_path, ": ", e.what());
    return false;
  }
}

void MapMonitor::load_state() {
  Persisted p;
  if (!load_state_file(p)) return;
  std::lock_guard lock(mu_);
  term_ = p.term;
  voted_for_ = p.voted_for;
  entry_ = p.entry;
  if (p.committed_epoch != 0 && p.committed_epoch == entry_.epoch) {
    committed_ = entry_.map;
    committed_epoch_ = entry_.epoch;
    // Learned "long ago": a restarted node has no lease until the leader talks
    // to it, and its committed map counts as active once the lease window passed.
    committed_learned_ms_ = 0;
  }
  AIOS_LOG_INFO("map monitor: restored term=", term_, " epoch=", entry_.epoch,
                " committed=", committed_epoch_);
}

void MapMonitor::save_state_locked() const {
  if (opts_.state_path.empty() || !voter_) return;
  nlohmann::json j = {{"term", term_},
                      {"voted_for", voted_for_},
                      {"committed_epoch", committed_epoch_},
                      {"entry", entry_.to_json()}};
  const std::string tmp = opts_.state_path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      AIOS_LOG_WARN("map monitor: cannot write ", tmp);
      return;
    }
    out << j.dump() << '\n';
    out.flush();
  }
  std::error_code ec;
  std::filesystem::rename(tmp, opts_.state_path, ec);
  if (ec) AIOS_LOG_WARN("map monitor: rename ", tmp, ": ", ec.message());
}

// ---- Helpers -----------------------------------------------------------------------

void MapMonitor::reset_election_deadline_locked(std::int64_t now_ms) {
  std::uniform_int_distribution<int> d(opts_.election_min_ticks, opts_.election_max_ticks);
  election_deadline_ms_ = now_ms + static_cast<std::int64_t>(d(rng_)) * opts_.tick_ms;
}

void MapMonitor::become_follower_locked(std::uint64_t term, std::int64_t now_ms) {
  if (term > term_) {
    term_ = term;
    voted_for_.clear();
  }
  if (role_ == Role::Leader) AIOS_LOG_INFO("map monitor: stepping down, term=", term_);
  role_ = Role::Follower;
  acked_.clear();
  reset_election_deadline_locked(now_ms);
  save_state_locked();
}

bool MapMonitor::same_content(const ClusterMap& a, const ClusterMap& b) const {
  return a.content_hash() == b.content_hash();
}

void MapMonitor::set_committed_locked(std::uint64_t commit_epoch, std::int64_t now_ms) {
  if (commit_epoch == 0 || commit_epoch < entry_.epoch || commit_epoch == committed_epoch_) {
    return;
  }
  // commit_epoch >= entry_.epoch: the entry we hold is (at least) what the leader
  // committed; a leader never reports a commit index past what it sent us.
  if (committed_ && committed_epoch_ != entry_.epoch) previous_ = committed_;
  committed_ = entry_.map;
  committed_epoch_ = entry_.epoch;
  committed_learned_ms_ = now_ms;
  save_state_locked();
  AIOS_LOG_INFO("map monitor: committed epoch=", committed_epoch_, " term=", entry_.term,
                " targets=", entry_.map.targets.size());
}

void MapMonitor::recompute_active_locked() {
  if (role_ != Role::Leader || !committed_) return;
  if (leader_active_epoch_ >= committed_epoch_) return;
  for (const auto& t : committed_->targets) {
    if (t.node_id == opts_.node_id) continue;
    auto it = acked_.find(t.addr);
    if (it == acked_.end() || it->second < committed_epoch_) return;
  }
  leader_active_epoch_ = committed_epoch_;
}

std::uint64_t MapMonitor::local_active_locked(std::int64_t now_ms) const {
  if (!committed_) return 0;
  if (leader_active_epoch_ >= committed_epoch_) return committed_epoch_;
  if (committed_learned_ms_ + opts_.lease_ms <= now_ms) return committed_epoch_;
  return previous_ ? previous_->epoch : 0;
}

// ---- Tick ------------------------------------------------------------------------

void MapMonitor::tick(std::int64_t now_ms, const std::vector<std::string>& learners) {
  (void)learners;  // learners pull; the leader no longer pushes the map to them
  Role role;
  bool voter;
  {
    std::lock_guard lock(mu_);
    if (election_deadline_ms_ == 0) reset_election_deadline_locked(now_ms);
    role = role_;
    voter = voter_;
    if (!voter) {
      // Pull outside the lock.
    } else if (role != Role::Leader && now_ms < election_deadline_ms_) {
      return;
    }
  }
  if (!voter) {
    run_pull(now_ms);
    return;
  }
  if (role == Role::Leader) {
    run_heartbeat(now_ms);
  } else {
    run_election(now_ms);
    bool leader_now;
    {
      std::lock_guard lock(mu_);
      leader_now = role_ == Role::Leader;
    }
    if (leader_now) run_heartbeat(now_ms);
  }
}

std::vector<std::optional<nlohmann::json>> MapMonitor::broadcast(
    const std::vector<std::string>& peers, const nlohmann::json& req) {
  std::vector<std::optional<nlohmann::json>> replies(peers.size());
  std::vector<std::thread> th;
  th.reserve(peers.size());
  for (std::size_t i = 0; i < peers.size(); ++i) {
    th.emplace_back([&, i] {
      try {
        replies[i] = send_(peers[i], req);
      } catch (...) {
        replies[i] = std::nullopt;
      }
    });
  }
  for (auto& t : th) t.join();
  return replies;
}

bool MapMonitor::run_prevote(std::int64_t now_ms) {
  // Raft Pre-Vote: ask whether a real election for term+1 could succeed, without
  // touching term_. A node that is partitioned off, or just restarted next to a
  // healthy leader, gets no pre-votes (peers still hear their leader) and so
  // never bumps its term and unseats that leader when it reconnects.
  nlohmann::json req;
  std::vector<std::string> peers;
  {
    std::lock_guard lock(mu_);
    req = {{"op", "prevote"},
           {"term", term_ + 1},
           {"cand", opts_.self_addr},
           {"last_term", entry_.term},
           {"last_epoch", entry_.epoch}};
    for (const auto& v : opts_.voters) {
      if (v != opts_.self_addr) peers.push_back(v);
    }
  }
  const auto replies = broadcast(peers, req);
  std::lock_guard lock(mu_);
  std::size_t granted = 1;
  for (const auto& r : replies) {
    if (!r || !r->is_object()) continue;
    const auto t = jul(*r, "term");
    if (t > term_) {
      become_follower_locked(t, now_ms);
      return false;
    }
    if (r->value("granted", false)) ++granted;
  }
  if (granted < majority()) {
    reset_election_deadline_locked(now_ms);
    return false;
  }
  return true;
}

void MapMonitor::run_election(std::int64_t now_ms) {
  if (!run_prevote(now_ms)) return;
  nlohmann::json req;
  std::vector<std::string> peers;
  std::uint64_t term;
  {
    std::lock_guard lock(mu_);
    if (role_ == Role::Leader) return;
    role_ = Role::Candidate;
    ++term_;
    voted_for_ = opts_.self_addr;
    leader_addr_.clear();
    reset_election_deadline_locked(now_ms);
    save_state_locked();
    term = term_;
    req = {{"op", "vote"},
           {"term", term_},
           {"cand", opts_.self_addr},
           {"last_term", entry_.term},
           {"last_epoch", entry_.epoch}};
    for (const auto& v : opts_.voters) {
      if (v != opts_.self_addr) peers.push_back(v);
    }
    AIOS_LOG_INFO("map monitor: election term=", term_, " voters=", opts_.voters.size());
  }
  const auto replies = broadcast(peers, req);
  std::lock_guard lock(mu_);
  if (role_ != Role::Candidate || term_ != term) return;  // superseded meanwhile
  std::size_t granted = 1;  // self
  for (const auto& r : replies) {
    if (!r || !r->is_object()) continue;
    const auto t = jul(*r, "term");
    if (t > term_) {
      become_follower_locked(t, now_ms);
      return;
    }
    if (r->value("granted", false)) ++granted;
  }
  if (granted < majority()) return;  // retry after the next deadline
  role_ = Role::Leader;
  leader_addr_ = opts_.self_addr;
  acked_.clear();
  last_majority_ack_ms_ = now_ms;
  // Raft's no-op entry: re-publish the latest map under this term so it (and
  // everything before it) commits by this term's majority.
  Entry e;
  e.term = term_;
  e.epoch = entry_.epoch + 1;
  e.map = entry_.map;
  e.map.epoch = e.epoch;
  entry_ = std::move(e);
  acked_[opts_.self_addr] = entry_.epoch;
  save_state_locked();
  AIOS_LOG_INFO("map monitor: leader term=", term_, " epoch=", entry_.epoch);
}

void MapMonitor::run_heartbeat(std::int64_t now_ms) {
  struct Target {
    std::string addr;
    bool voter{false};
    nlohmann::json req;
  };
  std::vector<Target> targets;
  std::uint64_t term;
  std::uint64_t entry_epoch;
  {
    std::lock_guard lock(mu_);
    if (role_ != Role::Leader) return;
    if (last_majority_ack_ms_ != 0 && now_ms - last_majority_ack_ms_ >= opts_.lease_ms) {
      // Cut off from the majority for a whole lease: the others may have elected
      // a successor by now. Step down so our heartbeats stop lending followers on
      // our side of the partition a lease they must not have.
      AIOS_LOG_WARN("map monitor: no majority for ", opts_.lease_ms, " ms, stepping down");
      become_follower_locked(term_, now_ms);
      return;
    }
    term = term_;
    entry_epoch = entry_.epoch;
    recompute_active_locked();
    const std::int64_t remaining =
        last_majority_ack_ms_ == 0 ? 0
                                   : opts_.lease_ms - (now_ms - last_majority_ack_ms_);
    nlohmann::json base = {{"op", "append"},
                           {"term", term_},
                           {"leader", opts_.self_addr},
                           {"commit_epoch", committed_epoch_},
                           {"active_epoch", leader_active_epoch_},
                           {"lease_remaining_ms", std::max<std::int64_t>(0, remaining)}};
    std::unordered_set<std::string> seen{opts_.self_addr};
    auto add = [&](const std::string& addr, bool voter) {
      if (addr.empty() || !seen.insert(addr).second) return;
      Target t;
      t.addr = addr;
      t.voter = voter;
      t.req = base;
      auto it = acked_.find(addr);
      if (it == acked_.end() || it->second != entry_.epoch) t.req["entry"] = entry_.to_json();
      targets.push_back(std::move(t));
    };
    for (const auto& v : opts_.voters) add(v, true);
    // Learners pull; do not unicast the map to every storage node.
  }
  std::vector<std::optional<nlohmann::json>> replies(targets.size());
  {
    std::vector<std::thread> th;
    th.reserve(targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
      th.emplace_back([&, i] {
        try {
          replies[i] = send_(targets[i].addr, targets[i].req);
        } catch (...) {
          replies[i] = std::nullopt;
        }
      });
    }
    for (auto& t : th) t.join();
  }
  std::lock_guard lock(mu_);
  if (role_ != Role::Leader || term_ != term) return;
  std::size_t voter_acks = 1;    // self is a voter iff listed; counted below
  std::size_t entry_acks = 1;
  if (!voter_) voter_acks = entry_acks = 0;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    const auto& r = replies[i];
    if (!r || !r->is_object()) continue;
    const auto t = jul(*r, "term");
    if (t > term_) {
      become_follower_locked(t, now_ms);
      return;
    }
    if (!r->value("ok", false)) continue;
    const auto last = jul(*r, "last_epoch");
    acked_[targets[i].addr] = last;
    const auto nid = jstr(*r, "node_id");
    if (!nid.empty()) node_of_addr_[targets[i].addr] = nid;
    if (targets[i].voter) {
      ++voter_acks;
      if (last == entry_epoch && entry_epoch == entry_.epoch) ++entry_acks;
    }
  }
  if (voter_acks >= majority()) last_majority_ack_ms_ = now_ms;
  if (entry_acks >= majority()) set_committed_locked(entry_.epoch, now_ms);
  recompute_active_locked();
}

// ---- Inbound --------------------------------------------------------------------------

nlohmann::json MapMonitor::handle(const nlohmann::json& req, std::int64_t now_ms) {
  if (!req.is_object()) return {{"ok", false}, {"error", "bad request"}};
  const auto op = jstr(req, "op");
  if (op == "vote") return handle_vote(req, now_ms);
  if (op == "prevote") return handle_prevote(req, now_ms);
  if (op == "append") return handle_append(req, now_ms);
  if (op == "pull") return handle_pull(req, now_ms);
  return {{"ok", false}, {"error", "unknown op"}};
}

nlohmann::json MapMonitor::handle_prevote(const nlohmann::json& req, std::int64_t now_ms) {
  std::lock_guard lock(mu_);
  const auto term = jul(req, "term");
  if (!voter_ || term < term_) return {{"term", term_}, {"granted", false}};
  // Still hearing from a live leader (or being one with a lease): the candidate
  // is the one that is cut off, not us. Do not help it disrupt the term.
  const bool leader_alive = role_ == Role::Leader
                                ? (last_majority_ack_ms_ != 0 &&
                                   now_ms - last_majority_ack_ms_ < opts_.lease_ms)
                                : now_ms < lease_until_ms_;
  const bool log_ok =
      at_least(jul(req, "last_term"), jul(req, "last_epoch"), entry_.term, entry_.epoch);
  return {{"term", term_}, {"granted", log_ok && !leader_alive}};
}

nlohmann::json MapMonitor::handle_vote(const nlohmann::json& req, std::int64_t now_ms) {
  std::lock_guard lock(mu_);
  const auto term = jul(req, "term");
  const auto cand = jstr(req, "cand");
  if (!voter_ || cand.empty() || term < term_) return {{"term", term_}, {"granted", false}};
  if (term > term_) become_follower_locked(term, now_ms);
  const bool log_ok =
      at_least(jul(req, "last_term"), jul(req, "last_epoch"), entry_.term, entry_.epoch);
  const bool may_vote = voted_for_.empty() || voted_for_ == cand;
  const bool granted = log_ok && may_vote;
  if (granted) {
    voted_for_ = cand;
    reset_election_deadline_locked(now_ms);
    save_state_locked();
  }
  return {{"term", term_}, {"granted", granted}};
}

nlohmann::json MapMonitor::handle_append(const nlohmann::json& req, std::int64_t now_ms) {
  std::lock_guard lock(mu_);
  const auto term = jul(req, "term");
  if (term < term_) {
    return {{"term", term_}, {"ok", false}, {"last_term", entry_.term},
            {"last_epoch", entry_.epoch}, {"node_id", opts_.node_id}};
  }
  if (term > term_ || role_ != Role::Follower) become_follower_locked(term, now_ms);
  leader_addr_ = jstr(req, "leader");
  {
    const auto remaining =
        static_cast<std::int64_t>(jul(req, "lease_remaining_ms", opts_.lease_ms));
    const auto grant = std::min<std::int64_t>(opts_.lease_ms, std::max<std::int64_t>(0, remaining));
    lease_until_ms_ = std::max(lease_until_ms_, now_ms + grant);
  }
  reset_election_deadline_locked(now_ms);
  bool changed = false;
  if (auto e = req.find("entry"); e != req.end()) {
    Entry ent;
    if (Entry::from_json(*e, ent) && at_least(ent.term, ent.epoch, entry_.term, entry_.epoch)) {
      changed = ent.epoch != entry_.epoch || ent.term != entry_.term;
      entry_ = std::move(ent);
    }
  }
  const auto commit = jul(req, "commit_epoch");
  const auto before = committed_epoch_;
  set_committed_locked(std::min(commit, entry_.epoch), now_ms);
  leader_commit_epoch_ = std::max(leader_commit_epoch_, commit);
  leader_active_epoch_ = jul(req, "active_epoch");
  if (changed && committed_epoch_ == before) save_state_locked();
  return {{"term", term_}, {"ok", true}, {"last_term", entry_.term},
          {"last_epoch", entry_.epoch}, {"node_id", opts_.node_id}};
}

void MapMonitor::run_pull(std::int64_t now_ms) {
  nlohmann::json req;
  std::vector<std::string> peers;
  std::string leader;
  {
    std::lock_guard lock(mu_);
    req = {{"op", "pull"},
           {"have_epoch", committed_epoch_},
           {"from", opts_.self_addr},
           {"node_id", opts_.node_id}};
    peers = opts_.voters;
    leader = leader_addr_;
  }
  if (peers.empty()) return;

  auto try_one = [&](const std::string& addr) -> std::optional<nlohmann::json> {
    try {
      return send_(addr, req);
    } catch (...) {
      return std::nullopt;
    }
  };

  // Acks that drive activation live on the leader. Prefer it once known so a
  // learner that can reach a follower does not skip the leader forever.
  std::vector<std::string> order;
  order.reserve(peers.size());
  if (!leader.empty() && std::find(peers.begin(), peers.end(), leader) != peers.end()) {
    order.push_back(leader);
  }
  for (const auto& p : peers) {
    if (p != leader) order.push_back(p);
  }
  for (const auto& addr : order) {
    auto reply = try_one(addr);
    if (!reply || !reply->is_object() || !reply->value("ok", false)) continue;
    apply_pull_reply(*reply, now_ms);
    // Stop once a known leader (or any voter, before we know one) grants a lease.
    // A follower grant is not enough: keep going so the leader records the ack.
    if (jul(*reply, "lease_remaining_ms") > 0 && (leader.empty() || addr == leader)) return;
  }
}

void MapMonitor::apply_pull_reply(const nlohmann::json& r, std::int64_t now_ms) {
  std::lock_guard lock(mu_);
  const auto term = jul(r, "term");
  if (term < term_) return;
  if (term > term_ || role_ == Role::Leader) become_follower_locked(term, now_ms);
  const auto leader = jstr(r, "leader");
  if (!leader.empty()) leader_addr_ = leader;
  {
    const auto remaining =
        static_cast<std::int64_t>(jul(r, "lease_remaining_ms", opts_.lease_ms));
    const auto grant = std::min<std::int64_t>(opts_.lease_ms, std::max<std::int64_t>(0, remaining));
    lease_until_ms_ = std::max(lease_until_ms_, now_ms + grant);
  }
  reset_election_deadline_locked(now_ms);
  bool changed = false;
  if (auto e = r.find("entry"); e != r.end()) {
    Entry ent;
    if (Entry::from_json(*e, ent) && at_least(ent.term, ent.epoch, entry_.term, entry_.epoch)) {
      changed = ent.epoch != entry_.epoch || ent.term != entry_.term;
      entry_ = std::move(ent);
    }
  }
  const auto before = committed_epoch_;
  const auto commit = jul(r, "commit_epoch");
  set_committed_locked(std::min(commit, entry_.epoch), now_ms);
  leader_commit_epoch_ = std::max(leader_commit_epoch_, commit);
  leader_active_epoch_ = jul(r, "active_epoch");
  if (changed && committed_epoch_ == before) save_state_locked();
}

nlohmann::json MapMonitor::handle_pull(const nlohmann::json& req, std::int64_t now_ms) {
  std::lock_guard lock(mu_);
  const auto have = jul(req, "have_epoch");
  const auto from = jstr(req, "from");
  if (role_ == Role::Leader && !from.empty()) {
    acked_[from] = have;
    const auto nid = jstr(req, "node_id");
    if (!nid.empty()) node_of_addr_[from] = nid;
    recompute_active_locked();
  }
  nlohmann::json reply = {{"ok", true},
                          {"term", term_},
                          {"leader", role_ == Role::Leader ? opts_.self_addr : leader_addr_},
                          {"commit_epoch", committed_epoch_},
                          {"last_term", entry_.term},
                          {"last_epoch", entry_.epoch},
                          {"node_id", opts_.node_id}};
  if (role_ == Role::Leader) {
    reply["active_epoch"] = leader_active_epoch_;
  } else {
    reply["active_epoch"] = local_active_locked(now_ms);
  }
  std::int64_t remaining = 0;
  if (role_ == Role::Leader) {
    if (last_majority_ack_ms_ != 0) {
      remaining = opts_.lease_ms - (now_ms - last_majority_ack_ms_);
    }
  } else {
    remaining = lease_until_ms_ - now_ms;
  }
  remaining = std::min<std::int64_t>(opts_.lease_ms, std::max<std::int64_t>(0, remaining));
  reply["lease_remaining_ms"] = remaining;
  if (committed_ && have < committed_epoch_) {
    Entry e;
    e.term = entry_.term;
    e.epoch = committed_epoch_;
    e.map = *committed_;
    e.map.epoch = e.epoch;
    reply["entry"] = e.to_json();
  }
  return reply;
}

// ---- Leader API / view ---------------------------------------------------------------

void MapMonitor::propose(const ClusterMap& content, std::int64_t now_ms) {
  std::lock_guard lock(mu_);
  if (role_ != Role::Leader) return;
  if (entry_.epoch != 0 && same_content(entry_.map, content)) return;
  Entry e;
  e.term = term_;
  e.epoch = entry_.epoch + 1;
  e.map = content;
  e.map.epoch = e.epoch;
  entry_ = std::move(e);
  acked_[opts_.self_addr] = entry_.epoch;
  save_state_locked();
  AIOS_LOG_INFO("map monitor: proposing epoch=", entry_.epoch, " targets=",
                entry_.map.targets.size());
  (void)now_ms;
}

MapMonitor::View MapMonitor::view(std::int64_t now_ms) const {
  std::lock_guard lock(mu_);
  View v;
  v.voter = voter_;
  v.leader = role_ == Role::Leader;
  v.leader_addr = leader_addr_;
  v.term = term_;
  v.committed_epoch = committed_epoch_;
  v.committed = committed_;
  v.previous = previous_;
  v.active_epoch = local_active_locked(now_ms);
  if (v.leader) {
    v.lease_valid = last_majority_ack_ms_ != 0 && now_ms - last_majority_ack_ms_ < opts_.lease_ms;
  } else {
    // A follower that knows the leader has committed past what it holds (a
    // restarted node catching up, one heartbeat behind) has no business acting
    // as primary under its stale map: replicas would only fence its writes.
    // Withhold the lease until it has the committed entry.
    v.lease_valid = now_ms < lease_until_ms_ && committed_epoch_ >= leader_commit_epoch_;
  }
  return v;
}

std::uint64_t MapMonitor::term() const {
  std::lock_guard lock(mu_);
  return term_;
}

bool MapMonitor::is_leader() const {
  std::lock_guard lock(mu_);
  return role_ == Role::Leader;
}

std::uint64_t MapMonitor::last_epoch() const {
  std::lock_guard lock(mu_);
  return entry_.epoch;
}

}  // namespace aios
