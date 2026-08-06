#include "cluster/cluster_map.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

#include <openssl/evp.h>

namespace aios {
namespace {

std::uint64_t hash_canonical(const std::string& s) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  EVP_Digest(s.data(), s.size(), md, &md_len, EVP_sha256(), nullptr);
  std::uint64_t v = 0;
  for (unsigned int i = 0; i < md_len && i < 8; ++i) {
    v = (v << 8) | md[i];
  }
  return v;
}

}  // namespace

std::string target_key(const StorageTarget& t) {
  return t.node_id + "\n" + t.aios_path;
}

std::string storage_class_of_target(const ClusterMap& map, const std::string& node_id,
                                    const std::string& aios_path) {
  for (const auto& t : map.targets) {
    if (t.node_id == node_id && t.aios_path == aios_path) return t.storage_class;
  }
  return {};
}

std::vector<StorageTarget> ClusterMap::targets_for_class(
    const std::string& storage_class) const {
  std::vector<StorageTarget> out;
  for (const auto& t : targets) {
    if (t.storage_class == storage_class) out.push_back(t);
  }
  return out;
}

nlohmann::json ClusterMap::to_json() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& t : targets) {
    arr.push_back({
        {"node_id", t.node_id},
        {"addr", t.addr},
        {"http_addr", t.http_addr},
        {"aios_path", t.aios_path},
        {"mount", t.mount},
        {"storage_class", t.storage_class},
        {"rack", t.rack},
        {"weight", t.weight},
        {"state", lifecycle_state_name(t.state)},
        {"bavail", t.bavail},
    });
  }
  return {
      {"epoch", epoch},
      {"replica_count", replica_count},
      {"placement",
       {{"vnodes_per_target", placement.vnodes_per_target},
        {"min_vnodes", placement.min_vnodes},
        {"max_vnodes", placement.max_vnodes}}},
      {"targets", std::move(arr)},
  };
}

ClusterMap ClusterMap::from_json(const nlohmann::json& j) {
  ClusterMap m;
  if (!j.is_object()) return m;
  m.epoch = j.value("epoch", static_cast<std::uint64_t>(0));
  m.replica_count = j.value("replica_count", 3);
  if (j.contains("placement") && j["placement"].is_object()) {
    const auto& p = j["placement"];
    m.placement.vnodes_per_target = p.value("vnodes_per_target", 128);
    m.placement.min_vnodes = p.value("min_vnodes", 16);
    m.placement.max_vnodes = p.value("max_vnodes", 1024);
  }
  if (!j.contains("targets") || !j["targets"].is_array()) return m;
  for (const auto& e : j["targets"]) {
    if (!e.is_object()) continue;
    StorageTarget t;
    t.node_id = e.value("node_id", "");
    t.addr = e.value("addr", "");
    t.http_addr = e.value("http_addr", "");
    t.aios_path = e.value("aios_path", "");
    t.mount = e.value("mount", "");
    t.storage_class = e.value("storage_class", "");
    t.rack = e.value("rack", "");
    if (t.rack.empty() && !t.node_id.empty()) t.rack = t.node_id;
    t.weight = e.value("weight", 1);
    t.state = lifecycle_state_from_string(e.value("state", "up"));
    t.bavail = e.value("bavail", static_cast<std::uint64_t>(0));
    if (!t.node_id.empty() && !t.aios_path.empty() && !t.storage_class.empty() &&
        t.state != LifecycleState::Off) {
      m.targets.push_back(std::move(t));
    }
  }
  return m;
}

ClusterMap ClusterMap::build(const MembershipTable& membership, const FsTable& fs_table,
                             int replica_count, const PlacementConfig& placement) {
  ClusterMap map;
  map.replica_count = replica_count > 0 ? replica_count : 1;
  map.placement = placement;
  if (map.placement.vnodes_per_target < 1) map.placement.vnodes_per_target = 1;
  if (map.placement.min_vnodes < 1) map.placement.min_vnodes = 1;
  if (map.placement.max_vnodes < map.placement.min_vnodes) {
    map.placement.max_vnodes = map.placement.min_vnodes;
  }

  std::unordered_map<std::string, Member> alive;
  for (const auto& m : membership.snapshot()) {
    if (m.state != MemberState::Online) continue;
    if (m.node_id.empty() || m.node_id.rfind("seed:", 0) == 0) continue;
    if (m.addr.empty()) continue;
    alive[m.node_id] = m;
  }

  for (const auto& e : fs_table.snapshot()) {
    if (!e.usable) continue;
    if (e.aios_path.empty() || e.node_id.empty()) continue;
    if (e.storage_class.empty()) continue;
    if (e.state == LifecycleState::Off) continue;
    auto it = alive.find(e.node_id);
    if (it == alive.end()) continue;
    StorageTarget t;
    t.node_id = e.node_id;
    t.addr = it->second.addr;
    t.http_addr = it->second.http_addr;
    t.aios_path = e.aios_path;
    t.mount = e.mount;
    t.storage_class = e.storage_class;
    t.rack = e.rack.empty() ? e.node_id : e.rack;
    t.weight = e.weight > 0 ? e.weight : 1;
    t.state = e.state;
    t.bavail = e.bavail;
    map.targets.push_back(std::move(t));
  }

  std::sort(map.targets.begin(), map.targets.end(),
            [](const StorageTarget& a, const StorageTarget& b) {
              if (a.storage_class != b.storage_class) {
                return a.storage_class < b.storage_class;
              }
              if (a.node_id != b.node_id) return a.node_id < b.node_id;
              return a.aios_path < b.aios_path;
            });

  std::ostringstream canon;
  canon << "replica_count=" << map.replica_count << '\n';
  canon << "vnodes_per_target=" << map.placement.vnodes_per_target << '\n';
  canon << "min_vnodes=" << map.placement.min_vnodes << '\n';
  canon << "max_vnodes=" << map.placement.max_vnodes << '\n';
  for (const auto& t : map.targets) {
    canon << t.storage_class << '\t' << t.node_id << '\t' << t.rack << '\t' << t.addr << '\t'
          << t.aios_path << '\t' << t.weight << '\t' << lifecycle_state_name(t.state) << '\n';
  }
  map.epoch = hash_canonical(canon.str());
  return map;
}

}  // namespace aios
