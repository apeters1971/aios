#pragma once

#include "client/error.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace aios {

// Boundary codec: typed API values ↔ UTF-8 strings on the wire/changelog.
template <class T>
struct stl_codec;

template <>
struct stl_codec<std::string> {
  static std::string to_string(const std::string& v) { return v; }
  static std::string from_string(const std::string& s) { return s; }
};

namespace detail {

inline void require_full_parse(const std::string& s, const char* end, const char* what) {
  if (end != s.data() + s.size() || s.empty()) {
    throw client_error("bad_request", std::string("stl_codec: invalid ") + what);
  }
}

}  // namespace detail

template <>
struct stl_codec<std::int64_t> {
  static std::string to_string(std::int64_t v) { return std::to_string(v); }
  static std::int64_t from_string(const std::string& s) {
    std::int64_t v = 0;
    const auto* first = s.data();
    const auto* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc{} || ptr != last) {
      throw client_error("bad_request", "stl_codec: invalid int64");
    }
    return v;
  }
};

template <>
struct stl_codec<std::uint64_t> {
  static std::string to_string(std::uint64_t v) { return std::to_string(v); }
  static std::uint64_t from_string(const std::string& s) {
    if (!s.empty() && s[0] == '-') {
      throw client_error("bad_request", "stl_codec: invalid uint64");
    }
    std::uint64_t v = 0;
    const auto* first = s.data();
    const auto* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc{} || ptr != last) {
      throw client_error("bad_request", "stl_codec: invalid uint64");
    }
    return v;
  }
};

template <>
struct stl_codec<int> {
  static std::string to_string(int v) { return stl_codec<std::int64_t>::to_string(v); }
  static int from_string(const std::string& s) {
    const auto v = stl_codec<std::int64_t>::from_string(s);
    if (v < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
        v > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
      throw client_error("bad_request", "stl_codec: int out of range");
    }
    return static_cast<int>(v);
  }
};

template <>
struct stl_codec<double> {
  static std::string to_string(double v) {
    char buf[64];
    // Enough digits for round-trip of IEEE-754 doubles.
    const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(buf)) {
      throw client_error("bad_request", "stl_codec: double encode failed");
    }
    return std::string(buf, static_cast<std::size_t>(n));
  }
  static double from_string(const std::string& s) {
    if (s.empty()) throw client_error("bad_request", "stl_codec: invalid double");
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    detail::require_full_parse(s, end, "double");
    return v;
  }
};

}  // namespace aios
