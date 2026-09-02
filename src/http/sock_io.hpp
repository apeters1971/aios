#pragma once

// Blocking socket primitives shared by the HTTP and S3 front-ends.
//
// Asio's synchronous read/write carry no deadline of their own: on SO_RCVTIMEO
// expiry recv reports EAGAIN, which asio reads as "not ready yet" and answers
// with another unbounded poll. The timeout is only observable from the raw
// syscall, so sessions that want SO_*TIMEO to mean anything go through these.

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>

namespace aios {

// Bounds how long a session may sit in a blocking read or write. Without it an
// idle client (one that connects and never sends, or that stops reading a large
// response) owns its thread forever.
inline void set_fd_timeouts(int fd, int idle_ms) {
  if (fd < 0 || idle_ms <= 0) return;
  // recv/send have to actually block for SO_*TIMEO to mean anything.
  const int fl = ::fcntl(fd, F_GETFL, 0);
  if (fl >= 0 && (fl & O_NONBLOCK)) ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  struct timeval tv;
  tv.tv_sec = idle_ms / 1000;
  tv.tv_usec = (idle_ms % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// Reads exactly n bytes. On failure err_out holds errno (0 for EOF).
inline bool fd_read_exact(int fd, void* out, std::size_t n, int& err_out) {
  auto* p = static_cast<char*>(out);
  std::size_t done = 0;
  while (done < n) {
    const auto r = ::recv(fd, p + done, n - done, 0);
    if (r > 0) {
      done += static_cast<std::size_t>(r);
      continue;
    }
    if (r == 0) {
      err_out = 0;
      return false;
    }
    if (errno == EINTR) continue;
    err_out = errno;
    return false;
  }
  err_out = 0;
  return true;
}

// Reads up to n bytes (at least one). Returns bytes read, 0 on EOF, -1 on error.
inline long fd_read_some(int fd, void* out, std::size_t n, int& err_out) {
  for (;;) {
    const auto r = ::recv(fd, out, n, 0);
    if (r >= 0) {
      err_out = 0;
      return static_cast<long>(r);
    }
    if (errno == EINTR) continue;
    err_out = errno;
    return -1;
  }
}

inline bool fd_write_all(int fd, const void* in, std::size_t n, int& err_out) {
  const auto* p = static_cast<const char*>(in);
  std::size_t done = 0;
  while (done < n) {
#ifdef MSG_NOSIGNAL
    const auto r = ::send(fd, p + done, n - done, MSG_NOSIGNAL);
#else
    const auto r = ::send(fd, p + done, n - done, 0);
#endif
    if (r > 0) {
      done += static_cast<std::size_t>(r);
      continue;
    }
    if (r < 0 && errno == EINTR) continue;
    err_out = r == 0 ? EPIPE : errno;
    return false;
  }
  err_out = 0;
  return true;
}

}  // namespace aios
