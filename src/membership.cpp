#include "membership.hpp"

#include "util/log.hpp"

#include <algorithm>
#include <random>

namespace aios {

void MembershipTable::set_local(const std::string& node_id,
                                const std::string& advertise_addr,
                                const std::string& http_addr, const std::string& rack) {
  std::lock_guard lock(mu_);
  local_id_ = node_id;
  Member m;
  m.node_id = node_id;
  m.addr = advertise_addr;
  m.http_addr = http_addr;
  m.rack = rack.empty() ? node_id : rack;
  m.state = MemberState::Online;
  m.last_seen_ms = now_ms();
  members_[node_id] = std::move(m);
}

void MembershipTable::add_seed(const std::string& addr) {
  std::lock_guard lock(mu_);
  // Keep seeds addressable even before we know node_id; key by "seed:<addr>".
  const std::string key = "seed:" + addr;
  if (members_.count(key)) return;
  for (const auto& [_, m] : members_) {
    if (m.addr == addr) return;
  }
  Member m;
  m.node_id = key;
  m.addr = addr;
  m.state = MemberState::Suspect;
  m.last_seen_ms = 0;
  members_[key] = std::move(m);
}

void MembershipTable::mark_alive(const std::string& node_id, const std::string& addr,
                                 std::int64_t now, const std::string& http_addr,
                                 const std::string& rack) {
  std::lock_guard lock(mu_);
  // Drop provisional seed entries that match this addr.
  for (auto it = members_.begin(); it != members_.end();) {
    if (it->first.rfind("seed:", 0) == 0 && it->second.addr == addr &&
        it->first != node_id) {
      it = members_.erase(it);
    } else {
      ++it;
    }
  }
  Member& m = members_[node_id];
  m.node_id = node_id;
  if (!addr.empty()) m.addr = addr;
  if (!http_addr.empty()) m.http_addr = http_addr;
  if (!rack.empty()) m.rack = rack;
  else if (m.rack.empty()) m.rack = node_id;
  m.state = MemberState::Online;
  m.last_seen_ms = now;
}

void MembershipTable::merge(const std::vector<Member>& remote, std::int64_t now) {
  std::lock_guard lock(mu_);
  for (const auto& incoming : remote) {
    if (incoming.node_id.empty() || incoming.node_id.rfind("seed:", 0) == 0) continue;
    if (incoming.node_id == local_id_) continue;
    Member r = incoming;
    // last_seen_ms comes from the observer's system clock. A peer running ahead
    // would stamp a time age() can never reach, pinning a dead node Online across
    // the cluster, so no observation may be newer than our own clock.
    if (now > 0 && r.last_seen_ms > now) r.last_seen_ms = now;
    auto it = members_.find(r.node_id);
    if (it == members_.end()) {
      members_[r.node_id] = r;
      if (members_[r.node_id].rack.empty()) members_[r.node_id].rack = r.node_id;
      continue;
    }
    // Strictly newer wins. last_seen_ms freezes once a node stops answering, so every
    // peer ends up holding the same value while each ages it at a different moment;
    // accepting a tie would let a peer that has not aged yet overwrite the Offline we
    // just derived and flap the node back into the ring.
    if (r.last_seen_ms > it->second.last_seen_ms) {
      auto keep_addr = it->second.addr;
      auto keep_http = it->second.http_addr;
      auto keep_rack = it->second.rack;
      it->second = r;
      if (it->second.addr.empty()) it->second.addr = keep_addr;
      if (it->second.http_addr.empty()) it->second.http_addr = keep_http;
      if (it->second.rack.empty()) it->second.rack = keep_rack.empty() ? r.node_id : keep_rack;
    } else if (r.last_seen_ms == it->second.last_seen_ms) {
      // Same observation: fill gaps, but never replace locally derived liveness.
      if (it->second.addr.empty()) it->second.addr = r.addr;
      if (it->second.http_addr.empty()) it->second.http_addr = r.http_addr;
      if (it->second.rack.empty()) it->second.rack = r.rack.empty() ? r.node_id : r.rack;
    }
  }
}

void MembershipTable::age(std::int64_t now, int suspect_after_ms,
                          int dead_after_ms) {
  std::lock_guard lock(mu_);
  for (auto& [id, m] : members_) {
    if (id == local_id_) {
      m.state = MemberState::Online;
      m.last_seen_ms = now;
      continue;
    }
    if (id.rfind("seed:", 0) == 0) continue;
    if (m.last_seen_ms == 0) continue;
    const auto age_ms = now - m.last_seen_ms;
    if (age_ms >= dead_after_ms) {
      m.state = MemberState::Offline;
    } else if (age_ms >= suspect_after_ms) {
      m.state = MemberState::Suspect;
    }
  }
}

std::vector<Member> MembershipTable::snapshot() const {
  std::lock_guard lock(mu_);
  std::vector<Member> out;
  out.reserve(members_.size());
  for (const auto& [_, m] : members_) {
    if (m.node_id.rfind("seed:", 0) == 0) continue;
    out.push_back(m);
  }
  return out;
}

std::vector<Member> MembershipTable::peers_for_gossip(std::size_t k) const {
  std::lock_guard lock(mu_);
  std::vector<Member> online;
  std::vector<Member> suspect;
  std::vector<Member> offline;
  std::vector<Member> seeds;
  for (const auto& [id, m] : members_) {
    if (id == local_id_) continue;
    if (m.addr.empty()) continue;
    if (id.rfind("seed:", 0) == 0) {
      seeds.push_back(m);
      continue;
    }
    if (m.state == MemberState::Online) online.push_back(m);
    else if (m.state == MemberState::Suspect) suspect.push_back(m);
    else if (m.state == MemberState::Offline) offline.push_back(m);
  }

  thread_local std::mt19937 rng{std::random_device{}()};
  auto shuffle = [&](std::vector<Member>& v) {
    std::shuffle(v.begin(), v.end(), rng);
  };
  shuffle(online);
  shuffle(suspect);
  shuffle(offline);
  shuffle(seeds);

  std::vector<Member> out;
  if (!suspect.empty() && k > 0) {
    out.push_back(suspect.front());
  }
  // One offline probe per round, ahead of the online peers so a busy cluster still
  // spends a slot on it. Nothing else ever dials an offline member, so without this
  // a node that comes back is only rediscovered if it has seeds of its own to dial.
  if (!offline.empty() && out.size() < k) {
    out.push_back(offline.front());
  }
  for (const auto& m : online) {
    if (out.size() >= k) break;
    out.push_back(m);
  }
  for (const auto& m : seeds) {
    if (out.size() >= k) break;
    bool dup = false;
    for (const auto& o : out) {
      if (o.addr == m.addr) {
        dup = true;
        break;
      }
    }
    if (!dup) out.push_back(m);
  }
  return out;
}

std::optional<Member> MembershipTable::find(const std::string& node_id) const {
  std::lock_guard lock(mu_);
  auto it = members_.find(node_id);
  if (it == members_.end()) return std::nullopt;
  return it->second;
}

nlohmann::json MembershipTable::to_json() const {
  nlohmann::json members = nlohmann::json::array();
  for (const auto& m : snapshot()) {
    members.push_back({
        {"node_id", m.node_id},
        {"addr", m.addr},
        {"http_addr", m.http_addr},
        {"rack", m.rack},
        {"state", member_state_name(m.state)},
        {"last_seen_ms", m.last_seen_ms},
    });
  }
  return {{"members", members}};
}

std::vector<Member> MembershipTable::from_json(const nlohmann::json& j) {
  std::vector<Member> out;
  const auto& arr = j.contains("members") ? j.at("members") : j;
  if (!arr.is_array()) return out;
  for (const auto& e : arr) {
    Member m;
    m.node_id = e.value("node_id", "");
    m.addr = e.value("addr", "");
    m.http_addr = e.value("http_addr", "");
    m.rack = e.value("rack", "");
    if (m.rack.empty() && !m.node_id.empty()) m.rack = m.node_id;
    m.state = member_state_from_string(e.value("state", "online"));
    m.last_seen_ms = e.value("last_seen_ms", std::int64_t{0});
    if (!m.node_id.empty()) out.push_back(std::move(m));
  }
  return out;
}

}  // namespace aios
