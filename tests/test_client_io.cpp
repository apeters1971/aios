#include "test_helpers.hpp"

#include "client/session.hpp"
#include "cluster/place.hpp"
#include "ec/codec_factory.hpp"
#include "http/http_server.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"
#include "util/write_grant.hpp"

#include <boost/asio.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <thread>
#include <vector>

using aios::test::DualStoreFixture;
using aios::test::EcFixture;

TEST(WriteGrant, RoundTripAndTamper) {
  using namespace aios;
  WriteGrant g;
  g.oid = "obj/1";
  g.seq = 3;
  g.epoch = 9;
  g.layout = "replica";
  g.n = 2;
  g.storage_class = "nvme";
  g.full_size = 4;
  g.full_crc = 1;
  g.expires_ms = now_ms() + 60000;
  StorageTarget a;
  a.node_id = "n1";
  a.aios_path = "/data/a/aios";
  a.http_addr = "127.0.0.1:7480";
  a.addr = "127.0.0.1:7400";
  StorageTarget b = a;
  b.node_id = "n2";
  b.aios_path = "/data/b/aios";
  g.acting_set = {a, b};

  std::string err;
  const auto blob = seal_write_grant(g, "cluster-secret", err);
  ASSERT_FALSE(blob.empty()) << err;
  auto opened = open_write_grant(blob, "cluster-secret", now_ms(), err);
  ASSERT_TRUE(opened) << err;
  EXPECT_EQ(opened->oid, g.oid);
  EXPECT_EQ(opened->seq, g.seq);
  EXPECT_EQ(opened->acting_set.size(), 2u);

  EXPECT_FALSE(open_write_grant(blob, "wrong-key", now_ms(), err));
  auto j = nlohmann::json::parse(blob);
  j["seq"] = 99;
  EXPECT_FALSE(open_write_grant(j.dump(), "cluster-secret", now_ms(), err));

  g.expires_ms = now_ms() - 1;
  const auto stale = seal_write_grant(g, "cluster-secret", err);
  EXPECT_FALSE(open_write_grant(stale, "cluster-secret", now_ms(), err));
  EXPECT_TRUE(open_write_grant(stale, "cluster-secret", now_ms(), err, /*allow_expired=*/true));
}

TEST(ClientIo, DisabledByDefault) {
  DualStoreFixture fx("aios-cio-off");
  const auto body = std::string("abcdef");
  const auto crc = aios::crc32c(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
  auto r = fx.svc->api_client_prepare("o", body.size(), crc, {}, true, {});
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.code, "not_supported");
}

TEST(ClientIo, ReplicaPrepareInstallPublish) {
  DualStoreFixture fx("aios-cio-repl");
  fx.cfg.io_path = "client";
  fx.svc = std::make_unique<aios::ObjectService>(fx.cfg, fx.map, fx.stores);
  fx.svc->set_advertise("127.0.0.1:7400");

  const std::string oid = "cio/repl";
  const std::string body = "client-io-replica";
  const auto crc = aios::crc32c(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
  auto prep = fx.svc->api_client_prepare(oid, body.size(), crc, {{"k", "v"}}, true, {});
  ASSERT_TRUE(prep.ok) << prep.error;
  ASSERT_TRUE(prep.json_body);
  const std::string grant = prep.json_body->value("grant", "");
  ASSERT_FALSE(grant.empty());
  const auto acting = prep.json_body->at("acting_set");
  ASSERT_EQ(acting.size(), 2u);

  for (int i = 0; i < 2; ++i) {
    auto ins = fx.svc->api_client_install(oid, i, grant,
                                          reinterpret_cast<const std::uint8_t*>(body.data()),
                                          body.size(), {{"k", "v"}}, crc);
    ASSERT_TRUE(ins.ok) << ins.error << " shard=" << i;
  }
  auto pub = fx.svc->api_client_publish(oid, grant);
  ASSERT_TRUE(pub.ok) << pub.error;
  EXPECT_GE(pub.replicas, 2);

  auto got = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  ASSERT_TRUE(got.ok && got.data);
  EXPECT_EQ(std::string(got.data->begin(), got.data->end()), body);
}

TEST(ClientIo, EcPrepareInstallPublish) {
  EcFixture fx("aios-cio-ec");
  fx.cfg.io_path = "client";
  fx.svc = std::make_unique<aios::ObjectService>(fx.cfg, fx.map, fx.stores);
  fx.svc->set_advertise("127.0.0.1:7400");

  const std::string oid = "cio/ec";
  const std::string body = "client-io-erasure";
  const auto crc = aios::crc32c(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
  aios::LayoutRequest lr;
  lr.layout = "ec";
  auto prep = fx.svc->api_client_prepare(oid, body.size(), crc, {}, true, {}, lr);
  ASSERT_TRUE(prep.ok) << prep.error;
  const std::string grant = prep.json_body->value("grant", "");
  const int k = prep.json_body->value("ec_k", 0);
  const int m = prep.json_body->value("ec_m", 0);
  const std::string codec = prep.json_body->value("ec_codec", "");
  std::string err;
  auto ec = aios::make_erasure_codec(k, m, codec, err);
  ASSERT_TRUE(ec) << err;
  std::vector<std::vector<std::uint8_t>> shards;
  ASSERT_TRUE(ec->encode(std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(body.data()), body.size()),
                         shards, err))
      << err;
  ASSERT_EQ(shards.size(), static_cast<std::size_t>(k + m));
  for (int i = 0; i < k + m; ++i) {
    auto ins = fx.svc->api_client_install(oid, i, grant, shards[static_cast<std::size_t>(i)].data(),
                                          shards[static_cast<std::size_t>(i)].size(), {});
    ASSERT_TRUE(ins.ok) << ins.error << " shard=" << i;
  }
  auto pub = fx.svc->api_client_publish(oid, grant);
  ASSERT_TRUE(pub.ok) << pub.error;
  auto got = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  ASSERT_TRUE(got.ok && got.data) << got.error;
  EXPECT_EQ(std::string(got.data->begin(), got.data->end()), body);
}

TEST(ClientIo, AbortLeavesNoTip) {
  DualStoreFixture fx("aios-cio-abort");
  fx.cfg.io_path = "client";
  fx.svc = std::make_unique<aios::ObjectService>(fx.cfg, fx.map, fx.stores);
  fx.svc->set_advertise("127.0.0.1:7400");
  const std::string oid = "cio/abort";
  const std::string body = "nope";
  const auto crc = aios::crc32c(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
  auto prep = fx.svc->api_client_prepare(oid, body.size(), crc, {}, true, {});
  ASSERT_TRUE(prep.ok) << prep.error;
  const std::string grant = prep.json_body->value("grant", "");
  auto ins = fx.svc->api_client_install(oid, 0, grant,
                                        reinterpret_cast<const std::uint8_t*>(body.data()),
                                        body.size(), {}, crc);
  ASSERT_TRUE(ins.ok) << ins.error;
  auto ab = fx.svc->api_client_abort(oid, grant);
  ASSERT_TRUE(ab.ok) << ab.error;
  auto got = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  EXPECT_FALSE(got.ok);
}

TEST(ClientIo, SessionHttpReplica) {
  DualStoreFixture fx("aios-cio-http");
  fx.cfg.io_path = "client";
  fx.svc = std::make_unique<aios::ObjectService>(fx.cfg, fx.map, fx.stores);
  fx.svc->set_advertise("127.0.0.1:7400");
  const int port_num = 18100 + static_cast<int>(::getpid() % 800);
  const std::string port = std::to_string(port_num);
  fx.cfg.http_listen = "127.0.0.1:" + port;
  fx.cfg.http_workers = 8;

  boost::asio::io_context ioc;
  aios::HttpServer http(ioc, fx.cfg, *fx.svc, fx.membership);
  http.start();
  std::thread th([&] { ioc.run(); });

  aios::SessionConfig sc;
  sc.endpoint = "127.0.0.1:" + port;
  sc.cluster_key = fx.cfg.cluster_key;
  sc.io_path = "client";
  {
    aios::Session session(sc);
    const std::string oid = "cio/http-repl";
    const std::string body = "session-client-io";
    EXPECT_NO_THROW(session.put_bytes(oid, body));
    auto snap = session.get_object(oid);
    EXPECT_TRUE(snap.exists);
    EXPECT_EQ(snap.body, body);
  }

  http.close_sessions();
  ioc.stop();
  th.join();
}
