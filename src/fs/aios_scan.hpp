#pragma once

#include "cluster/lifecycle.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aios {

struct AiosTarget {
  std::string mount;
  std::string target_path;  // directory that owns aios/
  std::string aios_path;    // target_path + "/aios"
  std::string storage_class;
  int weight{1};
  bool weight_explicit{false};  // true if .aios set weight:
  LifecycleState state{LifecycleState::Up};
  bool usable{false};
  std::string error;

  std::uint64_t bsize{0};
  std::uint64_t blocks{0};
  std::uint64_t bfree{0};
  std::uint64_t bavail{0};
  std::uint64_t files{0};
  std::uint64_t ffree{0};
};

// Parsed .aios marker (storage_class required).
struct AiosMarker {
  std::string storage_class;
  int weight{1};
  bool weight_specified{false};
  LifecycleState state{LifecycleState::Up};
  std::vector<std::string> target_paths;  // absolute paths under mount
};

// Parse .aios YAML. Requires storage_class. Missing/empty targets => mount_root.
// Omitting weight leaves weight_specified=false (caller derives from capacity).
bool parse_aios_marker(const std::string& yaml_text, const std::string& mount_root,
                       AiosMarker& out, std::string& err);

// Resolve paths under mount; create aios/; check uid/gid; fill statvfs.
// If weight is nullopt, weight is derived from total filesystem capacity (TiB).
AiosTarget prepare_target(const std::string& mount, const std::string& target_path,
                          const std::string& storage_class,
                          std::optional<int> weight = std::nullopt,
                          LifecycleState state = LifecycleState::Up);

// Update state and/or weight in an existing .aios file (rewrites YAML keys).
bool update_aios_marker_file(const std::string& marker_path, const std::optional<std::string>& state,
                             const std::optional<int>& weight, std::string& err);

// Scan all mounts for .aios markers and prepare targets.
std::vector<AiosTarget> scan_aios_filesystems();

}  // namespace aios
