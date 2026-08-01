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

nlohmann::json ClusterMap::to_json() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& t : targets) {
    arr.push_back({
        {"node_id", t.node_id},
        {"addr", t.addr},
        {"aios_path", t.aios_path},
        {"mount", t.mount},
        {"bavail", t.bavail},
    });
  }
  return {
      {"epoch", epoch},
      {"replica_count", replica_count},
      {"targets", std::move(arr)},
  };
}

ClusterMap ClusterMap::from_json(const nlohmann::json& j) {
  ClusterMap m;
  if (!j.is_object()) return m;
  m.epoch = j.value("epoch", static_cast<std::uint64_t>(0));
  m.replica_count = j.value("replica_count", 3);
  if (!j.contains("targets") || !j["targets"].is_array()) return m;
  for (const auto& e : j["targets"]) {
    if (!e.is_object()) continue;
    StorageTarget t;
    t.node_id = e.value("node_id", "");
    t.addr = e.value("addr", "");
    t.aios_path = e.value("aios_path", "");
    t.mount = e.value("mount", "");
    t.bavail = e.value("bavail", static_cast<std::uint64_t>(0));
    if (!t.node_id.empty() && !t.aios_path.empty()) {
      m.targets.push_back(std::move(t));
    }
  }
  return m;
}

ClusterMap ClusterMap::build(const MembershipTable& membership, const FsTable& fs_table,
                             int replica_count) {
  ClusterMap map;
  map.replica_count = replica_count > 0 ? replica_count : 1;

  std::unordered_map<std::string, Member> alive;
  for (const auto& m : membership.snapshot()) {
    if (m.state != MemberState::Alive) continue;
    if (m.node_id.empty() || m.node_id.rfind("seed:", 0) == 0) continue;
    if (m.addr.empty()) continue;
    alive[m.node_id] = m;
  }

  for (const auto& e : fs_table.snapshot()) {
    if (!e.usable) continue;
    if (e.aios_path.empty() || e.node_id.empty()) continue;
    auto it = alive.find(e.node_id);
    if (it == alive.end()) continue;
    StorageTarget t;
    t.node_id = e.node_id;
    t.addr = it->second.addr;
    t.aios_path = e.aios_path;
    t.mount = e.mount;
    t.bavail = e.bavail;
    map.targets.push_back(std::move(t));
  }

  std::sort(map.targets.begin(), map.targets.end(),
            [](const StorageTarget& a, const StorageTarget& b) {
              if (a.node_id != b.node_id) return a.node_id < b.node_id;
              return a.aios_path < b.aios_path;
            });

  std::ostringstream canon;
  canon << "replica_count=" << map.replica_count << '\n';
  for (const auto& t : map.targets) {
    canon << t.node_id << '\t' << t.addr << '\t' << t.aios_path << '\n';
  }
  map.epoch = hash_canonical(canon.str());
  return map;
}

}  // namespace aios
