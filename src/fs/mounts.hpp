#pragma once

#include <string>
#include <vector>

namespace aios {

struct MountPoint {
  std::string path;
  std::string fstype;
};

// Enumerate mounted filesystems; skips common virtual types.
std::vector<MountPoint> list_mounts();

bool is_virtual_fstype(const std::string& fstype);

}  // namespace aios
