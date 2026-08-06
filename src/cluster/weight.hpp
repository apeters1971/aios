#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace aios {

// One weight unit ≈ 1 TiB of capacity (or free space when autotuning).
inline constexpr std::uint64_t kWeightByteUnit = 1024ull * 1024ull * 1024ull * 1024ull;

// Map a byte size to a placement weight (≥ 1). Rounds to nearest TiB.
inline int bytes_to_weight(std::uint64_t bytes) {
  if (bytes == 0) return 1;
  const std::uint64_t w = (bytes + kWeightByteUnit / 2) / kWeightByteUnit;
  if (w < 1) return 1;
  constexpr std::uint64_t kMax = 1'000'000;
  return static_cast<int>(std::min(w, kMax));
}

// Minimum |Δweight| required before autotune applies a change.
// need = max(min_delta, ceil(current * threshold_pct / 100)), at least 1.
inline int weight_autotune_delta_needed(int current_weight, int threshold_pct, int min_delta) {
  const int cur = current_weight > 0 ? current_weight : 1;
  const int pct = std::max(0, threshold_pct);
  const int md = std::max(1, min_delta);
  const int rel = (cur * pct + 99) / 100;  // ceil
  return std::max(md, rel);
}

inline bool weight_autotune_should_update(int current_weight, int proposed_weight,
                                         int threshold_pct, int min_delta) {
  const int need = weight_autotune_delta_needed(current_weight, threshold_pct, min_delta);
  const int a = current_weight > 0 ? current_weight : 1;
  const int b = proposed_weight > 0 ? proposed_weight : 1;
  return std::abs(a - b) >= need;
}

}  // namespace aios
