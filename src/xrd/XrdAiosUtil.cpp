#include "xrd/XrdAiosUtil.hpp"
#include "xrd/aios_xrd_cred.h"

#include "XrdOuc/XrdOucEnv.hh"
#include "XrdSec/XrdSecEntity.hh"
#include "XrdSys/XrdSysError.hh"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace aios {
namespace xrd {
namespace {

void set_kv(MountConfig* cfg, const std::string& key, const std::string& val) {
  if (key == "endpoint" || key == "aios.endpoint")
    cfg->endpoint = val;
  else if (key == "cluster_key" || key == "aios.cluster_key")
    cfg->cluster_key = val;
  else if (key == "volume" || key == "aios.volume")
    cfg->volume = val.empty() ? "default" : val;
  else if (key == "stripe_unit" || key == "aios.stripe_unit")
    cfg->stripe_unit = static_cast<uint64_t>(std::stoull(val));
  else if (key == "stripe_width" || key == "aios.stripe_width")
    cfg->stripe_width = static_cast<uint32_t>(std::stoul(val));
}

void parse_parms(const char* parms, MountConfig* cfg) {
  if (!parms || !*parms) return;
  std::istringstream iss(parms);
  std::string tok;
  while (iss >> tok) {
    const auto eq = tok.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    set_kv(cfg, tok.substr(0, eq), tok.substr(eq + 1));
  }
}

void parse_config_file(const char* config_fn, MountConfig* cfg) {
  if (!config_fn || !*config_fn) return;
  std::ifstream in(config_fn);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream iss(line);
    std::string word;
    if (!(iss >> word)) continue;
    if (word.rfind("aios.", 0) != 0) continue;
    std::string rest;
    std::getline(iss >> std::ws, rest);
    while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ' || rest.back() == '\t'))
      rest.pop_back();
    set_kv(cfg, word, rest);
  }
}

std::vector<std::string> split_components(const char* path) {
  std::vector<std::string> out;
  if (!path) return out;
  const char* p = path;
  while (*p == '/') ++p;
  while (*p) {
    std::string c;
    while (*p && *p != '/') c.push_back(*p++);
    while (*p == '/') ++p;
    if (!c.empty()) out.push_back(std::move(c));
  }
  return out;
}

}  // namespace

bool load_config(const char* config_fn, const char* parms, MountConfig* out, XrdSysError* log) {
  if (!out) return false;
  *out = MountConfig{};
  if (const char* ep = std::getenv("AIOS_ENDPOINT")) out->endpoint = ep;
  if (const char* ck = std::getenv("AIOS_CLUSTER_KEY")) out->cluster_key = ck;
  parse_config_file(config_fn, out);
  parse_parms(parms, out);
  if (out->endpoint.empty() || out->cluster_key.empty()) {
    if (log) {
      log->Emsg("AiosConfig", "aios.endpoint and aios.cluster_key (or AIOS_* env / osslib parms) required");
    }
    return false;
  }
  return true;
}

int lookup_path(aios_posix_fs* fs, const char* path, aios_posix_stat* st_out) {
  if (!fs || !path || !st_out) return -EINVAL;
  if (path[0] != '/') return -EINVAL;
  if (std::strcmp(path, "/") == 0) return aios_posix_getattr(fs, 1, st_out);

  uint64_t ino = 1;
  aios_posix_stat cur{};
  for (const auto& component : split_components(path)) {
    int rc = aios_posix_lookup(fs, ino, component.c_str(), &cur);
    if (rc) return rc;
    ino = cur.ino;
    *st_out = cur;
  }
  return 0;
}

int resolve_parent(aios_posix_fs* fs, const char* path, uint64_t* parent_out, std::string* name_out) {
  if (!fs || !path || !parent_out || !name_out) return -EINVAL;
  std::string p = path;
  while (!p.empty() && p.back() == '/') p.pop_back();
  if (p.empty() || p == "/") return -EINVAL;
  const auto slash = p.rfind('/');
  const std::string parent_path = (slash == 0) ? "/" : p.substr(0, slash);
  *name_out = p.substr(slash + 1);
  if (name_out->empty()) return -EINVAL;
  aios_posix_stat st{};
  int rc = lookup_path(fs, parent_path.c_str(), &st);
  if (rc) return rc;
  *parent_out = st.ino;
  return 0;
}

void fill_stat(const aios_posix_stat& st, struct stat* out) {
  std::memset(out, 0, sizeof(*out));
  out->st_ino = static_cast<ino_t>(st.ino);
  out->st_mode = static_cast<mode_t>(st.mode);
  out->st_nlink = static_cast<nlink_t>(st.nlink);
  out->st_uid = static_cast<uid_t>(st.uid);
  out->st_gid = static_cast<gid_t>(st.gid);
  out->st_size = static_cast<off_t>(st.size);
#if defined(__APPLE__)
  out->st_atimespec.tv_sec = static_cast<time_t>(st.atime_ns / 1000000000ull);
  out->st_atimespec.tv_nsec = static_cast<long>(st.atime_ns % 1000000000ull);
  out->st_mtimespec.tv_sec = static_cast<time_t>(st.mtime_ns / 1000000000ull);
  out->st_mtimespec.tv_nsec = static_cast<long>(st.mtime_ns % 1000000000ull);
  out->st_ctimespec.tv_sec = static_cast<time_t>(st.ctime_ns / 1000000000ull);
  out->st_ctimespec.tv_nsec = static_cast<long>(st.ctime_ns % 1000000000ull);
#else
  out->st_atim.tv_sec = static_cast<time_t>(st.atime_ns / 1000000000ull);
  out->st_atim.tv_nsec = static_cast<long>(st.atime_ns % 1000000000ull);
  out->st_mtim.tv_sec = static_cast<time_t>(st.mtime_ns / 1000000000ull);
  out->st_mtim.tv_nsec = static_cast<long>(st.mtime_ns % 1000000000ull);
  out->st_ctim.tv_sec = static_cast<time_t>(st.ctime_ns / 1000000000ull);
  out->st_ctim.tv_nsec = static_cast<long>(st.ctime_ns % 1000000000ull);
#endif
  out->st_blksize = 4096;
  out->st_blocks = static_cast<blkcnt_t>((st.size + 511) / 512);
}

int apply_caller(aios_posix_fs* fs, XrdOucEnv* env) {
  if (!fs) return -EIO;
  if (!env) {
    aios_posix_clear_caller(fs);
    return 0;
  }
  return apply_caller(fs, *env);
}

int apply_caller(aios_posix_fs* fs, XrdOucEnv& env) {
  if (!fs) return -EIO;
  const XrdSecEntity* se = env.secEnv();
  if (!se || !se->name || !*se->name) return -EACCES;
  uint32_t uid = 0, gid = 0;
  if (int rc = aios_xrd_map_name(se->name, &uid, &gid)) return rc;
  aios_posix_set_caller(fs, uid, gid);
  return 0;
}

int mkdir_p(aios_posix_fs* fs, const char* path, mode_t mode) {
  if (!fs || !path || path[0] != '/') return -EINVAL;
  if (std::strcmp(path, "/") == 0) return 0;
  uint64_t cur = 1;
  for (const auto& component : split_components(path)) {
    aios_posix_stat st{};
    int rc = aios_posix_lookup(fs, cur, component.c_str(), &st);
    if (rc == 0) {
      if (!S_ISDIR(st.mode)) return -ENOTDIR;
      cur = st.ino;
      continue;
    }
    if (rc != -ENOENT) return rc;
    rc = aios_posix_mkdir(fs, cur, component.c_str(), static_cast<uint32_t>(mode), &st);
    if (rc) return rc;
    cur = st.ino;
  }
  return 0;
}

}  // namespace xrd
}  // namespace aios
