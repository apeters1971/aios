#pragma once

#include "posix/aios_posix.h"

#include "XrdOss/XrdOss.hh"

#include <string>
#include <vector>

namespace aios {
namespace xrd {

class XrdAiosOss;

class XrdAiosFile : public XrdOssDF {
public:
  XrdAiosFile(XrdAiosOss* oss, const char* tid);
  ~XrdAiosFile() override;

  int Open(const char* path, int Oflag, mode_t Mode, XrdOucEnv& env) override;
  int Close(long long* retsz = 0) override;
  ssize_t Read(void* buffer, off_t offset, size_t size) override;
  ssize_t Write(const void* buffer, off_t offset, size_t size) override;
  int Fstat(struct stat* buf) override;
  int Fsync() override;
  int Ftruncate(unsigned long long flen) override;
  int Fchmod(mode_t mode) override;

private:
  int restore_caller() const;

  XrdAiosOss* oss_{nullptr};
  uint64_t ino_{0};
  uint32_t uid_{0};
  uint32_t gid_{0};
  bool open_{false};
  bool caller_set_{false};
};

class XrdAiosDir : public XrdOssDF {
public:
  XrdAiosDir(XrdAiosOss* oss, const char* tid);
  ~XrdAiosDir() override;

  int Opendir(const char* path, XrdOucEnv& env) override;
  int Readdir(char* buff, int blen) override;
  int StatRet(struct stat* buff) override;
  int Close(long long* retsz = 0) override;

private:
  XrdAiosOss* oss_{nullptr};
  uint64_t ino_{0};
  uint64_t offset_{0};
  bool open_{false};
  struct stat* stat_ret_{nullptr};
  std::vector<std::pair<std::string, uint64_t>> entries_;
  size_t next_{0};
};

}  // namespace xrd
}  // namespace aios
