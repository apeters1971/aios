#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aios {

std::string base64_encode(const std::uint8_t* data, std::size_t len);
inline std::string base64_encode(const std::vector<std::uint8_t>& data) {
  return base64_encode(data.data(), data.size());
}
inline std::string base64_encode(const std::string& data) {
  return base64_encode(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

// Returns false on invalid input.
bool base64_decode(const std::string& in, std::vector<std::uint8_t>& out, std::string& err);

}  // namespace aios
