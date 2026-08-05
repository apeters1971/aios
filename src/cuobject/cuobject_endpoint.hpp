#pragma once

#include "config.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aios {

inline constexpr const char* kAmzRdmaToken = "x-amz-rdma-token";
inline constexpr const char* kAmzRdmaReply = "x-amz-rdma-reply";
// cuObjServer per-call limit (NVIDIA docs).
inline constexpr std::uint64_t kCuObjectMaxTransferBytes = 1ull << 30;

// Abstraction over NVIDIA cuObjServer (or stub/null for CI).
class CuObjectEndpoint {
 public:
  virtual ~CuObjectEndpoint() = default;

  virtual bool available() const = 0;

  // RDMA_WRITE: push `len` bytes from `local` to the client described by `token`.
  virtual bool rdma_get(const std::string& key, const void* local, std::size_t len,
                        const std::string& token, std::string& reply,
                        std::string& err) = 0;

  // RDMA_READ: pull `len` bytes into `local` from the client described by `token`.
  virtual bool rdma_put(const std::string& key, void* local, std::size_t len,
                        const std::string& token, std::string& reply,
                        std::string& err) = 0;
};

// Always unavailable (default when SDK absent or cuobject_listen empty).
std::shared_ptr<CuObjectEndpoint> make_null_cuobject_endpoint();

// Test fake: succeeds without NIC. PUT fills from `set_put_payload` when set.
class StubCuObjectEndpoint : public CuObjectEndpoint {
 public:
  explicit StubCuObjectEndpoint(bool available = true) : available_(available) {}

  bool available() const override { return available_; }
  void set_available(bool v) { available_ = v; }

  void set_put_payload(std::vector<std::uint8_t> data) { put_payload_ = std::move(data); }
  void set_fail(bool v) { fail_ = v; }

  const std::string& last_get_token() const { return last_get_token_; }
  const std::string& last_put_token() const { return last_put_token_; }
  std::size_t last_get_size() const { return last_get_size_; }
  std::size_t last_put_size() const { return last_put_size_; }
  int get_calls() const { return get_calls_; }
  int put_calls() const { return put_calls_; }

  bool rdma_get(const std::string& key, const void* local, std::size_t len,
                const std::string& token, std::string& reply, std::string& err) override;
  bool rdma_put(const std::string& key, void* local, std::size_t len, const std::string& token,
                std::string& reply, std::string& err) override;

 private:
  bool available_{true};
  bool fail_{false};
  std::vector<std::uint8_t> put_payload_;
  std::string last_get_token_;
  std::string last_put_token_;
  std::size_t last_get_size_{0};
  std::size_t last_put_size_{0};
  int get_calls_{0};
  int put_calls_{0};
  std::string last_key_;
};

// Construct from config: NVIDIA when AIOS_HAVE_CUOBJECT + cuobject_listen; else null.
std::shared_ptr<CuObjectEndpoint> make_cuobject_endpoint(const Config& cfg);

}  // namespace aios
