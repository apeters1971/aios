#include "store/fs_clone.hpp"

#include "util/log.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#elif defined(__APPLE__)
#include <sys/attr.h>
#include <sys/clonefile.h>
#endif

namespace fs = std::filesystem;

namespace aios {

bool clone_file_supported() {
#if defined(__linux__) || defined(__APPLE__)
  return true;
#else
  return false;
#endif
}

bool clone_file(const std::string& src, const std::string& dst, std::string& err) {
  std::error_code ec;
  fs::create_directories(fs::path(dst).parent_path(), ec);
  if (fs::exists(dst, ec)) {
    err = "clone dest exists: " + dst;
    return false;
  }

#if defined(__linux__)
  int src_fd = ::open(src.c_str(), O_RDONLY);
  if (src_fd < 0) {
    err = std::string("open src: ") + std::strerror(errno);
    return false;
  }
  int dst_fd = ::open(dst.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
  if (dst_fd < 0) {
    err = std::string("open dst: ") + std::strerror(errno);
    ::close(src_fd);
    return false;
  }
  if (::ioctl(dst_fd, FICLONE, src_fd) != 0) {
    err = std::string("FICLONE: ") + std::strerror(errno);
    ::close(dst_fd);
    ::close(src_fd);
    fs::remove(dst, ec);
    return false;
  }
  ::close(dst_fd);
  ::close(src_fd);
  return true;
#elif defined(__APPLE__)
  if (::clonefile(src.c_str(), dst.c_str(), 0) != 0) {
    err = std::string("clonefile: ") + std::strerror(errno);
    return false;
  }
  return true;
#else
  err = "clone_file not supported on this platform";
  return false;
#endif
}

bool copy_file_full(const std::string& src, const std::string& dst, std::string& err) {
  std::error_code ec;
  fs::create_directories(fs::path(dst).parent_path(), ec);
  fs::copy_file(src, dst, fs::copy_options::none, ec);
  if (ec) {
    err = "copy_file: " + ec.message();
    return false;
  }
  return true;
}

bool clone_or_copy_file(const std::string& src, const std::string& dst, bool allow_copy,
                        std::string& err) {
  if (clone_file(src, dst, err)) return true;
  if (!allow_copy) return false;
  AIOS_LOG_WARN("clone failed (", err, "); falling back to full copy");
  return copy_file_full(src, dst, err);
}

}  // namespace aios
