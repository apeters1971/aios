#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace aios {

// Gossip liveness (independent of operator lifecycle up/drain/off).
enum class MemberState { Online, Suspect, Offline };

inline const char* member_state_name(MemberState s) {
  switch (s) {
    case MemberState::Online:
      return "online";
    case MemberState::Suspect:
      return "suspect";
    case MemberState::Offline:
      return "offline";
  }
  return "unknown";
}

// Accept new names and legacy alive/dead for mixed-version gossip.
inline MemberState member_state_from_string(const std::string& s) {
  if (s == "online" || s == "alive") return MemberState::Online;
  if (s == "suspect") return MemberState::Suspect;
  return MemberState::Offline;  // offline, dead, or unknown
}

struct Member {
  std::string node_id;
  std::string addr;       // host:port TCP++ dial
  std::string http_addr;  // host:port HTTP API (may be empty)
  std::string rack;      // failure domain; empty until set_local / merge
  MemberState state{MemberState::Online};
  std::int64_t last_seen_ms{0};
};

class MembershipTable {
 public:
  void set_local(const std::string& node_id, const std::string& advertise_addr,
                 const std::string& http_addr = {}, const std::string& rack = {});

  // Upsert seed peer address (unknown node_id uses addr as provisional key until Hello).
  void add_seed(const std::string& addr);

  void mark_alive(const std::string& node_id, const std::string& addr, std::int64_t now,
                  const std::string& http_addr = {}, const std::string& rack = {});

  // Merge remote members. Prefer fresher last_seen; never demote local node.
  void merge(const std::vector<Member>& remote, std::int64_t now);

  void age(std::int64_t now, int suspect_after_ms, int dead_after_ms);

  std::vector<Member> snapshot() const;
  std::vector<Member> peers_for_gossip(std::size_t k) const;
  std::optional<Member> find(const std::string& node_id) const;

  nlohmann::json to_json() const;
  static std::vector<Member> from_json(const nlohmann::json& j);

 private:
  mutable std::mutex mu_;
  std::string local_id_;
  std::unordered_map<std::string, Member> members_;
};

}  // namespace aios
