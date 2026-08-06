#include "fs/aios_scan.hpp"

#include "fs/mounts.hpp"
#include "util/log.hpp"
#include "util/uid.hpp"

#include <yaml-cpp/yaml.h>

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cctype>
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

std::string lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool valid_storage_class(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (char c : s) {
    if (std::islower(static_cast<unsigned char>(c)) ||
        std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
      continue;
    }
    return false;
  }
  return true;
}

}  // namespace

bool parse_aios_marker(const std::string& yaml_text, const std::string& mount_root,
                       AiosMarker& out, std::string& err) {
  err.clear();
  out = AiosMarker{};
  std::string trimmed = yaml_text;
  if (trimmed.find_first_not_of(" \t\r\n") == std::string::npos) {
    err = "storage_class is required in .aios";
    return false;
  }
  try {
    YAML::Node root = YAML::Load(yaml_text);
    if (!root || !root.IsMap()) {
      err = ".aios must be a YAML mapping with storage_class";
      return false;
    }
    if (!root["storage_class"]) {
      err = "storage_class is required in .aios";
      return false;
    }
    out.storage_class = lower_copy(root["storage_class"].as<std::string>());
    if (!valid_storage_class(out.storage_class)) {
      err = "storage_class must match [a-z0-9_-]+";
      return false;
    }
    if (root["weight"]) {
      out.weight = root["weight"].as<int>();
      if (out.weight < 1) {
        err = "weight must be >= 1";
        return false;
      }
    }
    if (root["state"]) {
      const auto st = lower_copy(root["state"].as<std::string>());
      if (!valid_lifecycle_state_string(st)) {
        err = "state must be up, drain, or off";
        return false;
      }
      out.state = lifecycle_state_from_string(st);
    }
    if (!root["targets"]) {
      out.target_paths = {mount_root};
      return true;
    }
    if (root["targets"].IsSequence()) {
      for (const auto& t : root["targets"]) {
        const std::string rel = t.as<std::string>();
        const std::string abs = join_path(mount_root, rel);
        if (!under_mount(mount_root, abs)) {
          err = "target escapes mount: " + abs;
          return false;
        }
        out.target_paths.push_back(abs);
      }
    } else if (root["targets"].IsScalar()) {
      const std::string abs = join_path(mount_root, root["targets"].as<std::string>());
      if (!under_mount(mount_root, abs)) {
        err = "target escapes mount: " + abs;
        return false;
      }
      out.target_paths.push_back(abs);
    } else {
      err = "targets must be a string or sequence";
      return false;
    }
    if (out.target_paths.empty()) out.target_paths = {mount_root};
    return true;
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

AiosTarget prepare_target(const std::string& mount, const std::string& target_path,
                          const std::string& storage_class, int weight, LifecycleState state) {
  AiosTarget t;
  t.mount = mount;
  t.target_path = target_path;
  t.aios_path = (fs::path(target_path) / "aios").string();
  t.storage_class = storage_class;
  t.weight = weight > 0 ? weight : 1;
  t.state = state;

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
    AiosMarker parsed;
    std::string err;
    if (!parse_aios_marker(ss.str(), m.path, parsed, err)) {
      AIOS_LOG_ERROR(".aios parse on ", m.path, ": ", err);
      continue;
    }
    for (const auto& tp : parsed.target_paths) {
      out.push_back(
          prepare_target(m.path, tp, parsed.storage_class, parsed.weight, parsed.state));
    }
  }
  return out;
}

bool update_aios_marker_file(const std::string& marker_path,
                             const std::optional<std::string>& state,
                             const std::optional<int>& weight, std::string& err) {
  err.clear();
  if (!state && !weight) {
    err = "state or weight required";
    return false;
  }
  if (state && !valid_lifecycle_state_string(lower_copy(*state))) {
    err = "state must be up, drain, or off";
    return false;
  }
  if (weight && *weight < 1) {
    err = "weight must be >= 1";
    return false;
  }
  std::ifstream in(marker_path);
  if (!in) {
    err = "cannot read " + marker_path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  try {
    YAML::Node root = YAML::Load(ss.str());
    if (!root || !root.IsMap()) {
      err = ".aios must be a YAML mapping";
      return false;
    }
    if (state) root["state"] = lower_copy(*state);
    if (weight) root["weight"] = *weight;
    std::ofstream out(marker_path, std::ios::trunc);
    if (!out) {
      err = "cannot write " + marker_path;
      return false;
    }
    out << root;
    out << "\n";
    return true;
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

}  // namespace aios
