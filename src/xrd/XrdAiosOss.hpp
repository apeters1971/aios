#pragma once

#include "posix/aios_posix.h"
#include "xrd/XrdAiosUtil.hpp"

#include "XrdOss/XrdOss.hh"

#include <memory>
#include <string>

class XrdSysError;
class XrdSysLogger;

namespace aios {
namespace xrd {

class XrdAiosOss : public XrdOss {
public:
  XrdAiosOss();
  ~XrdAiosOss() override;

  XrdOssDF* newDir(const char* tident) override;
  XrdOssDF* newFile(const char* tident) override;

  int Init(XrdSysLogger* lp, const char* cfn) override;
  int Init(XrdSysLogger* lp, const char* cfn, XrdOucEnv* envP) override;

  int Chmod(const char* path, mode_t mode, XrdOucEnv* envP = 0) override;
  int Create(const char* tid, const char* path, mode_t mode, XrdOucEnv& env, int opts = 0) override;
  int Mkdir(const char* path, mode_t mode, int mkpath = 0, XrdOucEnv* envP = 0) override;
  int Remdir(const char* path, int Opts = 0, XrdOucEnv* envP = 0) override;
  int Rename(const char* oPath, const char* nPath, XrdOucEnv* oEnvP = 0,
             XrdOucEnv* nEnvP = 0) override;
  int Stat(const char* path, struct stat* buff, int opts = 0, XrdOucEnv* envP = 0) override;
  int Truncate(const char* path, unsigned long long fsize, XrdOucEnv* envP = 0) override;
  int Unlink(const char* path, int Opts = 0, XrdOucEnv* envP = 0) override;

  uint64_t Features() override { return XRDOSS_HASNAIO; }

  aios_posix_fs* fs() const { return fs_; }
  XrdSysError* log() const { return log_.get(); }

  /* Kept for plugin factory to stash parms before Init. */
  void set_boot(const char* config_fn, const char* parms);

private:
  std::unique_ptr<XrdSysError> log_;
  std::string config_fn_;
  std::string parms_;
  MountConfig cfg_;
  aios_posix_fs* fs_{nullptr};
  // Stable C strings for aios_posix_config.
  std::string endpoint_;
  std::string cluster_key_;
  std::string volume_;
};

}  // namespace xrd
}  // namespace aios
