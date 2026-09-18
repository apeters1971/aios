#pragma once

#include "store/io_engine.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aios {

// Packed append-only body records. SQLite still owns oid → location; this is
// the NVMe side: one (or a few) large files instead of a BLOB or a file per
// object. Location is stored as fs_path "seg/<id>.seg:<off>:<len>".
struct BodyLocation {
  std::uint32_t segment_id{0};
  std::uint64_t offset{0};  // payload start inside the segment file
  std::uint32_t length{0};
};

bool is_segment_locator(std::string_view path);
std::string format_segment_locator(const BodyLocation& loc);
bool parse_segment_locator(std::string_view path, BodyLocation& out);

class BodyLog {
 public:
  BodyLog() = default;
  ~BodyLog();
  BodyLog(const BodyLog&) = delete;
  BodyLog& operator=(const BodyLog&) = delete;

  bool open(const std::string& shard_dir, IoEngine* io, bool data_fsync,
            std::uint64_t segment_size, std::string& err);
  void close();

  bool append(std::span<const std::uint8_t> data, BodyLocation& out, std::string& err);

  // Read `len` bytes at logical offset `off` of the record. Bytes past the
  // physical payload are zeros (delta versions may grow the logical size).
  bool read(const BodyLocation& loc, std::uint64_t off, std::size_t len, std::uint8_t* out,
            std::string& err);

  struct ReadOp {
    BodyLocation loc;
    std::uint64_t off{0};
    std::span<std::uint8_t> buf;
  };
  bool read_many(std::span<ReadOp> ops, std::string& err);

  int fd_for(std::uint32_t segment_id, std::string& err);

 private:
  bool rotate_locked(std::string& err);
  bool open_segment_locked(std::uint32_t id, bool create, std::string& err);
  std::string segment_abs(std::uint32_t id) const;

  std::string dir_;
  IoEngine* io_{nullptr};
  PosixIoEngine posix_;  // fallback when io_ is null
  bool data_fsync_{true};
  std::uint64_t segment_size_{1ULL << 30};

  std::uint32_t active_id_{0};
  int active_fd_{-1};
  std::uint64_t cursor_{0};

  // Cached fds for reads of sealed segments (id → fd). active_id_ is also here.
  std::vector<int> fds_;  // index = id, -1 empty; grows as needed
};

}  // namespace aios
