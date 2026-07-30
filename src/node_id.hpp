#pragma once

#include <string>
#include <unistd.h>

namespace aios {

inline std::string default_hostname() {
  char buf[256];
  if (::gethostname(buf, sizeof(buf)) != 0) {
    return "unknown";
  }
  buf[sizeof(buf) - 1] = '\0';
  return std::string(buf);
}

}  // namespace aios
