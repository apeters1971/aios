#include "cluster/place.hpp"

#include <openssl/evp.h>

#include <unordered_set>

namespace aios {
namespace {

std::uint64_t sha256_u64(const std::string& s) {
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

Placement place(const std::string& oid, const ClusterMap& map, int n) {
  Placement p;
  p.epoch = map.epoch;
  if (map.targets.empty() || n < 1) return p;
  if (static_cast<std::size_t>(n) > map.targets.size()) return p;

  const std::size_t ring = map.targets.size();
  const std::size_t want = static_cast<std::size_t>(n);
  const std::size_t start = static_cast<std::size_t>(sha256_u64(oid) % ring);

  std::unordered_set<std::string> used_keys;
  std::unordered_set<std::string> used_nodes;

  auto try_add = [&](const StorageTarget& t, bool require_new_node) -> bool {
    if (used_keys.count(target_key(t))) return false;
    if (require_new_node && used_nodes.count(t.node_id)) return false;
    p.acting_set.push_back(t);
    used_keys.insert(target_key(t));
    used_nodes.insert(t.node_id);
    return true;
  };

  // Pass 1: distinct nodes.
  for (std::size_t i = 0; i < ring && p.acting_set.size() < want; ++i) {
    try_add(map.targets[(start + i) % ring], /*require_new_node=*/true);
  }
  // Pass 2: fill remaining with any unused targets (same-node mounts ok).
  for (std::size_t i = 0; i < ring && p.acting_set.size() < want; ++i) {
    try_add(map.targets[(start + i) % ring], /*require_new_node=*/false);
  }
  return p;
}

Placement place(const std::string& oid, const ClusterMap& map) {
  return place(oid, map, map.replica_count);
}

bool is_primary_for(const std::string& oid, const ClusterMap& map,
                    const std::string& node_id, const std::string& aios_path) {
  const auto p = place(oid, map);
  if (p.acting_set.empty()) return false;
  return p.acting_set[0].node_id == node_id && p.acting_set[0].aios_path == aios_path;
}

bool in_acting_set(const std::string& oid, const ClusterMap& map,
                   const std::string& node_id, const std::string& aios_path) {
  const auto p = place(oid, map);
  for (const auto& t : p.acting_set) {
    if (t.node_id == node_id && t.aios_path == aios_path) return true;
  }
  return false;
}

}  // namespace aios
