#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aios {

// Thin codec interface: XOR parity (m=1) and optional ISA-L Reed–Solomon.
struct ErasureCodec {
  virtual ~ErasureCodec() = default;

  virtual int k() const = 0;
  virtual int m() const = 0;
  virtual const char* name() const = 0;
  int shard_count() const { return k() + m(); }

  // Encode full object into shard_count() equal-length shards (zero-padded).
  virtual bool encode(std::span<const std::uint8_t> object,
                      std::vector<std::vector<std::uint8_t>>& shards_out,
                      std::string& err) const = 0;

  // Decode from any k present shards. Missing entries are std::nullopt.
  // shards_in.size() must equal shard_count().
  virtual bool decode(const std::vector<std::optional<std::vector<std::uint8_t>>>& shards_in,
                      std::size_t full_size, std::vector<std::uint8_t>& object_out,
                      std::string& err) const = 0;
};

}  // namespace aios
