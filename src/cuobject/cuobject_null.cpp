#include "cuobject/cuobject_endpoint.hpp"

namespace aios {
namespace {

class NullCuObjectEndpoint final : public CuObjectEndpoint {
 public:
  bool available() const override { return false; }

  bool rdma_get(const std::string&, const void*, std::size_t, const std::string&, std::string&,
                std::string& err) override {
    err = "cuobject unavailable";
    return false;
  }

  bool rdma_put(const std::string&, void*, std::size_t, const std::string&, std::string&,
                std::string& err) override {
    err = "cuobject unavailable";
    return false;
  }
};

}  // namespace

std::shared_ptr<CuObjectEndpoint> make_null_cuobject_endpoint() {
  return std::make_shared<NullCuObjectEndpoint>();
}

}  // namespace aios
