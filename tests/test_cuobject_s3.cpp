#include "cuobject/cuobject_endpoint.hpp"
#include <gtest/gtest.h>
#include "cuobject/cuobject_s3_xfer.hpp"

#include <iostream>
#include <string>
#include <vector>


TEST(CuObjectS3, Basic) {
  using namespace aios;
  // Null / factory
  {
    auto null_ep = make_null_cuobject_endpoint();
    EXPECT_TRUE(null_ep && !null_ep->available()) << "null unavailable";
    std::string reply, err;
    EXPECT_TRUE(!null_ep->rdma_get("k", nullptr, 0, "tok", reply, err)) << "null get fails";
    Config cfg;
    EXPECT_TRUE(!make_cuobject_endpoint(cfg)->available()) << "factory empty listen → null";
  }

  // Stub GET/PUT
  {
    StubCuObjectEndpoint stub;
    std::vector<std::uint8_t> buf(16, 'A');
    std::string reply, err;
    EXPECT_TRUE(stub.rdma_get("b/k", buf.data(), buf.size(), "tok1", reply, err)) << "stub get";
    EXPECT_TRUE(reply == "stub-ok") << "stub get reply";
    EXPECT_TRUE(stub.get_calls() == 1 && stub.last_get_size() == 16) << "stub get meta";

    stub.set_put_payload({'R', 'D', 'M', 'A'});
    std::vector<std::uint8_t> in(4);
    EXPECT_TRUE(stub.rdma_put("b/k", in.data(), in.size(), "tok2", reply, err)) << "stub put";
    EXPECT_TRUE(std::string(in.begin(), in.end()) == "RDMA") << "stub put payload";
  }

  // Helpers mirror S3Server control-plane decisions
  {
    EXPECT_TRUE(s3_want_rdma_get("tok", false, true)) << "want rdma get";
    EXPECT_TRUE(!s3_want_rdma_get("", false, true)) << "no token → no rdma";
    EXPECT_TRUE(!s3_want_rdma_get("tok", true, true)) << "range → no rdma";
    EXPECT_TRUE(!s3_want_rdma_get("tok", false, false)) << "head → no rdma";

    auto stub = std::make_shared<StubCuObjectEndpoint>(true);
    std::string data = "hello-tcp";
    std::string reply, err;
    EXPECT_TRUE(s3_try_rdma_get(stub.get(), "cuobj/tcp.bin", "client-token-get", data.data(),
                           data.size(), reply, err)) << "helper rdma get";
    EXPECT_TRUE(reply == "stub-ok") << "helper get reply";
    EXPECT_TRUE(stub->last_get_token() == "client-token-get") << "helper get token";
    EXPECT_TRUE(stub->get_calls() == 1) << "helper get calls";

    // GET failure → caller falls back to TCP (helper returns false)
    stub->set_fail(true);
    EXPECT_TRUE(!s3_try_rdma_get(stub.get(), "cuobj/tcp.bin", "tok", data.data(), data.size(), reply,
                            err)) << "helper get fail";
    stub->set_fail(false);

    stub->set_put_payload({'R', 'D', 'M', 'A'});
    std::vector<std::uint8_t> put_buf(4);
    EXPECT_TRUE(s3_try_rdma_put(stub.get(), "cuobj/rdma.bin", "client-token-put", put_buf.data(),
                           put_buf.size(), reply, err)) << "helper rdma put";
    EXPECT_TRUE(std::string(put_buf.begin(), put_buf.end()) == "RDMA") << "helper put bytes";
    EXPECT_TRUE(stub->put_calls() == 1) << "helper put calls";

    auto null_ep = make_null_cuobject_endpoint();
    EXPECT_TRUE(!s3_try_rdma_put(null_ep.get(), "cuobj/fail.bin", "tok", put_buf.data(), put_buf.size(),
                            reply, err)) << "put rejected when unavailable";
    EXPECT_TRUE(err.find("unavailable") != std::string::npos) << "unavailable message";
  }

  }
