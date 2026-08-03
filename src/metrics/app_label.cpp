#include "metrics/app_label.hpp"

#include <cctype>

namespace aios {
namespace {

thread_local std::string g_app_label;

bool is_label_char(unsigned char c) {
  return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':' || c == '/';
}

}  // namespace

bool normalize_app_label(const std::string& in, std::string& out, std::string& err) {
  out.clear();
  if (in.empty()) return true;
  if (in.size() > kAppLabelMaxLen) {
    err = "x-aios-app-label longer than 64 characters";
    return false;
  }
  for (unsigned char c : in) {
    if (!is_label_char(c)) {
      err = "x-aios-app-label must match [A-Za-z0-9_.:/-]+";
      return false;
    }
  }
  out = in;
  return true;
}

AppLabelScope::AppLabelScope(std::string label) : prev_(g_app_label) {
  g_app_label = std::move(label);
}

AppLabelScope::~AppLabelScope() { g_app_label = std::move(prev_); }

const std::string& AppLabelScope::current() { return g_app_label; }

}  // namespace aios
