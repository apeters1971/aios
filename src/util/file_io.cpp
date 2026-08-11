#include "util/file_io.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace aios {
namespace {

bool write_all(int fd, const void* data, std::size_t n, std::string& err) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::size_t done = 0;
  while (done < n) {
    const ssize_t w = ::write(fd, p + done, n - done);
    if (w > 0) {
      done += static_cast<std::size_t>(w);
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    err = std::string("write: ") + std::strerror(errno);
    return false;
  }
  return true;
}

bool read_all(int fd, void* dst, std::size_t n, std::string& err) {
  auto* p = static_cast<std::uint8_t*>(dst);
  std::size_t done = 0;
  while (done < n) {
    const ssize_t r = ::read(fd, p + done, n - done);
    if (r > 0) {
      done += static_cast<std::size_t>(r);
      continue;
    }
    if (r == 0) {
      err = "short read";
      return false;
    }
    if (errno == EINTR) continue;
    err = std::string("read: ") + std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace

bool file_write_all_fd(int fd, const void* data, std::size_t n, std::string& err) {
  return write_all(fd, data, n, err);
}

bool file_create_empty(const std::string& path, std::string& err) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    err = std::string("create: ") + std::strerror(errno);
    return false;
  }
  ::close(fd);
  return true;
}

bool file_truncate(const std::string& path, std::string& err) {
  return file_create_empty(path, err);
}

bool file_read_exact(const std::string& path, void* dst, std::size_t n, std::string& err) {
  if (n == 0) return true;
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    err = std::string("open: ") + std::strerror(errno);
    return false;
  }
  const bool ok = read_all(fd, dst, n, err);
  ::close(fd);
  return ok;
}

bool file_read_exact(const std::string& path, std::size_t n, std::vector<std::uint8_t>& out,
                     std::string& err) {
  out.assign(n, 0);
  if (n == 0) return true;
  if (!file_read_exact(path, out.data(), n, err)) {
    out.clear();
    return false;
  }
  return true;
}

bool file_read_all(const std::string& path, std::vector<std::uint8_t>& out, std::string& err) {
  out.clear();
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    err = std::string("open: ") + std::strerror(errno);
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    err = std::string("fstat: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  if (st.st_size < 0) {
    err = "negative file size";
    ::close(fd);
    return false;
  }
  const auto n = static_cast<std::size_t>(st.st_size);
  out.assign(n, 0);
  bool ok = true;
  if (n > 0) {
    auto* p = out.data();
    std::size_t done = 0;
    while (done < n) {
      const ssize_t r = ::read(fd, p + done, n - done);
      if (r > 0) {
        done += static_cast<std::size_t>(r);
        continue;
      }
      if (r == 0) {
        err = "short read";
        ok = false;
        break;
      }
      if (errno == EINTR) continue;
      err = std::string("read: ") + std::strerror(errno);
      ok = false;
      break;
    }
  }
  ::close(fd);
  if (!ok) out.clear();
  return ok;
}

bool file_copy(const std::string& src, const std::string& dst, std::string& err,
               std::uint64_t max_bytes) {
  const int in_fd = ::open(src.c_str(), O_RDONLY);
  if (in_fd < 0) {
    err = std::string("open src: ") + std::strerror(errno);
    return false;
  }
  const int out_fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out_fd < 0) {
    err = std::string("open dst: ") + std::strerror(errno);
    ::close(in_fd);
    return false;
  }

  std::vector<std::uint8_t> buf(256 * 1024);
  std::uint64_t copied = 0;
  bool ok = true;
  while (true) {
    std::size_t want = buf.size();
    if (max_bytes > 0) {
      if (copied >= max_bytes) break;
      want = static_cast<std::size_t>(
          std::min<std::uint64_t>(want, max_bytes - copied));
    }
    const ssize_t n = ::read(in_fd, buf.data(), want);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      err = std::string("read: ") + std::strerror(errno);
      ok = false;
      break;
    }
    if (!write_all(out_fd, buf.data(), static_cast<std::size_t>(n), err)) {
      ok = false;
      break;
    }
    copied += static_cast<std::uint64_t>(n);
  }
  ::close(in_fd);
  ::close(out_fd);
  return ok;
}

bool file_write_trunc(const std::string& path, const void* data, std::size_t n,
                      std::string& err) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    err = std::string("open: ") + std::strerror(errno);
    return false;
  }
  const bool ok = (n == 0) || write_all(fd, data, n, err);
  ::close(fd);
  return ok;
}

}  // namespace aios
