#pragma once

#include "cluster/lifecycle.hpp"
#include "fs/aios_scan.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace aios {

struct FsEntry {
  std::string node_id;
  std::string mount;
  std::string target_path;
  std::string aios_path;
  std::string storage_class;
  std::string rack{};  // effective failure domain
  int weight{1};
  LifecycleState state{LifecycleState::Up};  // effective (node × target)
  std::uint64_t bsize{0};
  std::uint64_t blocks{0};
  std::uint64_t bfree{0};
  std::uint64_t bavail{0};
  std::uint64_t files{0};
  std::uint64_t ffree{0};
  bool usable{false};
  std::int64_t updated_ms{0};
};

class FsTable {
 public:
  // Replace all local-origin entries with scan results.
  // node_state is folded into each entry's effective state; Off targets are omitted.
  // node_rack is the node default (empty → node_id); .aios rack_explicit overrides.
  void set_local(const std::string& node_id, const std::vector<AiosTarget>& targets,
                 LifecycleState node_state = LifecycleState::Up,
                 const std::string& node_rack = {});

  void merge(const std::vector<FsEntry>& remote);

  std::vector<FsEntry> snapshot() const;

  nlohmann::json to_json() const;
  static std::vector<FsEntry> from_json(const nlohmann::json& j);

 private:
  static std::string key_of(const FsEntry& e) {
    return e.node_id + "\n" + e.aios_path;
  }

  mutable std::mutex mu_;
  std::string local_id_;
  std::unordered_map<std::string, FsEntry> entries_;
};

}  // namespace aios
