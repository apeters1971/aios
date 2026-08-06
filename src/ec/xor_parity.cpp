#include "ec/xor_parity.hpp"

#include <algorithm>

namespace aios {

XorParityCodec::XorParityCodec(int k) : k_(k) {}

bool XorParityCodec::encode(std::span<const std::uint8_t> object,
                            std::vector<std::vector<std::uint8_t>>& shards_out,
                            std::string& err) const {
  if (k_ < 1) {
    err = "invalid k";
    return false;
  }
  const std::size_t shard_len = (object.size() + static_cast<std::size_t>(k_) - 1) /
                                static_cast<std::size_t>(k_);
  shards_out.assign(static_cast<std::size_t>(k_ + 1),
                    std::vector<std::uint8_t>(shard_len, 0));
  for (int i = 0; i < k_; ++i) {
    const std::size_t off = static_cast<std::size_t>(i) * shard_len;
    if (off >= object.size()) continue;
    const std::size_t n = std::min(shard_len, object.size() - off);
    std::copy(object.begin() + static_cast<std::ptrdiff_t>(off),
              object.begin() + static_cast<std::ptrdiff_t>(off + n), shards_out[i].begin());
  }
  auto& parity = shards_out[static_cast<std::size_t>(k_)];
  for (int i = 0; i < k_; ++i) {
    for (std::size_t b = 0; b < shard_len; ++b) {
      parity[b] ^= shards_out[static_cast<std::size_t>(i)][b];
    }
  }
  return true;
}

bool XorParityCodec::decode(
    const std::vector<std::optional<std::vector<std::uint8_t>>>& shards_in,
    std::size_t full_size, std::vector<std::uint8_t>& object_out, std::string& err) const {
  if (static_cast<int>(shards_in.size()) != k_ + 1) {
    err = "wrong shard count";
    return false;
  }
  int present = 0;
  int missing = -1;
  std::size_t shard_len = 0;
  bool have_len = false;
  for (int i = 0; i < k_ + 1; ++i) {
    if (!shards_in[static_cast<std::size_t>(i)]) {
      if (missing >= 0) {
        err = "too many missing shards";
        return false;
      }
      missing = i;
      continue;
    }
    ++present;
    const auto& s = *shards_in[static_cast<std::size_t>(i)];
    if (!have_len) {
      shard_len = s.size();
      have_len = true;
    } else if (s.size() != shard_len) {
      err = "shard length mismatch";
      return false;
    }
  }
  if (present < k_) {
    err = "need at least k shards";
    return false;
  }
  if (!have_len && full_size == 0) {
    object_out.clear();
    return true;
  }
  if (!have_len) {
    err = "empty shards with nonzero full_size";
    return false;
  }
  const std::size_t want =
      (full_size + static_cast<std::size_t>(k_) - 1) / static_cast<std::size_t>(k_);
  if (shard_len != want) {
    err = "shard length does not match full_size";
    return false;
  }

  std::vector<std::vector<std::uint8_t>> shards(static_cast<std::size_t>(k_ + 1));
  for (int i = 0; i < k_ + 1; ++i) {
    if (shards_in[static_cast<std::size_t>(i)]) {
      shards[static_cast<std::size_t>(i)] = *shards_in[static_cast<std::size_t>(i)];
    }
  }
  if (missing >= 0) {
    std::vector<std::uint8_t> rebuilt(shard_len, 0);
    for (int i = 0; i < k_ + 1; ++i) {
      if (i == missing) continue;
      for (std::size_t b = 0; b < shard_len; ++b) {
        rebuilt[b] ^= shards[static_cast<std::size_t>(i)][b];
      }
    }
    shards[static_cast<std::size_t>(missing)] = std::move(rebuilt);
  }

  object_out.assign(full_size, 0);
  for (int i = 0; i < k_; ++i) {
    const std::size_t off = static_cast<std::size_t>(i) * shard_len;
    if (off >= full_size) break;
    const std::size_t n = std::min(shard_len, full_size - off);
    std::copy(shards[static_cast<std::size_t>(i)].begin(),
              shards[static_cast<std::size_t>(i)].begin() + static_cast<std::ptrdiff_t>(n),
              object_out.begin() + static_cast<std::ptrdiff_t>(off));
  }
  return true;
}

std::unique_ptr<ErasureCodec> make_xor_parity_codec(int k, std::string& err) {
  if (k < 1) {
    err = "ec_k must be >= 1";
    return nullptr;
  }
  return std::make_unique<XorParityCodec>(k);
}

}  // namespace aios
