#include "ec/isal_rs.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#if defined(AIOS_HAVE_ISAL)
#include <isa-l/erasure_code.h>
#endif

namespace aios {

namespace {
constexpr int kMaxShards = 255;
}  // namespace

#if defined(AIOS_HAVE_ISAL)

namespace {

// Port of ISA-L ec_simple_example gf_gen_decode_matrix_simple.
int gen_decode_matrix(const std::uint8_t* encode_matrix, std::uint8_t* decode_matrix,
                      std::uint8_t* invert_matrix, std::uint8_t* temp_matrix,
                      std::uint8_t* decode_index, const int* frag_err_list, int nerrs, int k,
                      int /*total_m*/) {
  std::uint8_t frag_in_err[kMaxShards];
  std::memset(frag_in_err, 0, sizeof(frag_in_err));
  for (int i = 0; i < nerrs; ++i) frag_in_err[frag_err_list[i]] = 1;

  std::uint8_t* b = temp_matrix;
  for (int i = 0, r = 0; i < k; ++i, ++r) {
    while (frag_in_err[r]) ++r;
    for (int j = 0; j < k; ++j) b[k * i + j] = encode_matrix[k * r + j];
    decode_index[i] = static_cast<std::uint8_t>(r);
  }

  if (gf_invert_matrix(b, invert_matrix, k) < 0) return -1;

  for (int i = 0; i < nerrs; ++i) {
    if (frag_err_list[i] < k) {
      for (int j = 0; j < k; ++j) {
        decode_matrix[k * i + j] = invert_matrix[k * frag_err_list[i] + j];
      }
    }
  }

  for (int p = 0; p < nerrs; ++p) {
    if (frag_err_list[p] >= k) {
      for (int i = 0; i < k; ++i) {
        std::uint8_t s = 0;
        for (int j = 0; j < k; ++j) {
          s ^= gf_mul(invert_matrix[j * k + i], encode_matrix[k * frag_err_list[p] + j]);
        }
        decode_matrix[k * p + i] = s;
      }
    }
  }
  return 0;
}

}  // namespace

IsaLReedSolomonCodec::IsaLReedSolomonCodec(int k, int m) : k_(k), m_(m) {
  const int total = k_ + m_;
  encode_matrix_.assign(static_cast<std::size_t>(total * k_), 0);
  gf_gen_cauchy1_matrix(encode_matrix_.data(), total, k_);
}

bool IsaLReedSolomonCodec::encode(std::span<const std::uint8_t> object,
                                  std::vector<std::vector<std::uint8_t>>& shards_out,
                                  std::string& err) const {
  if (k_ < 1 || m_ < 1) {
    err = "invalid k/m";
    return false;
  }
  const std::size_t shard_len =
      (object.size() + static_cast<std::size_t>(k_) - 1) / static_cast<std::size_t>(k_);
  const int total = k_ + m_;
  shards_out.assign(static_cast<std::size_t>(total), std::vector<std::uint8_t>(shard_len, 0));

  for (int i = 0; i < k_; ++i) {
    const std::size_t off = static_cast<std::size_t>(i) * shard_len;
    if (off >= object.size()) continue;
    const std::size_t n = std::min(shard_len, object.size() - off);
    std::copy(object.begin() + static_cast<std::ptrdiff_t>(off),
              object.begin() + static_cast<std::ptrdiff_t>(off + n),
              shards_out[static_cast<std::size_t>(i)].begin());
  }

  if (shard_len == 0) return true;
  if (shard_len > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    err = "shard length exceeds ISA-L int limit";
    return false;
  }

  std::vector<std::uint8_t> g_tbls(static_cast<std::size_t>(k_ * m_ * 32));
  // ISA-L takes non-const coeff pointers but does not modify them.
  auto* coeff = const_cast<std::uint8_t*>(encode_matrix_.data() + (k_ * k_));
  ec_init_tables(k_, m_, coeff, g_tbls.data());

  std::vector<std::uint8_t*> frag_ptrs(static_cast<std::size_t>(total));
  for (int i = 0; i < total; ++i) {
    frag_ptrs[static_cast<std::size_t>(i)] = shards_out[static_cast<std::size_t>(i)].data();
  }
  ec_encode_data(static_cast<int>(shard_len), k_, m_, g_tbls.data(), frag_ptrs.data(),
                 frag_ptrs.data() + k_);
  return true;
}

bool IsaLReedSolomonCodec::decode(
    const std::vector<std::optional<std::vector<std::uint8_t>>>& shards_in,
    std::size_t full_size, std::vector<std::uint8_t>& object_out, std::string& err) const {
  const int total = k_ + m_;
  if (static_cast<int>(shards_in.size()) != total) {
    err = "wrong shard count";
    return false;
  }

  int nerrs = 0;
  int frag_err_list[kMaxShards];
  std::size_t shard_len = 0;
  bool have_len = false;
  int present = 0;
  for (int i = 0; i < total; ++i) {
    if (!shards_in[static_cast<std::size_t>(i)]) {
      if (nerrs >= m_) {
        err = "too many missing shards";
        return false;
      }
      frag_err_list[nerrs++] = i;
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
  if (shard_len > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    err = "shard length exceeds ISA-L int limit";
    return false;
  }

  // Working buffers: start from present shards; recover erasures in place.
  std::vector<std::vector<std::uint8_t>> shards(static_cast<std::size_t>(total));
  for (int i = 0; i < total; ++i) {
    if (shards_in[static_cast<std::size_t>(i)]) {
      shards[static_cast<std::size_t>(i)] = *shards_in[static_cast<std::size_t>(i)];
    } else {
      shards[static_cast<std::size_t>(i)].assign(shard_len, 0);
    }
  }

  if (nerrs > 0) {
    std::vector<std::uint8_t> decode_matrix(static_cast<std::size_t>(total * k_));
    std::vector<std::uint8_t> invert_matrix(static_cast<std::size_t>(total * k_));
    std::vector<std::uint8_t> temp_matrix(static_cast<std::size_t>(total * k_));
    std::vector<std::uint8_t> g_tbls(static_cast<std::size_t>(k_ * nerrs * 32));
    std::uint8_t decode_index[kMaxShards];

    if (gen_decode_matrix(const_cast<std::uint8_t*>(encode_matrix_.data()),
                          decode_matrix.data(), invert_matrix.data(), temp_matrix.data(),
                          decode_index, frag_err_list, nerrs, k_, total) != 0) {
      err = "failed to build decode matrix";
      return false;
    }

    std::vector<std::uint8_t*> recover_srcs(static_cast<std::size_t>(k_));
    for (int i = 0; i < k_; ++i) {
      recover_srcs[static_cast<std::size_t>(i)] =
          shards[static_cast<std::size_t>(decode_index[i])].data();
    }
    std::vector<std::vector<std::uint8_t>> recover_out(static_cast<std::size_t>(nerrs),
                                                      std::vector<std::uint8_t>(shard_len));
    std::vector<std::uint8_t*> recover_ptrs(static_cast<std::size_t>(nerrs));
    for (int i = 0; i < nerrs; ++i) {
      recover_ptrs[static_cast<std::size_t>(i)] = recover_out[static_cast<std::size_t>(i)].data();
    }

    ec_init_tables(k_, nerrs, decode_matrix.data(), g_tbls.data());
    ec_encode_data(static_cast<int>(shard_len), k_, nerrs, g_tbls.data(), recover_srcs.data(),
                   recover_ptrs.data());

    for (int i = 0; i < nerrs; ++i) {
      shards[static_cast<std::size_t>(frag_err_list[i])] =
          std::move(recover_out[static_cast<std::size_t>(i)]);
    }
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

#endif  // AIOS_HAVE_ISAL

std::unique_ptr<ErasureCodec> make_isal_rs_codec(int k, int m, std::string& err) {
#if defined(AIOS_HAVE_ISAL)
  if (k < 1 || m < 1) {
    err = "ec_k and ec_m must be >= 1";
    return nullptr;
  }
  if (k + m > kMaxShards) {
    err = "k+m exceeds ISA-L max (255)";
    return nullptr;
  }
  return std::make_unique<IsaLReedSolomonCodec>(k, m);
#else
  (void)k;
  (void)m;
  err = "ISA-L not available (build with AIOS_WITH_ISAL and libisal)";
  return nullptr;
#endif
}

}  // namespace aios
