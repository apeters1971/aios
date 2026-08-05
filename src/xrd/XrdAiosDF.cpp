#include "xrd/XrdAiosDF.hpp"
#include "xrd/XrdAiosOss.hpp"
#include "xrd/XrdAiosUtil.hpp"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace aios {
namespace xrd {

XrdAiosFile::XrdAiosFile(XrdAiosOss* oss, const char* tid)
    : XrdOssDF(tid, XrdOssDF::DF_isFile), oss_(oss) {}

XrdAiosFile::~XrdAiosFile() { Close(); }

int XrdAiosFile::Open(const char* path, int Oflag, mode_t Mode, XrdOucEnv& env) {
  if (!oss_ || !oss_->fs()) return -EIO;
  if (int rc = apply_caller(oss_->fs(), env)) return rc;

  aios_posix_stat st{};
  int rc = lookup_path(oss_->fs(), path, &st);
  const bool create = (Oflag & O_CREAT) != 0;
  const bool excl = (Oflag & O_EXCL) != 0;

  if (rc == -ENOENT && create) {
    uint64_t parent = 0;
    std::string name;
    rc = resolve_parent(oss_->fs(), path, &parent, &name);
    if (rc) return rc;
    rc = aios_posix_create(oss_->fs(), parent, name.c_str(), static_cast<uint32_t>(Mode ? Mode : 0644),
                           &st);
    if (rc) return rc;
  } else if (rc) {
    return rc;
  } else if (create && excl) {
    return -EEXIST;
  }

  if (!S_ISREG(st.mode)) return -EISDIR;

  int amode = 0;
  const int acc = Oflag & O_ACCMODE;
  if (acc == O_RDONLY || acc == O_RDWR) amode |= R_OK;
  if (acc == O_WRONLY || acc == O_RDWR) amode |= W_OK;
  if (amode) {
    rc = aios_posix_access(oss_->fs(), st.ino, amode);
    if (rc) return rc;
  }

  if (Oflag & O_TRUNC) {
    rc = aios_posix_truncate(oss_->fs(), st.ino, 0);
    if (rc) return rc;
  }

  ino_ = st.ino;
  open_ = true;
  return XrdOssOK;
}

int XrdAiosFile::Close(long long* retsz) {
  if (retsz && open_ && oss_ && oss_->fs()) {
    aios_posix_stat st{};
    if (aios_posix_getattr(oss_->fs(), ino_, &st) == 0) *retsz = static_cast<long long>(st.size);
  }
  open_ = false;
  ino_ = 0;
  return XrdOssOK;
}

ssize_t XrdAiosFile::Read(void* buffer, off_t offset, size_t size) {
  if (!open_ || !oss_ || !oss_->fs()) return -EBADF;
  size_t got = 0;
  int rc = aios_posix_read(oss_->fs(), ino_, static_cast<uint64_t>(offset), buffer, size, &got);
  if (rc) return rc;
  return static_cast<ssize_t>(got);
}

ssize_t XrdAiosFile::Write(const void* buffer, off_t offset, size_t size) {
  if (!open_ || !oss_ || !oss_->fs()) return -EBADF;
  size_t wrote = 0;
  int rc =
      aios_posix_write(oss_->fs(), ino_, static_cast<uint64_t>(offset), buffer, size, &wrote);
  if (rc) return rc;
  return static_cast<ssize_t>(wrote);
}

int XrdAiosFile::Fstat(struct stat* buf) {
  if (!open_ || !oss_ || !oss_->fs() || !buf) return -EBADF;
  aios_posix_stat st{};
  int rc = aios_posix_getattr(oss_->fs(), ino_, &st);
  if (rc) return rc;
  fill_stat(st, buf);
  return XrdOssOK;
}

int XrdAiosFile::Fsync() {
  if (!open_ || !oss_ || !oss_->fs()) return -EBADF;
  return aios_posix_fsync(oss_->fs(), ino_);
}

int XrdAiosFile::Ftruncate(unsigned long long flen) {
  if (!open_ || !oss_ || !oss_->fs()) return -EBADF;
  return aios_posix_truncate(oss_->fs(), ino_, flen);
}

int XrdAiosFile::Fchmod(mode_t mode) {
  if (!open_ || !oss_ || !oss_->fs()) return -EBADF;
  aios_posix_stat st{};
  st.mode = static_cast<uint32_t>(mode);
  return aios_posix_setattr(oss_->fs(), ino_, &st, AIOS_POSIX_SET_MODE);
}

XrdAiosDir::XrdAiosDir(XrdAiosOss* oss, const char* tid)
    : XrdOssDF(tid, XrdOssDF::DF_isDir), oss_(oss) {}

XrdAiosDir::~XrdAiosDir() { Close(); }

int XrdAiosDir::Opendir(const char* path, XrdOucEnv& env) {
  if (!oss_ || !oss_->fs()) return -EIO;
  if (int rc = apply_caller(oss_->fs(), env)) return rc;
  aios_posix_stat st{};
  int rc = lookup_path(oss_->fs(), path, &st);
  if (rc) return rc;
  if (!S_ISDIR(st.mode)) return -ENOTDIR;
  if ((rc = aios_posix_access(oss_->fs(), st.ino, R_OK))) return rc;

  entries_.clear();
  next_ = 0;
  offset_ = 0;
  aios_posix_dirent ents[64];
  while (true) {
    int n = aios_posix_readdir(oss_->fs(), st.ino, &offset_, ents, 64);
    if (n < 0) return n;
    if (n == 0) break;
    for (int i = 0; i < n; ++i) entries_.emplace_back(ents[i].name, ents[i].ino);
  }
  ino_ = st.ino;
  open_ = true;
  return XrdOssOK;
}

int XrdAiosDir::Readdir(char* buff, int blen) {
  if (!open_ || !buff || blen <= 0) return -EINVAL;
  if (next_ >= entries_.size()) {
    buff[0] = '\0';
    if (stat_ret_) std::memset(stat_ret_, 0, sizeof(*stat_ret_));
    return XrdOssOK;
  }
  const auto& [name, child] = entries_[next_++];
  if (static_cast<int>(name.size()) >= blen) return -ENAMETOOLONG;
  std::memcpy(buff, name.c_str(), name.size() + 1);
  if (stat_ret_ && oss_ && oss_->fs()) {
    aios_posix_stat st{};
    if (aios_posix_getattr(oss_->fs(), child, &st) == 0)
      fill_stat(st, stat_ret_);
    else
      std::memset(stat_ret_, 0, sizeof(*stat_ret_));
  }
  return XrdOssOK;
}

int XrdAiosDir::StatRet(struct stat* buff) {
  stat_ret_ = buff;
  return XrdOssOK;
}

int XrdAiosDir::Close(long long* /*retsz*/) {
  open_ = false;
  ino_ = 0;
  entries_.clear();
  next_ = 0;
  offset_ = 0;
  stat_ret_ = nullptr;
  return XrdOssOK;
}

}  // namespace xrd
}  // namespace aios
