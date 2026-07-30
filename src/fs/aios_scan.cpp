#include "fs/aios_scan.hpp"

#include "fs/mounts.hpp"
#include "util/log.hpp"
#include "util/uid.hpp"

#include <yaml-cpp/yaml.h>

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace aios {
namespace {

std::string join_path(const std::string& a, const std::string& b) {
  if (b.empty() || b == ".") return a;
  if (!b.empty() && b[0] == '/') return b;
  fs::path p = fs::path(a) / b;
  return p.lexically_normal().string();
}

bool under_mount(const std::string& mount, const std::string& path) {
  fs::path m = fs::path(mount).lexically_normal();
  fs::path p = fs::path(path).lexically_normal();
  auto mstr = m.string();
  auto pstr = p.string();
  if (mstr == "/") return true;
  if (pstr == mstr) return true;
  if (pstr.size() > mstr.size() && pstr.compare(0, mstr.size(), mstr) == 0 &&
      pstr[mstr.size()] == '/') {
    return true;
  }
  return false;
}

}  // namespace

std::vector<std::string> parse_aios_targets(const std::string& yaml_text,
                                            const std::string& mount_root,
                                            std::string& err) {
  err.clear();
  std::string trimmed = yaml_text;
  // Treat whitespace-only as empty config.
  if (trimmed.find_first_not_of(" \t\r\n") == std::string::npos) {
    return {mount_root};
  }
  try {
    YAML::Node root = YAML::Load(yaml_text);
    if (!root || root.IsNull()) {
      return {mount_root};
    }
    if (root.IsMap() && !root["targets"]) {
      return {mount_root};
    }
    if (root.IsMap() && root["targets"]) {
      std::vector<std::string> out;
      if (root["targets"].IsSequence()) {
        for (const auto& t : root["targets"]) {
          const std::string rel = t.as<std::string>();
          const std::string abs = join_path(mount_root, rel);
          if (!under_mount(mount_root, abs)) {
            err = "target escapes mount: " + abs;
            return {};
          }
          out.push_back(abs);
        }
      } else if (root["targets"].IsScalar()) {
        const std::string abs = join_path(mount_root, root["targets"].as<std::string>());
        if (!under_mount(mount_root, abs)) {
          err = "target escapes mount: " + abs;
          return {};
        }
        out.push_back(abs);
      }
      if (out.empty()) return {mount_root};
      return out;
    }
    // Non-map unexpected content: treat as whole partition.
    return {mount_root};
  } catch (const std::exception& e) {
    err = e.what();
    return {};
  }
}

AiosTarget prepare_target(const std::string& mount, const std::string& target_path) {
  AiosTarget t;
  t.mount = mount;
  t.target_path = target_path;
  t.aios_path = (fs::path(target_path) / "aios").string();

  std::error_code ec;
  if (!fs::exists(target_path, ec)) {
    t.error = "target path missing: " + target_path;
    return t;
  }
  if (!fs::is_directory(target_path, ec)) {
    t.error = "target is not a directory: " + target_path;
    return t;
  }

  if (!fs::exists(t.aios_path, ec)) {
    if (!fs::create_directory(t.aios_path, ec)) {
      t.error = "mkdir aios failed: " + ec.message();
      return t;
    }
    AIOS_LOG_INFO("created ", t.aios_path);
  }
  if (!fs::is_directory(t.aios_path, ec)) {
    t.error = "aios path is not a directory: " + t.aios_path;
    return t;
  }

  struct stat st {};
  if (::stat(t.aios_path.c_str(), &st) != 0) {
    t.error = std::string("stat aios failed: ") + std::strerror(errno);
    return t;
  }
  const uid_t uid = effective_uid();
  const gid_t gid = effective_gid();
  if (st.st_uid != uid || st.st_gid != gid) {
    std::ostringstream oss;
    oss << "uid/gid mismatch on " << t.aios_path << ": dir=" << st.st_uid << "/"
        << st.st_gid << " euid/egid=" << uid << "/" << gid;
    t.error = oss.str();
    AIOS_LOG_ERROR(t.error);
    return t;
  }

  struct statvfs vfs {};
  if (::statvfs(target_path.c_str(), &vfs) != 0) {
    t.error = std::string("statvfs failed: ") + std::strerror(errno);
    return t;
  }
  t.bsize = static_cast<std::uint64_t>(vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize);
  t.blocks = static_cast<std::uint64_t>(vfs.f_blocks);
  t.bfree = static_cast<std::uint64_t>(vfs.f_bfree);
  t.bavail = static_cast<std::uint64_t>(vfs.f_bavail);
  t.files = static_cast<std::uint64_t>(vfs.f_files);
  t.ffree = static_cast<std::uint64_t>(vfs.f_ffree);
  t.usable = true;
  return t;
}

std::vector<AiosTarget> scan_aios_filesystems() {
  std::vector<AiosTarget> out;
  for (const auto& m : list_mounts()) {
    const fs::path marker = fs::path(m.path) / ".aios";
    std::error_code ec;
    if (!fs::is_regular_file(marker, ec)) continue;

    std::ifstream in(marker);
    if (!in) {
      AIOS_LOG_WARN("cannot read ", marker.string());
      continue;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string err;
    auto targets = parse_aios_targets(ss.str(), m.path, err);
    if (!err.empty()) {
      AIOS_LOG_ERROR(".aios parse on ", m.path, ": ", err);
      continue;
    }
    for (const auto& tp : targets) {
      out.push_back(prepare_target(m.path, tp));
    }
  }
  return out;
}

}  // namespace aios
