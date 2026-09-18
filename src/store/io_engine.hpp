#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace aios {

// One pread: fill buf from fd at offset. Short reads are retried internally.
struct IoPread {
  int fd{-1};
  std::uint64_t offset{0};
  std::span<std::uint8_t> buf;
};

// Hide io_uring vs POSIX behind one engine. Reads are the hot path; writes stay
// sequential appends on a single fd (BodyLog) and use pwrite.
class IoEngine {
 public:
  virtual ~IoEngine() = default;

  virtual const char* name() const = 0;

  virtual bool pread(int fd, std::uint64_t offset, std::span<std::uint8_t> buf,
                     std::string& err) = 0;
  virtual bool pwrite(int fd, std::uint64_t offset, std::span<const std::uint8_t> buf,
                      std::string& err) = 0;
  // Independent reads; implementations may submit them as one io_uring batch.
  virtual bool pread_many(std::span<IoPread> ops, std::string& err) = 0;
};

// POSIX pread/pwrite. Used on macOS and as the Linux fallback.
class PosixIoEngine final : public IoEngine {
 public:
  const char* name() const override { return "posix"; }
  bool pread(int fd, std::uint64_t offset, std::span<std::uint8_t> buf, std::string& err) override;
  bool pwrite(int fd, std::uint64_t offset, std::span<const std::uint8_t> buf,
              std::string& err) override;
  bool pread_many(std::span<IoPread> ops, std::string& err) override;
};

#ifdef AIOS_HAVE_LIBURING
class UringIoEngine final : public IoEngine {
 public:
  explicit UringIoEngine(unsigned queue_depth = 256);
  ~UringIoEngine() override;
  UringIoEngine(const UringIoEngine&) = delete;
  UringIoEngine& operator=(const UringIoEngine&) = delete;

  const char* name() const override { return "io_uring"; }
  bool pread(int fd, std::uint64_t offset, std::span<std::uint8_t> buf, std::string& err) override;
  bool pwrite(int fd, std::uint64_t offset, std::span<const std::uint8_t> buf,
              std::string& err) override;
  bool pread_many(std::span<IoPread> ops, std::string& err) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
#endif

// io_uring when liburing was found at configure time, otherwise POSIX.
std::unique_ptr<IoEngine> make_io_engine();

}  // namespace aios
