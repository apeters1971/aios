#include "fs/fs_table.hpp"

#include "util/log.hpp"

namespace aios {

void FsTable::set_local(const std::string& node_id, const std::vector<AiosTarget>& targets,
                        LifecycleState node_state) {
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
  const auto ts = now_ms();
  for (const auto& t : targets) {
    if (!t.usable) continue;
    const LifecycleState eff = worse_lifecycle(node_state, t.state);
    if (eff == LifecycleState::Off) continue;  // not advertised
    FsEntry e;
    e.node_id = node_id;
    e.mount = t.mount;
    e.target_path = t.target_path;
    e.aios_path = t.aios_path;
    e.storage_class = t.storage_class;
    e.weight = t.weight > 0 ? t.weight : 1;
    e.state = eff;
    e.bsize = t.bsize;
    e.blocks = t.blocks;
    e.bfree = t.bfree;
    e.bavail = t.bavail;
    e.files = t.files;
    e.ffree = t.ffree;
    e.usable = true;
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

std::vector<FsEntry> FsTable::from_json(const nlohmann::json& j) {
  std::vector<FsEntry> out;
  const auto& arr = j.contains("entries") ? j.at("entries") : j;
  if (!arr.is_array()) return out;
  for (const auto& x : arr) {
    FsEntry e;
    e.node_id = x.value("node_id", "");
    e.mount = x.value("mount", "");
    e.target_path = x.value("target_path", "");
    e.aios_path = x.value("aios_path", "");
    e.storage_class = x.value("storage_class", "");
    e.weight = x.value("weight", 1);
    e.state = lifecycle_state_from_string(x.value("state", "up"));
    e.bsize = x.value("bsize", std::uint64_t{0});
    e.blocks = x.value("blocks", std::uint64_t{0});
    e.bfree = x.value("bfree", std::uint64_t{0});
    e.bavail = x.value("bavail", std::uint64_t{0});
    e.files = x.value("files", std::uint64_t{0});
    e.ffree = x.value("ffree", std::uint64_t{0});
    e.usable = x.value("usable", false);
    e.updated_ms = x.value("updated_ms", std::int64_t{0});
    out.push_back(std::move(e));
  }
  return out;
}

}  // namespace aios
