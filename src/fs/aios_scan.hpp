#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aios {

struct AiosTarget {
  std::string mount;
  std::string target_path;  // directory that owns aios/
  std::string aios_path;    // target_path + "/aios"
  bool usable{false};
  std::string error;

  std::uint64_t bsize{0};
  std::uint64_t blocks{0};
  std::uint64_t bfree{0};
  std::uint64_t bavail{0};
  std::uint64_t files{0};
  std::uint64_t ffree{0};
};

// Parse .aios YAML content. Empty / missing targets => single target at mount_root.
std::vector<std::string> parse_aios_targets(const std::string& yaml_text,
                                            const std::string& mount_root,
                                            std::string& err);

// Resolve paths under mount; create aios/; check uid/gid; fill statvfs.
AiosTarget prepare_target(const std::string& mount, const std::string& target_path);

// Scan all mounts for .aios markers and prepare targets.
std::vector<AiosTarget> scan_aios_filesystems();

}  // namespace aios
