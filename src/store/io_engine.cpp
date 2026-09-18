#include "store/io_engine.hpp"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include <vector>

#ifdef AIOS_HAVE_LIBURING
#include <liburing.h>
#endif

namespace aios {
namespace {

bool posix_pread_exact(int fd, std::uint64_t offset, std::span<std::uint8_t> buf,
                       std::string& err) {
  std::size_t done = 0;
  while (done < buf.size()) {
    const ssize_t n =
        ::pread(fd, buf.data() + done, buf.size() - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      err = std::string("pread: ") + std::strerror(errno);
      return false;
    }
    if (n == 0) {
      err = "pread: unexpected EOF";
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

bool posix_pwrite_exact(int fd, std::uint64_t offset, std::span<const std::uint8_t> buf,
                        std::string& err) {
  std::size_t done = 0;
  while (done < buf.size()) {
    const ssize_t n =
        ::pwrite(fd, buf.data() + done, buf.size() - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      err = std::string("pwrite: ") + std::strerror(errno);
      return false;
    }
    if (n == 0) {
      err = "pwrite: short write";
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace

bool PosixIoEngine::pread(int fd, std::uint64_t offset, std::span<std::uint8_t> buf,
                          std::string& err) {
  if (buf.empty()) return true;
  return posix_pread_exact(fd, offset, buf, err);
}

bool PosixIoEngine::pwrite(int fd, std::uint64_t offset, std::span<const std::uint8_t> buf,
                           std::string& err) {
  if (buf.empty()) return true;
  return posix_pwrite_exact(fd, offset, buf, err);
}

bool PosixIoEngine::pread_many(std::span<IoPread> ops, std::string& err) {
  for (auto& op : ops) {
    if (!pread(op.fd, op.offset, op.buf, err)) return false;
  }
  return true;
}

#ifdef AIOS_HAVE_LIBURING

struct UringIoEngine::Impl {
  io_uring ring{};
  bool ready{false};
  std::mutex mu;
  unsigned qd{256};
};

UringIoEngine::UringIoEngine(unsigned queue_depth) : impl_(std::make_unique<Impl>()) {
  impl_->qd = queue_depth < 8 ? 8 : queue_depth;
  if (io_uring_queue_init(impl_->qd, &impl_->ring, 0) == 0) impl_->ready = true;
}

UringIoEngine::~UringIoEngine() {
  if (impl_ && impl_->ready) {
    io_uring_queue_exit(&impl_->ring);
    impl_->ready = false;
  }
}

bool UringIoEngine::pread(int fd, std::uint64_t offset, std::span<std::uint8_t> buf,
                          std::string& err) {
  if (!impl_ || !impl_->ready) return posix_pread_exact(fd, offset, buf, err);
  IoPread op{fd, offset, buf};
  return pread_many(std::span<IoPread>(&op, 1), err);
}

bool UringIoEngine::pwrite(int fd, std::uint64_t offset, std::span<const std::uint8_t> buf,
                           std::string& err) {
  // Sequential appends are issued one at a time; POSIX pwrite is enough.
  return posix_pwrite_exact(fd, offset, buf, err);
}

bool UringIoEngine::pread_many(std::span<IoPread> ops, std::string& err) {
  if (ops.empty()) return true;
  if (!impl_ || !impl_->ready) {
    for (auto& op : ops) {
      if (!posix_pread_exact(op.fd, op.offset, op.buf, err)) return false;
    }
    return true;
  }

  std::lock_guard<std::mutex> lock(impl_->mu);
  std::vector<std::size_t> done(ops.size(), 0);
  std::size_t remaining = 0;
  for (const auto& op : ops) {
    if (!op.buf.empty()) ++remaining;
  }
  if (remaining == 0) return true;

  auto submit_one = [&](std::size_t i) -> bool {
    auto& op = ops[i];
    if (done[i] >= op.buf.size()) return true;
    io_uring_sqe* sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) return false;
    io_uring_prep_read(sqe, op.fd, op.buf.data() + done[i], op.buf.size() - done[i],
                       static_cast<off_t>(op.offset + done[i]));
    sqe->user_data = i;
    return true;
  };

  std::size_t next = 0;
  unsigned inflight = 0;
  const unsigned max_inflight = impl_->qd;

  auto pump = [&]() -> bool {
    while (inflight < max_inflight && next < ops.size()) {
      if (ops[next].buf.empty() || done[next] >= ops[next].buf.size()) {
        ++next;
        continue;
      }
      if (!submit_one(next)) break;
      ++inflight;
      ++next;
    }
    if (inflight == 0) return true;
    const int sub = io_uring_submit(&impl_->ring);
    if (sub < 0) {
      err = std::string("io_uring_submit: ") + std::strerror(-sub);
      return false;
    }
    return true;
  };

  if (!pump()) return false;

  while (remaining > 0) {
    io_uring_cqe* cqe = nullptr;
    const int wr = io_uring_wait_cqe(&impl_->ring, &cqe);
    if (wr < 0) {
      err = std::string("io_uring_wait_cqe: ") + std::strerror(-wr);
      return false;
    }
    const auto i = static_cast<std::size_t>(cqe->user_data);
    const int res = cqe->res;
    io_uring_cqe_seen(&impl_->ring, cqe);
    --inflight;
    if (res < 0) {
      err = std::string("io_uring read: ") + std::strerror(-res);
      return false;
    }
    if (res == 0) {
      err = "io_uring read: unexpected EOF";
      return false;
    }
    done[i] += static_cast<std::size_t>(res);
    if (done[i] < ops[i].buf.size()) {
      if (!submit_one(i)) {
        // Ring full: submit what we have and retry this SQE next loop.
        if (!pump()) return false;
        io_uring_sqe* sqe = io_uring_get_sqe(&impl_->ring);
        if (!sqe) {
          err = "io_uring: no SQE";
          return false;
        }
        io_uring_prep_read(sqe, ops[i].fd, ops[i].buf.data() + done[i],
                           ops[i].buf.size() - done[i],
                           static_cast<off_t>(ops[i].offset + done[i]));
        sqe->user_data = i;
        ++inflight;
        const int sub = io_uring_submit(&impl_->ring);
        if (sub < 0) {
          err = std::string("io_uring_submit: ") + std::strerror(-sub);
          return false;
        }
      } else {
        ++inflight;
        const int sub = io_uring_submit(&impl_->ring);
        if (sub < 0) {
          err = std::string("io_uring_submit: ") + std::strerror(-sub);
          return false;
        }
      }
    } else {
      --remaining;
      if (!pump()) return false;
    }
  }
  return true;
}

#endif  // AIOS_HAVE_LIBURING

std::unique_ptr<IoEngine> make_io_engine() {
#ifdef AIOS_HAVE_LIBURING
  auto uring = std::make_unique<UringIoEngine>();
  // Constructor still succeeds if setup failed; pread_many falls back to POSIX.
  return uring;
#else
  return std::make_unique<PosixIoEngine>();
#endif
}

}  // namespace aios
