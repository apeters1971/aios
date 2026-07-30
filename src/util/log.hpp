#pragma once

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

namespace aios {

inline std::mutex& log_mutex() {
  static std::mutex m;
  return m;
}

inline const char* log_level() {
  static const char* lvl = [] {
    const char* e = std::getenv("AIOS_LOG");
    return e ? e : "info";
  }();
  return lvl;
}

inline bool log_enabled(std::string_view want) {
  const std::string_view cur = log_level();
  if (cur == "debug") return true;
  if (cur == "info") return want != "debug";
  if (cur == "warn") return want == "warn" || want == "error";
  if (cur == "error") return want == "error";
  return want != "debug";
}

template <typename... Args>
void log_line(std::string_view level, Args&&... args) {
  if (!log_enabled(level)) return;
  std::lock_guard lock(log_mutex());
  std::cerr << "[aios][" << level << "] ";
  (std::cerr << ... << std::forward<Args>(args));
  std::cerr << '\n';
}

#define AIOS_LOG_DEBUG(...) ::aios::log_line("debug", __VA_ARGS__)
#define AIOS_LOG_INFO(...) ::aios::log_line("info", __VA_ARGS__)
#define AIOS_LOG_WARN(...) ::aios::log_line("warn", __VA_ARGS__)
#define AIOS_LOG_ERROR(...) ::aios::log_line("error", __VA_ARGS__)

inline std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

}  // namespace aios
