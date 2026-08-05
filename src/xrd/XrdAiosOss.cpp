#include "xrd/XrdAiosOss.hpp"
#include "xrd/XrdAiosDF.hpp"
#include "xrd/XrdAiosUtil.hpp"

#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysLogger.hh"
#include "XrdVersion.hh"

#include <sys/stat.h>

namespace aios {
namespace xrd {

XrdAiosOss::XrdAiosOss() = default;

XrdAiosOss::~XrdAiosOss() {
  if (fs_) {
    aios_posix_unmount(fs_);
    fs_ = nullptr;
  }
}

void XrdAiosOss::set_boot(const char* config_fn, const char* parms) {
  config_fn_ = config_fn ? config_fn : "";
  parms_ = parms ? parms : "";
}

XrdOssDF* XrdAiosOss::newDir(const char* tident) { return new XrdAiosDir(this, tident); }

XrdOssDF* XrdAiosOss::newFile(const char* tident) { return new XrdAiosFile(this, tident); }

int XrdAiosOss::Init(XrdSysLogger* lp, const char* cfn) { return Init(lp, cfn, nullptr); }

int XrdAiosOss::Init(XrdSysLogger* lp, const char* cfn, XrdOucEnv* /*envP*/) {
  log_ = std::make_unique<XrdSysError>(lp, "aios");
  if (config_fn_.empty() && cfn) config_fn_ = cfn;

  if (!load_config(config_fn_.c_str(), parms_.c_str(), &cfg_, log_.get())) return -EINVAL;

  endpoint_ = cfg_.endpoint;
  cluster_key_ = cfg_.cluster_key;
  volume_ = cfg_.volume;

  aios_posix_config pcfg{};
  pcfg.endpoint = endpoint_.c_str();
  pcfg.cluster_key = cluster_key_.c_str();
  pcfg.volume = volume_.c_str();
  pcfg.app_label = "xrd";
  pcfg.stripe_unit = cfg_.stripe_unit;
  pcfg.stripe_width = cfg_.stripe_width;
  pcfg.uid = 0;
  pcfg.gid = 0;

  int err = 0;
  fs_ = aios_posix_mount(&pcfg, &err);
  if (!fs_) {
    log_->Emsg("Init", "aios_posix_mount failed");
    return err ? (err > 0 ? -err : err) : -EIO;
  }
  log_->Emsg("Init", "mounted AIOS volume", volume_.c_str());
  return XrdOssOK;
}

int XrdAiosOss::Chmod(const char* path, mode_t mode, XrdOucEnv* envP) {
  if (!fs_) return -EIO;
  if (int rc = apply_caller(fs_, envP)) return rc;
  aios_posix_stat st{};
  int rc = lookup_path(fs_, path, &st);
  if (rc) return rc;
  aios_posix_stat set{};
  set.mode = static_cast<uint32_t>(mode);
  return aios_posix_setattr(fs_, st.ino, &set, AIOS_POSIX_SET_MODE);
}

int XrdAiosOss::Create(const char* /*tid*/, const char* path, mode_t mode, XrdOucEnv& env,
                       int opts) {
  if (!fs_) return -EIO;
  if (int rc = apply_caller(fs_, env)) return rc;

  if (opts & XRDOSS_mkpath) {
    std::string p = path ? path : "";
    while (!p.empty() && p.back() == '/') p.pop_back();
    const auto slash = p.rfind('/');
    if (slash != std::string::npos && slash > 0) {
      const std::string parent = p.substr(0, slash);
      if (int rc = mkdir_p(fs_, parent.c_str(), 0755)) {
        if (rc != -EEXIST) return rc;
      }
    }
  }

  aios_posix_stat existing{};
  int rc = lookup_path(fs_, path, &existing);
  if (rc == 0) {
    if (opts & XRDOSS_new) return -EEXIST;
    if (S_ISDIR(existing.mode)) return -EISDIR;
    rc = aios_posix_truncate(fs_, existing.ino, 0);
    return rc;
  }
  if (rc != -ENOENT) return rc;

  uint64_t parent = 0;
  std::string name;
  rc = resolve_parent(fs_, path, &parent, &name);
  if (rc) return rc;
  aios_posix_stat st{};
  return aios_posix_create(fs_, parent, name.c_str(), static_cast<uint32_t>(mode ? mode : 0644),
                           &st);
}

int XrdAiosOss::Mkdir(const char* path, mode_t mode, int mkpath, XrdOucEnv* envP) {
  if (!fs_) return -EIO;
  if (int rc = apply_caller(fs_, envP)) return rc;
  if (mkpath) return mkdir_p(fs_, path, mode ? mode : 0755);
  uint64_t parent = 0;
  std::string name;
  int rc = resolve_parent(fs_, path, &parent, &name);
  if (rc) return rc;
  return aios_posix_mkdir(fs_, parent, name.c_str(), static_cast<uint32_t>(mode ? mode : 0755),
                          nullptr);
}

int XrdAiosOss::Remdir(const char* path, int /*Opts*/, XrdOucEnv* envP) {
  if (!fs_) return -EIO;
  if (int rc = apply_caller(fs_, envP)) return rc;
  uint64_t parent = 0;
  std::string name;
  int rc = resolve_parent(fs_, path, &parent, &name);
  if (rc) return rc;
  return aios_posix_rmdir(fs_, parent, name.c_str());
}

int XrdAiosOss::Rename(const char* oPath, const char* nPath, XrdOucEnv* oEnvP,
                       XrdOucEnv* /*nEnvP*/) {
  if (!fs_) return -EIO;
  if (int rc = apply_caller(fs_, oEnvP)) return rc;
  uint64_t op = 0, np = 0;
  std::string on, nn;
  int rc = resolve_parent(fs_, oPath, &op, &on);
  if (rc) return rc;
  rc = resolve_parent(fs_, nPath, &np, &nn);
  if (rc) return rc;
  return aios_posix_rename(fs_, op, on.c_str(), np, nn.c_str());
}

int XrdAiosOss::Stat(const char* path, struct stat* buff, int /*opts*/, XrdOucEnv* envP) {
  if (!fs_ || !buff) return -EIO;
  if (int rc = apply_caller(fs_, envP)) return rc;
  aios_posix_stat st{};
  int rc = lookup_path(fs_, path, &st);
  if (rc) return rc;
  fill_stat(st, buff);
  return XrdOssOK;
}

int XrdAiosOss::Truncate(const char* path, unsigned long long fsize, XrdOucEnv* envP) {
  if (!fs_) return -EIO;
  if (int rc = apply_caller(fs_, envP)) return rc;
  aios_posix_stat st{};
  int rc = lookup_path(fs_, path, &st);
  if (rc) return rc;
  return aios_posix_truncate(fs_, st.ino, fsize);
}

int XrdAiosOss::Unlink(const char* path, int /*Opts*/, XrdOucEnv* envP) {
  if (!fs_) return -EIO;
  if (int rc = apply_caller(fs_, envP)) return rc;
  uint64_t parent = 0;
  std::string name;
  int rc = resolve_parent(fs_, path, &parent, &name);
  if (rc) return rc;
  return aios_posix_unlink(fs_, parent, name.c_str());
}

}  // namespace xrd
}  // namespace aios

using aios::xrd::XrdAiosOss;

extern "C" {
XrdVERSIONINFO(XrdOssGetStorageSystem2, aios);

XrdOss* XrdOssGetStorageSystem2(XrdOss* /*native_oss*/, XrdSysLogger* Logger, const char* config_fn,
                                const char* parms, XrdOucEnv* /*envP*/) {
  auto* oss = new XrdAiosOss();
  oss->set_boot(config_fn, parms);
  // Init is called by the loader after this returns.
  (void)Logger;
  return oss;
}

// Older entry point for loaders that only resolve v1.
XrdVERSIONINFO(XrdOssGetStorageSystem, aios);

XrdOss* XrdOssGetStorageSystem(XrdOss* native_oss, XrdSysLogger* Logger, const char* config_fn,
                               const char* parms) {
  return XrdOssGetStorageSystem2(native_oss, Logger, config_fn, parms, nullptr);
}
}
