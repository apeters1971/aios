#pragma once

#include "ec/erasure_codec.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aios {

#if defined(AIOS_HAVE_ISAL)

// Systematic Reed–Solomon via Intel ISA-L (Cauchy generator matrix).
class IsaLReedSolomonCodec final : public ErasureCodec {
 public:
  IsaLReedSolomonCodec(int k, int m);

  int k() const override { return k_; }
  int m() const override { return m_; }
  const char* name() const override { return "isal"; }

  bool encode(std::span<const std::uint8_t> object,
              std::vector<std::vector<std::uint8_t>>& shards_out,
              std::string& err) const override;

  bool decode(const std::vector<std::optional<std::vector<std::uint8_t>>>& shards_in,
              std::size_t full_size, std::vector<std::uint8_t>& object_out,
              std::string& err) const override;

 private:
  int k_;
  int m_;  // parity count
  // Cached Cauchy encode matrix: (k+m) rows × k cols.
  std::vector<std::uint8_t> encode_matrix_;
};

#endif

std::unique_ptr<ErasureCodec> make_isal_rs_codec(int k, int m, std::string& err);

}  // namespace aios
