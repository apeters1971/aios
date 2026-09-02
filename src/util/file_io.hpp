#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aios {

// POSIX file helpers for hot object I/O paths (avoid iostreams).

bool file_create_empty(const std::string& path, std::string& err);
// Create a new empty file; fails (EEXIST) if the path already exists.
bool file_create_exclusive(const std::string& path, std::string& err);
bool file_truncate(const std::string& path, std::string& err);

// Read exactly `n` bytes into `dst` (must have room for n).
bool file_read_exact(const std::string& path, void* dst, std::size_t n, std::string& err);

// Read exactly `n` bytes into a new vector.
bool file_read_exact(const std::string& path, std::size_t n, std::vector<std::uint8_t>& out,
                     std::string& err);

// Read the whole regular file.
bool file_read_all(const std::string& path, std::vector<std::uint8_t>& out, std::string& err);

// Copy entire regular file (or up to `max_bytes` if non-zero) src → dst (create/trunc).
bool file_copy(const std::string& src, const std::string& dst, std::string& err,
               std::uint64_t max_bytes = 0);

// Append/write raw bytes (create if needed). Used by remote get-to-file helpers.
bool file_write_trunc(const std::string& path, const void* data, std::size_t n, std::string& err);
bool file_write_all_fd(int fd, const void* data, std::size_t n, std::string& err);

}  // namespace aios
