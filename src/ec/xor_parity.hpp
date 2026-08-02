#pragma once

#include "ec/erasure_codec.hpp"

#include <memory>

namespace aios {

// Systematic XOR parity: k data shards + 1 parity (m must be 1).
class XorParityCodec final : public ErasureCodec {
 public:
  explicit XorParityCodec(int k);

  int k() const override { return k_; }
  int m() const override { return 1; }
  const char* name() const override { return "xor"; }

  bool encode(std::span<const std::uint8_t> object,
              std::vector<std::vector<std::uint8_t>>& shards_out,
              std::string& err) const override;

  bool decode(const std::vector<std::optional<std::vector<std::uint8_t>>>& shards_in,
              std::size_t full_size, std::vector<std::uint8_t>& object_out,
              std::string& err) const override;

 private:
  int k_;
};

std::unique_ptr<ErasureCodec> make_xor_parity_codec(int k, std::string& err);

}  // namespace aios
