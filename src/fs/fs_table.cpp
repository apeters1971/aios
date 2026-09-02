#include "fs/fs_table.hpp"

#include "util/log.hpp"

#include <cstdint>

namespace aios {

void FsTable::set_local(const std::string& node_id, const std::vector<AiosTarget>& targets,
                        LifecycleState node_state, const std::string& node_rack) {
  std::lock_guard lock(mu_);
  local_id_ = node_id;
  // Drop previous local entries.
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.node_id == node_id) {
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  const std::string default_rack = node_rack.empty() ? node_id : node_rack;
  const auto ts = now_ms();
  for (const auto& t : targets) {
    const LifecycleState eff = worse_lifecycle(node_state, t.state);
    // An off or unusable target is advertised as a tombstone rather than dropped.
    // Merge is upsert-only, so simply omitting the entry would leave every peer
    // holding the last "up" copy forever and placing on a target we no longer serve.
    // ClusterMap::build skips both !usable and Off, so these never enter the map.
    const bool tombstone = !t.usable || eff == LifecycleState::Off;
    if (tombstone && t.aios_path.empty()) continue;  // nothing peers could be holding
    FsEntry e;
    e.node_id = node_id;
    e.mount = t.mount;
    e.target_path = t.target_path;
    e.aios_path = t.aios_path;
    e.storage_class = t.storage_class;
    e.rack = (t.rack_explicit && !t.rack.empty()) ? t.rack : default_rack;
    e.weight = t.weight > 0 ? t.weight : 1;
    e.state = eff;
    e.bsize = t.bsize;
    e.blocks = t.blocks;
    e.bfree = t.bfree;
    e.bavail = t.bavail;
    e.files = t.files;
    e.ffree = t.ffree;
    e.usable = !tombstone;
    e.updated_ms = ts;
    entries_[key_of(e)] = std::move(e);
  }
}

void FsTable::merge(const std::vector<FsEntry>& remote) {
  std::lock_guard lock(mu_);
  for (const auto& r : remote) {
    if (r.node_id.empty() || r.aios_path.empty()) continue;
    if (r.node_id == local_id_) continue;  // local scan is authoritative
    const auto k = key_of(r);
    auto it = entries_.find(k);
    if (it == entries_.end() || r.updated_ms >= it->second.updated_ms) {
      entries_[k] = r;
      if (entries_[k].rack.empty()) entries_[k].rack = r.node_id;
    }
  }
}

std::vector<FsEntry> FsTable::snapshot() const {
  std::lock_guard lock(mu_);
  std::vector<FsEntry> out;
  out.reserve(entries_.size());
  for (const auto& [_, e] : entries_) out.push_back(e);
  return out;
}

nlohmann::json FsTable::to_json() const {
  nlohmann::json entries = nlohmann::json::array();
  for (const auto& e : snapshot()) {
    entries.push_back({
        {"node_id", e.node_id},
        {"mount", e.mount},
        {"target_path", e.target_path},
        {"aios_path", e.aios_path},
        {"storage_class", e.storage_class},
        {"rack", e.rack},
        {"weight", e.weight},
        {"state", lifecycle_state_name(e.state)},
        {"bsize", e.bsize},
        {"blocks", e.blocks},
        {"bfree", e.bfree},
        {"bavail", e.bavail},
        {"files", e.files},
        {"ffree", e.ffree},
        {"usable", e.usable},
        {"updated_ms", e.updated_ms},
    });
  }
  return {{"entries", entries}};
}

namespace {

// Gossip payloads come from peers; a wrong-typed field skips the entry rather
// than throwing out of the receive path. Missing keys keep the default.
bool jget(const nlohmann::json& o, const char* key, std::string& out) {
  auto it = o.find(key);
  if (it == o.end()) return true;
  if (!it->is_string()) return false;
  out = it->get<std::string>();
  return true;
}
bool jget(const nlohmann::json& o, const char* key, std::uint64_t& out) {
  auto it = o.find(key);
  if (it == o.end()) return true;
  if (it->is_number_unsigned()) {
    out = it->get<std::uint64_t>();
    return true;
  }
  if (it->is_number_integer() && it->get<std::int64_t>() >= 0) {
    out = static_cast<std::uint64_t>(it->get<std::int64_t>());
    return true;
  }
  return false;
}
bool jget(const nlohmann::json& o, const char* key, std::int64_t& out) {
  auto it = o.find(key);
  if (it == o.end()) return true;
  if (!it->is_number_integer()) return false;
  out = it->get<std::int64_t>();
  return true;
}
bool jget(const nlohmann::json& o, const char* key, int& out) {
  auto it = o.find(key);
  if (it == o.end()) return true;
  if (!it->is_number_integer()) return false;
  const auto v = it->get<std::int64_t>();
  if (v < INT32_MIN || v > INT32_MAX) return false;
  out = static_cast<int>(v);
  return true;
}
bool jget(const nlohmann::json& o, const char* key, bool& out) {
  auto it = o.find(key);
  if (it == o.end()) return true;
  if (!it->is_boolean()) return false;
  out = it->get<bool>();
  return true;
}

}  // namespace

std::vector<FsEntry> FsTable::from_json(const nlohmann::json& j) {
  std::vector<FsEntry> out;
  const nlohmann::json* arr = &j;
  if (j.is_object()) {
    auto it = j.find("entries");
    if (it == j.end()) return out;
    arr = &*it;
  }
  if (!arr->is_array()) return out;
  for (const auto& x : *arr) {
    if (!x.is_object()) continue;
    FsEntry e;
    std::string state = "up";
    const bool ok = jget(x, "node_id", e.node_id) && jget(x, "mount", e.mount) &&
                    jget(x, "target_path", e.target_path) &&
                    jget(x, "aios_path", e.aios_path) &&
                    jget(x, "storage_class", e.storage_class) && jget(x, "rack", e.rack) &&
                    jget(x, "weight", e.weight) && jget(x, "state", state) &&
                    jget(x, "bsize", e.bsize) && jget(x, "blocks", e.blocks) &&
                    jget(x, "bfree", e.bfree) && jget(x, "bavail", e.bavail) &&
                    jget(x, "files", e.files) && jget(x, "ffree", e.ffree) &&
                    jget(x, "usable", e.usable) && jget(x, "updated_ms", e.updated_ms);
    if (!ok) {
      AIOS_LOG_WARN("fs_table: skipping malformed gossip entry");
      continue;
    }
    if (e.rack.empty() && !e.node_id.empty()) e.rack = e.node_id;
    e.state = lifecycle_state_from_string(state);
    out.push_back(std::move(e));
  }
  return out;
}

}  // namespace aios
