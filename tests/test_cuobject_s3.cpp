#include "cuobject/cuobject_endpoint.hpp"
#include "cuobject/cuobject_s3_xfer.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

int& failures() {
  static int n = 0;
  return n;
}
void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL cuobject_s3: " << msg << "\n";
    ++failures();
  }
}

}  // namespace

int test_cuobject_s3() {
  using namespace aios;
  failures() = 0;

  // Null / factory
  {
    auto null_ep = make_null_cuobject_endpoint();
    expect(null_ep && !null_ep->available(), "null unavailable");
    std::string reply, err;
    expect(!null_ep->rdma_get("k", nullptr, 0, "tok", reply, err), "null get fails");
    Config cfg;
    expect(!make_cuobject_endpoint(cfg)->available(), "factory empty listen → null");
  }

  // Stub GET/PUT
  {
    StubCuObjectEndpoint stub;
    std::vector<std::uint8_t> buf(16, 'A');
    std::string reply, err;
    expect(stub.rdma_get("b/k", buf.data(), buf.size(), "tok1", reply, err), "stub get");
    expect(reply == "stub-ok", "stub get reply");
    expect(stub.get_calls() == 1 && stub.last_get_size() == 16, "stub get meta");

    stub.set_put_payload({'R', 'D', 'M', 'A'});
    std::vector<std::uint8_t> in(4);
    expect(stub.rdma_put("b/k", in.data(), in.size(), "tok2", reply, err), "stub put");
    expect(std::string(in.begin(), in.end()) == "RDMA", "stub put payload");
  }

  // Helpers mirror S3Server control-plane decisions
  {
    expect(s3_want_rdma_get("tok", false, true), "want rdma get");
    expect(!s3_want_rdma_get("", false, true), "no token → no rdma");
    expect(!s3_want_rdma_get("tok", true, true), "range → no rdma");
    expect(!s3_want_rdma_get("tok", false, false), "head → no rdma");

    auto stub = std::make_shared<StubCuObjectEndpoint>(true);
    std::string data = "hello-tcp";
    std::string reply, err;
    expect(s3_try_rdma_get(stub.get(), "cuobj/tcp.bin", "client-token-get", data.data(),
                           data.size(), reply, err),
           "helper rdma get");
    expect(reply == "stub-ok", "helper get reply");
    expect(stub->last_get_token() == "client-token-get", "helper get token");
    expect(stub->get_calls() == 1, "helper get calls");

    // GET failure → caller falls back to TCP (helper returns false)
    stub->set_fail(true);
    expect(!s3_try_rdma_get(stub.get(), "cuobj/tcp.bin", "tok", data.data(), data.size(), reply,
                            err),
           "helper get fail");
    stub->set_fail(false);

    stub->set_put_payload({'R', 'D', 'M', 'A'});
    std::vector<std::uint8_t> put_buf(4);
    expect(s3_try_rdma_put(stub.get(), "cuobj/rdma.bin", "client-token-put", put_buf.data(),
                           put_buf.size(), reply, err),
           "helper rdma put");
    expect(std::string(put_buf.begin(), put_buf.end()) == "RDMA", "helper put bytes");
    expect(stub->put_calls() == 1, "helper put calls");

    auto null_ep = make_null_cuobject_endpoint();
    expect(!s3_try_rdma_put(null_ep.get(), "cuobj/fail.bin", "tok", put_buf.data(), put_buf.size(),
                            reply, err),
           "put rejected when unavailable");
    expect(err.find("unavailable") != std::string::npos, "unavailable message");
  }

  if (failures() == 0) std::cout << "test_cuobject_s3 OK\n";
  return failures();
}
