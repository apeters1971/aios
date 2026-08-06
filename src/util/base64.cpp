#include "util/base64.hpp"

namespace aios {
namespace {

constexpr char kEncode[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decode_val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

std::string base64_encode(const std::uint8_t* data, std::size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (std::size_t i = 0; i < len; i += 3) {
    const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                            ((i + 1 < len ? data[i + 1] : 0u) << 8) |
                            (i + 2 < len ? data[i + 2] : 0u);
    out.push_back(kEncode[(n >> 18) & 63]);
    out.push_back(kEncode[(n >> 12) & 63]);
    out.push_back(i + 1 < len ? kEncode[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < len ? kEncode[n & 63] : '=');
  }
  return out;
}

bool base64_decode(const std::string& in, std::vector<std::uint8_t>& out, std::string& err) {
  if (in.size() % 4 != 0) {
    err = "base64 length not multiple of 4";
    return false;
  }
  out.clear();
  out.reserve(in.size() / 4 * 3);
  for (std::size_t i = 0; i < in.size(); i += 4) {
    const int a = decode_val(in[i]);
    const int b = decode_val(in[i + 1]);
    if (a < 0 || b < 0) {
      err = "invalid base64";
      return false;
    }
    const int c = in[i + 2] == '=' ? -2 : decode_val(in[i + 2]);
    const int d = in[i + 3] == '=' ? -2 : decode_val(in[i + 3]);
    if (c == -1 || d == -1) {
      err = "invalid base64";
      return false;
    }
    if (c == -2 && d != -2) {
      err = "invalid base64 padding";
      return false;
    }
    out.push_back(static_cast<std::uint8_t>((a << 2) | (b >> 4)));
    if (c >= 0) {
      out.push_back(static_cast<std::uint8_t>(((b & 15) << 4) | (c >> 2)));
    }
    if (d >= 0) {
      out.push_back(static_cast<std::uint8_t>(((c & 3) << 6) | d));
    }
  }
  err.clear();
  return true;
}

}  // namespace aios
