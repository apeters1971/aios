#pragma once

#include <stdexcept>
#include <string>

namespace aios {

class client_error : public std::runtime_error {
 public:
  client_error(std::string code, std::string message)
      : std::runtime_error(message), code_(std::move(code)) {}

  const std::string& code() const noexcept { return code_; }

 private:
  std::string code_;
};

}  // namespace aios
