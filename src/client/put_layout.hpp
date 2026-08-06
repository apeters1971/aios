#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace aios {

// Optional per-PUT placement overrides → x-aios-layout / storage-class / ec-* headers.
struct PutLayout {
  std::optional<std::string> layout;  // "replica" | "ec"
  std::optional<std::string> storage_class;
  std::optional<int> ec_k;
  std::optional<int> ec_m;
  std::optional<std::string> ec_codec;

  bool empty() const {
    return !layout && !storage_class && !ec_k && !ec_m && !ec_codec;
  }
};

inline void apply_put_layout_headers(std::unordered_map<std::string, std::string>& headers,
                                     const PutLayout& layout) {
  if (layout.layout) headers["x-aios-layout"] = *layout.layout;
  if (layout.storage_class) headers["x-aios-storage-class"] = *layout.storage_class;
  if (layout.ec_k) headers["x-aios-ec-k"] = std::to_string(*layout.ec_k);
  if (layout.ec_m) headers["x-aios-ec-m"] = std::to_string(*layout.ec_m);
  if (layout.ec_codec) headers["x-aios-ec-codec"] = *layout.ec_codec;
}

}  // namespace aios
