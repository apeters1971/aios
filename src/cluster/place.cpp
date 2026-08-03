#include "cluster/place.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

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

struct VNode {
  std::uint64_t hash{0};
  std::size_t target_index{0};
};

int vnode_count_for(const StorageTarget& t, const PlacementConfig& pc) {
  const int w = t.weight > 0 ? t.weight : 1;
  long long v = static_cast<long long>(w) * pc.vnodes_per_target;
  if (v < pc.min_vnodes) v = pc.min_vnodes;
  if (v > pc.max_vnodes) v = pc.max_vnodes;
  return static_cast<int>(v);
}

std::vector<VNode> build_ring(const std::vector<StorageTarget>& targets,
                              const PlacementConfig& pc) {
  std::vector<VNode> ring;
  ring.reserve(targets.size() * static_cast<std::size_t>(std::max(1, pc.vnodes_per_target)));
  for (std::size_t ti = 0; ti < targets.size(); ++ti) {
    const int vn = vnode_count_for(targets[ti], pc);
    const std::string base = target_key(targets[ti]);
    for (int i = 0; i < vn; ++i) {
      VNode v;
      v.hash = sha256_u64(base + ":vnode:" + std::to_string(i));
      v.target_index = ti;
      ring.push_back(v);
    }
  }
  std::sort(ring.begin(), ring.end(),
            [](const VNode& a, const VNode& b) {
              if (a.hash != b.hash) return a.hash < b.hash;
              return a.target_index < b.target_index;
            });
  return ring;
}

}  // namespace

Placement place(const std::string& oid, const ClusterMap& map, int n,
                const std::string& storage_class) {
  Placement p;
  p.epoch = map.epoch;
  p.storage_class = storage_class;
  if (map.targets.empty() || n < 1 || storage_class.empty()) return p;

  const auto pool = map.targets_for_class(storage_class);
  if (static_cast<std::size_t>(n) > pool.size()) return p;

  const auto ring = build_ring(pool, map.placement);
  if (ring.empty()) return p;

  const std::uint64_t oid_hash = sha256_u64(oid);
  auto it = std::lower_bound(ring.begin(), ring.end(), oid_hash,
                             [](const VNode& v, std::uint64_t h) { return v.hash < h; });
  std::size_t start = static_cast<std::size_t>(it - ring.begin());
  if (start >= ring.size()) start = 0;

  const std::size_t want = static_cast<std::size_t>(n);
  std::unordered_set<std::string> used_keys;
  std::unordered_set<std::string> used_nodes;

  auto try_add = [&](std::size_t ti, bool require_new_node) -> bool {
    const auto& t = pool[ti];
    if (used_keys.count(target_key(t))) return false;
    if (require_new_node && used_nodes.count(t.node_id)) return false;
    p.acting_set.push_back(t);
    used_keys.insert(target_key(t));
    used_nodes.insert(t.node_id);
    return true;
  };

  // Pass 1: distinct nodes, walking the vnode ring clockwise.
  for (std::size_t i = 0; i < ring.size() && p.acting_set.size() < want; ++i) {
    try_add(ring[(start + i) % ring.size()].target_index, /*require_new_node=*/true);
  }
  // Pass 2: fill remaining with any unused targets in the pool.
  for (std::size_t i = 0; i < ring.size() && p.acting_set.size() < want; ++i) {
    try_add(ring[(start + i) % ring.size()].target_index, /*require_new_node=*/false);
  }
  return p;
}

Placement place(const std::string& oid, const ClusterMap& map,
                const std::string& storage_class) {
  return place(oid, map, map.replica_count, storage_class);
}

bool is_primary_for(const std::string& oid, const ClusterMap& map,
                    const std::string& storage_class, const std::string& node_id,
                    const std::string& aios_path) {
  const auto p = place(oid, map, storage_class);
  if (p.acting_set.empty()) return false;
  return p.acting_set[0].node_id == node_id && p.acting_set[0].aios_path == aios_path;
}

bool in_acting_set(const std::string& oid, const ClusterMap& map,
                   const std::string& storage_class, const std::string& node_id,
                   const std::string& aios_path) {
  const auto p = place(oid, map, storage_class);
  for (const auto& t : p.acting_set) {
    if (t.node_id == node_id && t.aios_path == aios_path) return true;
  }
  return false;
}

}  // namespace aios
