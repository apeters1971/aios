#pragma once

#include <string>

namespace aios {

inline constexpr const char* kAppLabelHeader = "x-aios-app-label";
inline constexpr std::size_t kAppLabelMaxLen = 64;
inline constexpr std::size_t kAppLabelMaxDistinct = 256;
inline constexpr const char* kAppLabelOverflow = "_overflow";

// Validate/normalize a client application label.
// Empty input → empty (unlabeled). Invalid → false and err set.
bool normalize_app_label(const std::string& in, std::string& out, std::string& err);

// Request-scoped label for OPS accounting (and future QoS).
class AppLabelScope {
 public:
  explicit AppLabelScope(std::string label);
  ~AppLabelScope();

  AppLabelScope(const AppLabelScope&) = delete;
  AppLabelScope& operator=(const AppLabelScope&) = delete;

  static const std::string& current();

 private:
  std::string prev_;
};

}  // namespace aios
