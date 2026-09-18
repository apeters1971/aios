#include "store/body_log.hpp"

#include "util/crc32c.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace aios {
namespace {

constexpr std::uint32_t kSegMagic = 0x314F4941u;  // 'AIO1'
constexpr std::uint32_t kRecMagic = 0x4A424F41u;  // 'AOBJ'
constexpr std::uint32_t kSegVersion = 1;
constexpr std::size_t kSegHeaderSize = 16;
constexpr std::size_t kRecHeaderSize = 16;

void put_le_u32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

std::uint64_t align8(std::uint64_t n) { return (n + 7u) & ~std::uint64_t{7}; }

IoEngine& engine_or(IoEngine* io, PosixIoEngine& posix) { return io ? *io : posix; }

}  // namespace

bool is_segment_locator(std::string_view path) {
  return path.size() > 4 && path.substr(0, 4) == "seg/";
}

std::string format_segment_locator(const BodyLocation& loc) {
  char buf[80];
  std::snprintf(buf, sizeof(buf), "seg/%012u.seg:%llu:%u", loc.segment_id,
                static_cast<unsigned long long>(loc.offset), loc.length);
  return buf;
}

bool parse_segment_locator(std::string_view path, BodyLocation& out) {
  out = BodyLocation{};
  if (!is_segment_locator(path)) return false;
  // seg/000000000001.seg:<off>:<len>
  const auto rest = path.substr(4);
  const auto dot = rest.find(".seg:");
  if (dot == std::string_view::npos) return false;
  const auto id_sv = rest.substr(0, dot);
  const auto nums = rest.substr(dot + 5);
  const auto colon = nums.find(':');
  if (colon == std::string_view::npos) return false;
  const auto off_sv = nums.substr(0, colon);
  const auto len_sv = nums.substr(colon + 1);
  unsigned id = 0;
  unsigned long long off = 0;
  unsigned len = 0;
  if (std::from_chars(id_sv.data(), id_sv.data() + id_sv.size(), id).ec != std::errc{}) {
    return false;
  }
  if (std::from_chars(off_sv.data(), off_sv.data() + off_sv.size(), off).ec != std::errc{}) {
    return false;
  }
  if (std::from_chars(len_sv.data(), len_sv.data() + len_sv.size(), len).ec != std::errc{}) {
    return false;
  }
  if (id == 0) return false;
  out.segment_id = id;
  out.offset = off;
  out.length = len;
  return true;
}

BodyLog::~BodyLog() { close(); }

void BodyLog::close() {
  for (int& fd : fds_) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  fds_.clear();
  active_fd_ = -1;
  active_id_ = 0;
  cursor_ = 0;
  dir_.clear();
}

std::string BodyLog::segment_abs(std::uint32_t id) const {
  char name[32];
  std::snprintf(name, sizeof(name), "%012u.seg", id);
  return (fs::path(dir_) / "segments" / name).string();
}

bool BodyLog::open_segment_locked(std::uint32_t id, bool create, std::string& err) {
  if (id == 0) {
    err = "invalid segment id";
    return false;
  }
  if (fds_.size() <= id) fds_.resize(id + 1, -1);
  if (fds_[id] >= 0) {
    if (id == active_id_) active_fd_ = fds_[id];
    return true;
  }
  const auto path = segment_abs(id);
  int flags = O_RDWR;
  if (create) flags |= O_CREAT;
  int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    err = std::string("open segment: ") + std::strerror(errno);
    return false;
  }
  fds_[id] = fd;
  if (id == active_id_) active_fd_ = fd;
  return true;
}

int BodyLog::fd_for(std::uint32_t segment_id, std::string& err) {
  if (segment_id == 0) {
    err = "invalid segment id";
    return -1;
  }
  if (fds_.size() > segment_id && fds_[segment_id] >= 0) return fds_[segment_id];
  if (!open_segment_locked(segment_id, /*create=*/false, err)) return -1;
  return fds_[segment_id];
}

bool BodyLog::open(const std::string& shard_dir, IoEngine* io, bool data_fsync,
                   std::uint64_t segment_size, std::string& err) {
  close();
  dir_ = shard_dir;
  io_ = io;
  data_fsync_ = data_fsync;
  segment_size_ = segment_size < 4096 ? 4096 : segment_size;

  std::error_code ec;
  fs::create_directories(fs::path(dir_) / "segments", ec);
  if (ec) {
    err = "mkdir segments: " + ec.message();
    return false;
  }

  std::uint32_t max_id = 0;
  const fs::path segdir = fs::path(dir_) / "segments";
  for (auto it = fs::directory_iterator(segdir, ec); it != fs::directory_iterator(); ++it) {
    if (!it->is_regular_file(ec)) continue;
    const auto name = it->path().filename().string();
    unsigned id = 0;
    if (name.size() < 5 || name.substr(name.size() - 4) != ".seg") continue;
    if (std::from_chars(name.data(), name.data() + name.size() - 4, id).ec != std::errc{}) {
      continue;
    }
    if (id > max_id) max_id = id;
  }

  if (max_id == 0) {
    return rotate_locked(err);
  }
  active_id_ = max_id;
  if (!open_segment_locked(active_id_, /*create=*/false, err)) return false;
  struct stat st {};
  if (::fstat(active_fd_, &st) != 0) {
    err = std::string("fstat segment: ") + std::strerror(errno);
    return false;
  }
  cursor_ = static_cast<std::uint64_t>(st.st_size);
  if (cursor_ < kSegHeaderSize) {
    // Incomplete header: rewrite it.
    std::uint8_t hdr[kSegHeaderSize]{};
    put_le_u32(hdr + 0, kSegMagic);
    put_le_u32(hdr + 4, kSegVersion);
    put_le_u32(hdr + 8, active_id_);
    put_le_u32(hdr + 12, 0);
    auto& ioeng = engine_or(io_, posix_);
    if (!ioeng.pwrite(active_fd_, 0, std::span<const std::uint8_t>(hdr, kSegHeaderSize), err)) {
      return false;
    }
    cursor_ = kSegHeaderSize;
  }
  return true;
}

bool BodyLog::rotate_locked(std::string& err) {
  const std::uint32_t next = active_id_ + 1;
  active_id_ = next;
  active_fd_ = -1;
  if (!open_segment_locked(next, /*create=*/true, err)) return false;
  std::uint8_t hdr[kSegHeaderSize]{};
  put_le_u32(hdr + 0, kSegMagic);
  put_le_u32(hdr + 4, kSegVersion);
  put_le_u32(hdr + 8, next);
  put_le_u32(hdr + 12, 0);
  auto& ioeng = engine_or(io_, posix_);
  if (!ioeng.pwrite(active_fd_, 0, std::span<const std::uint8_t>(hdr, kSegHeaderSize), err)) {
    return false;
  }
  if (data_fsync_ && ::fsync(active_fd_) != 0) {
    err = std::string("fsync new segment: ") + std::strerror(errno);
    return false;
  }
  cursor_ = kSegHeaderSize;
  return true;
}

bool BodyLog::append(std::span<const std::uint8_t> data, BodyLocation& out, std::string& err) {
  if (data.size() > std::numeric_limits<std::uint32_t>::max()) {
    err = "body too large for segment record";
    return false;
  }
  if (active_fd_ < 0) {
    err = "body log not open";
    return false;
  }
  const auto rec = kRecHeaderSize + data.size();
  const auto padded = align8(rec);
  if (cursor_ > 0 && cursor_ + padded > segment_size_ && cursor_ > kSegHeaderSize) {
    if (!rotate_locked(err)) return false;
  }
  // A single record larger than the segment file is still written (one-object
  // segment); callers should have routed huge bodies to standalone files.
  std::uint8_t hdr[kRecHeaderSize]{};
  put_le_u32(hdr + 0, kRecMagic);
  put_le_u32(hdr + 4, static_cast<std::uint32_t>(data.size()));
  put_le_u32(hdr + 8, data.empty() ? 0 : crc32c(data.data(), data.size()));
  put_le_u32(hdr + 12, 0);
  auto& ioeng = engine_or(io_, posix_);
  if (!ioeng.pwrite(active_fd_, cursor_, std::span<const std::uint8_t>(hdr, kRecHeaderSize),
                    err)) {
    return false;
  }
  if (!data.empty() &&
      !ioeng.pwrite(active_fd_, cursor_ + kRecHeaderSize, data, err)) {
    return false;
  }
  if (padded > rec) {
    std::uint8_t z[8]{};
    if (!ioeng.pwrite(active_fd_, cursor_ + rec,
                      std::span<const std::uint8_t>(z, static_cast<std::size_t>(padded - rec)),
                      err)) {
      return false;
    }
  }
  if (data_fsync_ && ::fsync(active_fd_) != 0) {
    err = std::string("fsync segment: ") + std::strerror(errno);
    return false;
  }
  out.segment_id = active_id_;
  out.offset = cursor_ + kRecHeaderSize;
  out.length = static_cast<std::uint32_t>(data.size());
  cursor_ += padded;
  return true;
}

bool BodyLog::read(const BodyLocation& loc, std::uint64_t off, std::size_t len, std::uint8_t* out,
                   std::string& err) {
  ReadOp op;
  op.loc = loc;
  op.off = off;
  op.buf = std::span<std::uint8_t>(out, len);
  return read_many(std::span<ReadOp>(&op, 1), err);
}

bool BodyLog::read_many(std::span<ReadOp> ops, std::string& err) {
  if (ops.empty()) return true;
  auto& ioeng = engine_or(io_, posix_);

  // Logical reads may extend past the physical record (delta growth). Split
  // into a physical pread plus a zero fill.
  std::vector<IoPread> ios;
  ios.reserve(ops.size());

  for (auto& op : ops) {
    if (op.buf.empty()) continue;
    if (op.loc.segment_id == 0) {
      err = "invalid segment location";
      return false;
    }
    const int fd = fd_for(op.loc.segment_id, err);
    if (fd < 0) return false;
    const std::uint64_t phys = op.loc.length;
    if (op.off >= phys) {
      std::memset(op.buf.data(), 0, op.buf.size());
      continue;
    }
    const std::size_t from_disk =
        static_cast<std::size_t>(std::min<std::uint64_t>(op.buf.size(), phys - op.off));
    if (from_disk < op.buf.size()) {
      std::memset(op.buf.data() + from_disk, 0, op.buf.size() - from_disk);
    }
    if (from_disk == 0) continue;
    IoPread io;
    io.fd = fd;
    io.offset = op.loc.offset + op.off;
    io.buf = std::span<std::uint8_t>(op.buf.data(), from_disk);
    ios.push_back(io);
  }
  if (ios.empty()) return true;
  return ioeng.pread_many(ios, err);
}

}  // namespace aios
