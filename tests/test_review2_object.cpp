// Regression tests for the 2026-09-02 code review (object slice).
#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include "cluster/place.hpp"
#include "config.hpp"
#include "net/framing.hpp"
#include "net/object_client.hpp"
#include "net/server.hpp"
#include "object/object_service.hpp"
#include "object/pubsub.hpp"
#include "object/repair.hpp"
#include "store/object_store.hpp"
#include "util/auth.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using tcp = boost::asio::ip::tcp;
using aios::test::DualStoreFixture;
using aios::test::temp_root;

const std::string kKey = "550e8400-e29b-41d4-a716-446655440000";

std::atomic<int> g_r2_seq{0};

std::vector<std::uint8_t> payload(std::size_t n, std::uint8_t seed = 0x11) {
  std::vector<std::uint8_t> out(n);
  for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>(seed + (i % 241));
  return out;
}

int pick_port() {
  static std::atomic<int> next{21000 + static_cast<int>(::getpid() % 300) * 10};
  return next.fetch_add(1);
}

// One ObjectService behind a real TcpServer (in-process "node").
struct RpcNode {
  std::string id;
  std::string addr;
  std::string aios_path;
  aios::Config cfg;
  aios::LocalStores stores;
  std::unique_ptr<aios::ObjectService> svc;
  std::unique_ptr<aios::TcpServer> server;
  boost::asio::io_context ioc;
  std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work;
  std::thread thr;
  bool running{false};
  int pre_hello_timeout_ms{5000};

  void start(aios::ClusterMap& map) {
    if (running) return;
    ioc.restart();
    aios::ObjectStoreOptions opts;
    opts.shard_count = 4;
    opts.clone_required = false;
    opts.max_versions = 16;
    stores.sync_paths({aios_path}, opts);
    svc = std::make_unique<aios::ObjectService>(cfg, map, stores);
    svc->set_advertise(addr);

    std::string host, port;
    ASSERT_TRUE(aios::split_host_port(addr, host, port));
    aios::RpcHandlers handlers;
    handlers.local_node_id = id;
    handlers.local_listen = addr;
    handlers.cluster_key = cfg.cluster_key;
    handlers.auth_skew_ms = cfg.auth_skew_ms;
    handlers.pre_hello_timeout_ms = pre_hello_timeout_ms;
    handlers.on_object = [this](const aios::Frame& req) { return svc->handle(req); };
    server = std::make_unique<aios::TcpServer>(ioc, host, port, std::move(handlers));
    server->start();
    work = std::make_unique<
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(ioc));
    thr = std::thread([this] {
      try {
        ioc.run();
      } catch (...) {
      }
    });
    running = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  void stop() {
    if (!running) return;
    aios::object_rpc_pool_clear();
    if (server) server->close();
    aios::object_rpc_pool_clear();
    work.reset();
    ioc.stop();
    if (thr.joinable()) thr.join();
    server.reset();
    svc.reset();
    running = false;
  }
};

// n single-target nodes on loopback, replica_count copies per object.
struct Cluster {
  std::filesystem::path root;
  aios::ClusterMap map;
  std::vector<std::unique_ptr<RpcNode>> nodes;

  Cluster(int n, int replica_count, int write_quorum, int pre_hello_timeout_ms = 5000) {
    const int seq = g_r2_seq.fetch_add(1);
    root = temp_root(("aios-review2-object-" + std::to_string(seq)).c_str());
    aios::MembershipTable membership;
    aios::FsTable fs_table;
    std::vector<aios::FsEntry> remotes;
    for (int i = 0; i < n; ++i) {
      auto node = std::make_unique<RpcNode>();
      node->id = "node-" + std::string(1, static_cast<char>('a' + i));
      node->addr = "127.0.0.1:" + std::to_string(pick_port());
      node->pre_hello_timeout_ms = pre_hello_timeout_ms;
      const auto path = root / node->id / "aios";
      std::filesystem::create_directories(path);
      node->aios_path = path.string();
      node->cfg.node_id = node->id;
      node->cfg.listen = node->addr;
      node->cfg.cluster_key = kKey;
      node->cfg.replica_count = replica_count;
      node->cfg.write_quorum = write_quorum;
      node->cfg.auth_skew_ms = 60000;
      node->cfg.clone_required = false;
      node->cfg.max_versions = 16;
      if (i == 0) {
        membership.set_local(node->id, node->addr);
        fs_table.set_local(node->id, {aios::test::make_target(node->aios_path)});
      } else {
        membership.mark_alive(node->id, node->addr, aios::now_ms());
        aios::FsEntry e;
        e.node_id = node->id;
        e.mount = path.string();
        e.target_path = path.string();
        e.aios_path = node->aios_path;
        e.storage_class = "nvme";
        e.weight = 1;
        e.bavail = 1000;
        e.usable = true;
        e.updated_ms = aios::now_ms();
        remotes.push_back(e);
      }
      nodes.push_back(std::move(node));
    }
    fs_table.merge(remotes);
    map = aios::ClusterMap::build(membership, fs_table, replica_count, aios::PlacementConfig{});
    for (auto& node : nodes) node->start(map);
  }

  ~Cluster() {
    for (auto& node : nodes) node->stop();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  RpcNode* by_id(const std::string& id) {
    for (auto& n : nodes) {
      if (n->id == id) return n.get();
    }
    return nullptr;
  }

  RpcNode* primary_for(const std::string& oid) {
    auto p = aios::place(oid, map, "nvme");
    return p.acting_set.empty() ? nullptr : by_id(p.acting_set[0].node_id);
  }

  // First oid with the given prefix whose primary is `node`.
  std::string oid_with_primary(const std::string& prefix, const RpcNode* node) {
    for (int i = 0; i < 10000; ++i) {
      const std::string oid = prefix + std::to_string(i);
      if (primary_for(oid) == node) return oid;
    }
    return {};
  }
};

// Raw client socket to a TcpServer.
struct RawConn {
  boost::asio::io_context ioc;
  tcp::socket sock{ioc};
  bool connect(const std::string& addr) {
    std::string host, port;
    if (!aios::split_host_port(addr, host, port)) return false;
    boost::system::error_code ec;
    tcp::resolver res(ioc);
    auto eps = res.resolve(host, port, ec);
    if (ec) return false;
    boost::asio::connect(sock, eps, ec);
    return !ec;
  }
};

// Replica-role ObjectPut frame handed straight to ObjectService::handle.
aios::Frame replica_put(const std::string& aios_path, const std::string& oid, std::uint64_t seq,
                        const std::vector<std::uint8_t>& data, std::uint64_t epoch) {
  aios::Frame f;
  f.type = aios::MsgType::ObjectPut;
  f.body = {{"oid", oid},
            {"aios_path", aios_path},
            {"role", "replica"},
            {"seq", seq},
            {"base_seq", seq - 1},
            {"size", data.size()},
            {"crc32c", aios::crc32c(data.data(), data.size())},
            {"inline_body", false},
            {"epoch", epoch}};
  f.flags = aios::kFlagRawBody;
  f.raw = data;
  return f;
}

}  // namespace

// ---- OBJ-1 / STO-7: auth --------------------------------------------------------

TEST(Review2Object, AuthVerifyRejectsWrongTypedFields) {
  using namespace aios;
  std::string err;
  nlohmann::json good = {{"node_id", "x"}};
  auth_sign(good, MsgType::Hello, kKey);
  ASSERT_TRUE(auth_verify(good, MsgType::Hello, kKey, 60000, err)) << err;

  auto with = [&](const char* k, nlohmann::json v) {
    auto b = good;
    b[k] = std::move(v);
    return b;
  };
  EXPECT_NO_THROW({
    EXPECT_FALSE(auth_verify(with("sig", 0), MsgType::Hello, kKey, 60000, err));
    EXPECT_FALSE(auth_verify(with("ts", "x"), MsgType::Hello, kKey, 60000, err));
    EXPECT_FALSE(auth_verify(with("nonce", 1), MsgType::Hello, kKey, 60000, err));
    EXPECT_FALSE(auth_verify(with("nonce", nullptr), MsgType::Hello, kKey, 60000, err));
    EXPECT_FALSE(auth_verify(nlohmann::json::array(), MsgType::Hello, kKey, 60000, err));
    EXPECT_FALSE(auth_verify(nlohmann::json(nullptr), MsgType::Hello, kKey, 60000, err));
  });
}

TEST(Review2Object, ReplayRejectedAfterManyMessagesWithinWindow) {
  using namespace aios;
  std::string err;
  nlohmann::json first = {{"probe", "replay"}};
  auth_sign(first, MsgType::Ping, kKey);
  ASSERT_TRUE(auth_verify(first, MsgType::Ping, kKey, 60000, err)) << err;
  // Old cache was 4096 entries FIFO; push well past that inside the skew window.
  for (int i = 0; i < 6000; ++i) {
    nlohmann::json b = {{"i", i}};
    auth_sign(b, MsgType::Ping, kKey);
    ASSERT_TRUE(auth_verify(b, MsgType::Ping, kKey, 60000, err)) << err;
  }
  EXPECT_FALSE(auth_verify(first, MsgType::Ping, kKey, 60000, err));
  EXPECT_NE(err.find("replay"), std::string::npos) << err;
}

// ---- OBJ-1: malformed frames must not take the server down ----------------------

TEST(Review2Object, TcpServerSurvivesMalformedHelloAndTypedFields) {
  using namespace aios;
  Cluster c(1, 1, 1);
  auto* n = c.nodes[0].get();
  const std::string oid = "r2/alive";
  const auto data = payload(64);
  ASSERT_TRUE(n->svc->api_put(oid, data.data(), data.size(), {}, true, {}).ok);

  auto poke = [&](nlohmann::json hello) {
    RawConn rc;
    ASSERT_TRUE(rc.connect(n->addr));
    Frame f;
    f.type = MsgType::Hello;
    f.body = std::move(hello);
    std::string err;
    boost::system::error_code ec;
    ASSERT_TRUE(write_frame(rc.sock, f, err, ec, 2000)) << err;
    Frame reply;
    // Either an Error frame or EOF; must not hang.
    (void)read_frame(rc.sock, reply, err, ec, 5000);
  };
  {
    nlohmann::json h = {{"node_id", "evil"}, {"listen", "127.0.0.1:1"}};
    auth_sign(h, MsgType::Hello, kKey);
    h["sig"] = 0;
    poke(h);
  }
  {
    nlohmann::json h = {{"node_id", "evil"}, {"listen", "127.0.0.1:1"}};
    auth_sign(h, MsgType::Hello, kKey);
    h["ts"] = "x";
    poke(h);
  }
  {
    nlohmann::json h = {{"node_id", 7}, {"listen", nlohmann::json::array()}};
    auth_sign(h, MsgType::Hello, kKey);
    poke(h);
  }

  // Server is still serving authenticated RPC.
  auto st = object_stat_remote(n->addr, "client", "127.0.0.1:1", kKey, 60000, c.map.epoch,
                               n->aios_path, oid);
  ASSERT_TRUE(st.ok) << st.error;
  EXPECT_EQ(st.size, data.size());

  // Wrong-typed scalar in an authenticated request → bad_request, not a crash.
  nlohmann::json body = {{"oid", oid},        {"aios_path", n->aios_path}, {"role", "replica"},
                         {"seq", "1"},        {"size", 3},                 {"epoch", c.map.epoch}};
  auto r = object_rpc(n->addr, "client", "127.0.0.1:1", kKey, 60000, MsgType::ObjectPut, body,
                      std::vector<std::uint8_t>{'a', 'b', 'c'});
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.code, "bad_request") << r.error;

  auto st2 = object_stat_remote(n->addr, "client", "127.0.0.1:1", kKey, 60000, c.map.epoch,
                                n->aios_path, oid);
  EXPECT_TRUE(st2.ok) << st2.error;
}

TEST(Review2Object, HandleReturnsBadRequestOnWrongTypes) {
  DualStoreFixture fx("aios-r2-badreq");
  aios::Frame f;
  f.type = aios::MsgType::ObjectGet;
  f.body = {{"oid", 12}, {"epoch", fx.map.epoch}};
  auto r = fx.svc->handle(f);
  EXPECT_FALSE(r.body.value("ok", true));
  EXPECT_EQ(r.body.value("code", ""), "bad_request");

  f.type = aios::MsgType::ObjectStat;
  f.body = {{"oid", "x"}, {"seq", "1"}, {"epoch", fx.map.epoch}};
  r = fx.svc->handle(f);
  EXPECT_EQ(r.body.value("code", ""), "bad_request");

  f.body = nlohmann::json::array();
  r = fx.svc->handle(f);
  EXPECT_FALSE(r.body.value("ok", true));
}

// ---- OBJ-2: fs_path from the wire is ignored ------------------------------------

TEST(Review2Object, ReplicaPutIgnoresWireFsPath) {
  DualStoreFixture fx("aios-r2-fspath");
  const std::string oid = "r2/fspath";
  auto p = aios::place(oid, fx.map, "nvme");
  ASSERT_GE(p.acting_set.size(), 2u);
  const auto& target = p.acting_set[1].aios_path;
  const auto data = payload(200 * 1024);
  auto f = replica_put(target, oid, 1, data, fx.map.epoch);
  f.body["fs_path"] = "../../../../tmp/aios-r2-escaped";
  auto r = fx.svc->handle(f);
  ASSERT_TRUE(r.body.value("ok", false)) << r.body.dump();

  auto* store = fx.stores.get(target);
  ASSERT_NE(store, nullptr);
  std::string err;
  auto info = store->stat(oid, 1, err);
  ASSERT_TRUE(info.has_value()) << err;
  EXPECT_EQ(info->size, data.size());
  EXPECT_EQ(info->fs_path.find(".."), std::string::npos) << info->fs_path;
  auto body_path = store->fs_body_path(oid, 1, err);
  ASSERT_TRUE(body_path.has_value()) << err;
  const auto canon = std::filesystem::weakly_canonical(*body_path);
  const auto root = std::filesystem::weakly_canonical(target);
  EXPECT_EQ(canon.string().rfind(root.string(), 0), 0u) << canon << " not under " << root;
  EXPECT_TRUE(std::filesystem::exists(canon));
  EXPECT_FALSE(std::filesystem::exists("/tmp/aios-r2-escaped"));
}

// ---- OBJ-13: raw trailer bound to the signed envelope ---------------------------

TEST(Review2Object, RawBodySha256Enforced) {
  using namespace aios;
  DualStoreFixture fx("aios-r2-sha");
  const std::string oid = "r2/sha";
  auto p = place(oid, fx.map, "nvme");
  const auto& target = p.acting_set[1].aios_path;
  const auto data = payload(4096);

  auto make = [&](std::uint64_t seq) {
    auto f = replica_put(target, oid, seq, data, fx.map.epoch);
    return f;
  };
  // Unsigned in-process frame without sha256: compatibility path, accepted.
  {
    auto r = fx.svc->handle(make(1));
    EXPECT_TRUE(r.body.value("ok", false)) << r.body.dump();
  }
  // Signed frame without sha256 → rejected.
  {
    auto f = make(2);
    auth_sign(f.body, MsgType::ObjectPut, kKey);
    auto r = fx.svc->handle(f);
    EXPECT_FALSE(r.body.value("ok", true));
    EXPECT_EQ(r.body.value("code", ""), "bad_request");
  }
  // Tampered trailer with a sha256 for the original bytes → rejected.
  {
    auto f = make(2);
    f.body["sha256"] = sha256_hex(std::string(data.begin(), data.end()));
    auth_sign(f.body, MsgType::ObjectPut, kKey);
    f.raw[10] ^= 0xff;
    auto r = fx.svc->handle(f);
    EXPECT_FALSE(r.body.value("ok", true));
    EXPECT_EQ(r.body.value("code", ""), "bad_request");
  }
  // Correct sha256 → accepted.
  {
    auto f = make(2);
    f.body["sha256"] = sha256_hex(std::string(data.begin(), data.end()));
    auth_sign(f.body, MsgType::ObjectPut, kKey);
    auto r = fx.svc->handle(f);
    EXPECT_TRUE(r.body.value("ok", false)) << r.body.dump();
  }
}

TEST(Review2Object, ClientAddsSha256ToRawFrames) {
  using namespace aios;
  Cluster c(1, 1, 1);
  auto* n = c.nodes[0].get();
  const std::string oid = "r2/client-sha";
  const auto data = payload(1000);
  PreparedVersion v;
  v.oid = oid;
  v.seq = 1;
  v.size = data.size();
  v.crc32c = crc32c(data.data(), data.size());
  auto r = object_install_remote(n->addr, "client", "127.0.0.1:1", kKey, 60000, c.map.epoch,
                                 n->aios_path, v, data.data(), data.size(), {});
  ASSERT_TRUE(r.ok) << r.error;
  std::string err;
  auto st = n->stores.get(n->aios_path)->stat(oid, 1, err);
  ASSERT_TRUE(st.has_value()) << err;
  EXPECT_EQ(st->size, data.size());
}

// ---- OBJ-5: timeouts -------------------------------------------------------------

TEST(Review2Object, ServerClosesSilentConnectionBeforeHello) {
  Cluster c(1, 1, 1, /*pre_hello_timeout_ms=*/500);
  RawConn rc;
  ASSERT_TRUE(rc.connect(c.nodes[0]->addr));
  const auto t0 = std::chrono::steady_clock::now();
  aios::Frame f;
  std::string err;
  boost::system::error_code ec;
  const bool got = aios::read_frame(rc.sock, f, err, ec, 10000);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  EXPECT_FALSE(got);
  EXPECT_LT(ms, 5000) << "server did not close idle pre-Hello socket";
}

TEST(Review2Object, ClientRpcTimesOutAgainstSilentPeer) {
  using namespace aios;
  boost::asio::io_context ioc;
  tcp::acceptor acc(ioc, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
  const auto port = acc.local_endpoint().port();
  std::atomic<bool> stop{false};
  std::vector<std::shared_ptr<tcp::socket>> held;
  std::thread accepter([&] {
    while (true) {
      auto s = std::make_shared<tcp::socket>(ioc);
      boost::system::error_code ec;
      acc.accept(*s, ec);
      if (ec || stop) break;
      held.push_back(s);  // accept, never reply
    }
  });
  object_rpc_set_timeout_ms(700);
  const auto t0 = std::chrono::steady_clock::now();
  auto r = object_stat_remote("127.0.0.1:" + std::to_string(port), "client", "127.0.0.1:1",
                              kKey, 60000, 1, "/x", "oid");
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  object_rpc_set_timeout_ms(0);
  EXPECT_FALSE(r.ok);
  EXPECT_LT(ms, 10000) << "rpc did not time out";
  stop = true;
  boost::system::error_code ec;
  {
    // Wake the blocking accept so the thread observes `stop`.
    tcp::socket wake(ioc);
    wake.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port), ec);
  }
  accepter.join();
  acc.close(ec);
  for (auto& s : held) s->close(ec);
  object_rpc_pool_clear();
}

// ---- OBJ-6 / OBJ-11 / OBJ-14: stage sessions ------------------------------------

namespace {

aios::Frame stage_begin(const std::string& path, const std::string& oid, std::uint64_t seq,
                        std::uint64_t epoch) {
  aios::Frame f;
  f.type = aios::MsgType::ObjectStageBegin;
  f.body = {{"oid", oid}, {"aios_path", path}, {"seq", seq}, {"epoch", epoch}};
  return f;
}

aios::Frame stage_data(const std::string& path, const std::string& oid, std::uint64_t seq,
                       std::uint64_t offset, const std::vector<std::uint8_t>& chunk,
                       std::uint64_t epoch) {
  aios::Frame f;
  f.type = aios::MsgType::ObjectStageData;
  f.body = {{"oid", oid}, {"aios_path", path}, {"seq", seq}, {"offset", offset}, {"epoch", epoch}};
  f.flags = aios::kFlagRawBody;
  f.raw = chunk;
  return f;
}

}  // namespace

TEST(Review2Object, StageBeginSupersedesSessionAndDataStaysConsistent) {
  DualStoreFixture fx("aios-r2-stage");
  const std::string oid = "r2/stage";
  auto p = aios::place(oid, fx.map, "nvme");
  const auto& target = p.acting_set[1].aios_path;
  const auto chunk = payload(1000);
  const auto e = fx.map.epoch;

  ASSERT_TRUE(fx.svc->handle(stage_begin(target, oid, 1, e)).body.value("ok", false));
  ASSERT_TRUE(fx.svc->handle(stage_data(target, oid, 1, 0, chunk, e)).body.value("ok", false));
  // Second begin invalidates the first session: its offset progress is gone.
  ASSERT_TRUE(fx.svc->handle(stage_begin(target, oid, 1, e)).body.value("ok", false));
  auto stale = fx.svc->handle(stage_data(target, oid, 1, 1000, chunk, e));
  EXPECT_FALSE(stale.body.value("ok", true));
  EXPECT_TRUE(fx.svc->handle(stage_data(target, oid, 1, 0, chunk, e)).body.value("ok", false));

  // Hammer begin/data on the same key from several threads: no crash, no
  // use-after-close, and every accepted chunk lands at the offset it claimed.
  std::atomic<int> accepted{0};
  std::vector<std::thread> ths;
  for (int t = 0; t < 6; ++t) {
    ths.emplace_back([&, t] {
      for (int i = 0; i < 150; ++i) {
        if (t % 2 == 0) {
          fx.svc->handle(stage_begin(target, oid, 2, e));
        } else {
          auto r = fx.svc->handle(stage_data(target, oid, 2, 0, chunk, e));
          if (r.body.value("ok", false)) accepted.fetch_add(1);
        }
      }
    });
  }
  for (auto& th : ths) th.join();
  EXPECT_GT(accepted.load(), 0);

  // Not in the acting set → not_replica (OBJ-14).
  auto other = stage_begin(target, oid, 3, e);
  other.body["aios_path"] = "/definitely/not/a/target";
  auto nr = fx.svc->handle(other);
  EXPECT_FALSE(nr.body.value("ok", true));
}

TEST(Review2Object, AbortVersionClosesStageSessionAndRemovesFile) {
  DualStoreFixture fx("aios-r2-stage-abort");
  const std::string oid = "r2/stage-abort";
  auto p = aios::place(oid, fx.map, "nvme");
  const auto& target = p.acting_set[1].aios_path;
  const auto chunk = payload(3000);
  const auto e = fx.map.epoch;

  ASSERT_TRUE(fx.svc->handle(stage_begin(target, oid, 5, e)).body.value("ok", false));
  ASSERT_TRUE(fx.svc->handle(stage_data(target, oid, 5, 0, chunk, e)).body.value("ok", false));
  std::string path, err;
  ASSERT_TRUE(fx.stores.get(target)->stage_path_for(oid, 5, path, err)) << err;
  ASSERT_TRUE(std::filesystem::exists(path));

  aios::Frame ab;
  ab.type = aios::MsgType::ObjectAbortVersion;
  ab.body = {{"oid", oid}, {"aios_path", target}, {"seq", 5}, {"role", "replica"}, {"epoch", e}};
  auto r = fx.svc->handle(ab);
  EXPECT_TRUE(r.body.value("ok", false)) << r.body.dump();
  EXPECT_FALSE(std::filesystem::exists(path));
  // Session is gone: further data is rejected instead of writing to a closed fd.
  auto after = fx.svc->handle(stage_data(target, oid, 5, 3000, chunk, e));
  EXPECT_FALSE(after.body.value("ok", true));
}

// ---- OBJ-8: pipeline holds the per-oid guard ------------------------------------

TEST(Review2Object, PipelineAbortDoesNotDropConcurrentPut) {
  DualStoreFixture fx("aios-r2-pipe");
  const std::string oid = "r2/pipe";
  std::string staging;
  auto b = fx.svc->api_begin_put_pipeline(oid, {}, 4096, staging);
  ASSERT_TRUE(b.ok) << b.error;

  const auto data = payload(512, 0x42);
  auto put_fut = std::async(std::launch::async, [&] {
    return fx.svc->api_put(oid, data.data(), data.size(), {}, true, {});
  });
  // The PUT must wait for the pipeline (guard held), not race its seq.
  EXPECT_EQ(put_fut.wait_for(std::chrono::milliseconds(300)), std::future_status::timeout);
  ASSERT_TRUE(fx.svc->api_put_pipeline_abort(oid).ok);
  ASSERT_EQ(put_fut.wait_for(std::chrono::seconds(20)), std::future_status::ready);
  auto put = put_fut.get();
  ASSERT_TRUE(put.ok) << put.error;

  auto p = aios::place(oid, fx.map, "nvme");
  for (const auto& t : p.acting_set) {
    std::string err;
    auto got = fx.stores.get(t.aios_path)->get(oid, err);
    ASSERT_TRUE(got.has_value()) << t.aios_path << ": " << err;
    EXPECT_EQ(*got, data);
  }
}

// ---- OBJ-10: multi-store LIST -----------------------------------------------------

TEST(Review2Object, LocalListMergesStoresAndClampsLimit) {
  DualStoreFixture fx("aios-r2-list", /*replica_count=*/1, /*write_quorum=*/1);
  std::set<std::string> expect;
  for (int i = 0; i < 24; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "r2list/%02d", i);
    const std::string oid = buf;
    const auto d = payload(8, static_cast<std::uint8_t>(i));
    ASSERT_TRUE(fx.svc->api_put(oid, d.data(), d.size(), {}, true, {}).ok);
    expect.insert(oid);
  }
  // Interleaved across both stores?
  std::string err;
  const auto n1 = fx.stores.get(fx.p1)->list("r2list/", "", "", 100, "", false, err).objects.size();
  const auto n2 = fx.stores.get(fx.p2)->list("r2list/", "", "", 100, "", false, err).objects.size();
  ASSERT_GT(n1, 0u);
  ASSERT_GT(n2, 0u);
  ASSERT_EQ(n1 + n2, expect.size());

  std::vector<std::string> seen;
  std::string cursor;
  for (int page = 0; page < 100; ++page) {
    auto r = fx.svc->api_list("r2list/", "", "", 2, cursor, false, false);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_LE(r.list.objects.size(), 2u);
    // Each page is oid-sorted; the store itself pages shard-major, so global
    // order across pages is not a property of the API.
    std::vector<std::string> page_oids;
    for (const auto& o : r.list.objects) page_oids.push_back(o.oid);
    EXPECT_TRUE(std::is_sorted(page_oids.begin(), page_oids.end()));
    for (auto& o : page_oids) seen.push_back(std::move(o));
    if (r.list.next_cursor.empty()) break;
    cursor = r.list.next_cursor;
  }
  EXPECT_EQ(std::set<std::string>(seen.begin(), seen.end()), expect);
  EXPECT_EQ(seen.size(), expect.size()) << "duplicates across pages";

  auto all = fx.svc->api_list("r2list/", "", "", 0, "", false, false);
  ASSERT_TRUE(all.ok);
  EXPECT_EQ(all.list.objects.size(), expect.size());
  EXPECT_TRUE(all.list.next_cursor.empty());
}

// ---- OBJ-12: bounded durable catch-up ---------------------------------------------

TEST(Review2Object, DurableSubscribeCatchupIsBounded) {
  DualStoreFixture fx("aios-r2-pubsub");
  const std::string topic = "r2-bounded";
  ASSERT_TRUE(fx.svc->api_pubsub_create(topic, aios::DeliveryMode::Durable).ok);
  const std::uint8_t msg[] = {'m'};
  for (int i = 0; i < 1005; ++i) {
    auto r = fx.svc->api_pubsub_publish(topic, msg, 1, "text/plain");
    ASSERT_TRUE(r.ok) << r.error;
  }
  auto s = fx.svc->api_pubsub_subscribe(topic, 0, true, 0);
  ASSERT_TRUE(s.ok) << s.error;
  EXPECT_EQ(s.pub_messages.size(), 1000u);
  ASSERT_TRUE(s.json_body.has_value());
  EXPECT_EQ(s.json_body->value("next_after_id", 0), 1000);
  EXPECT_TRUE(s.json_body->value("more", false));
  auto rest = fx.svc->api_pubsub_subscribe(topic, 1000, true, 0);
  ASSERT_TRUE(rest.ok) << rest.error;
  EXPECT_EQ(rest.pub_messages.size(), 5u);
  EXPECT_FALSE(rest.json_body->value("more", true));
}

// ---- OBJ-3 / OBJ-14: cross-node txns do not deadlock ----------------------------

TEST(Review2Object, ConcurrentCrossNodeTxnsComplete) {
  using namespace aios;
  Cluster c(2, /*replica_count=*/2, /*write_quorum=*/2);
  auto* a = c.nodes[0].get();
  auto* b = c.nodes[1].get();
  const auto data = payload(256);

  std::atomic<int> txn_no{0};
  auto run_txn = [&](RpcNode* coord, RpcNode* other) -> std::string {
    ApiResult tb = coord->svc->api_txn_begin();
    if (!tb.ok) return "begin: " + tb.error;
    const auto txn_id = tb.attrs.at("txn_id");
    // Distinct oids per txn: two txns preparing the same oid conflict by design.
    const std::string tag = "r2/txn-" + std::to_string(txn_no.fetch_add(1)) + "-";
    for (int i = 0; i < 4; ++i) {
      const auto oid_remote =
          c.oid_with_primary(tag + "remote-" + std::to_string(i) + "-", other);
      const auto oid_local = c.oid_with_primary(tag + "local-" + std::to_string(i) + "-", coord);
      auto p1 = coord->svc->api_txn_prepare_put(txn_id, oid_remote, data.data(), data.size(),
                                                {}, {});
      if (!p1.ok) return "prepare remote: " + p1.error;
      auto p2 = coord->svc->api_txn_prepare_put(txn_id, oid_local, data.data(), data.size(),
                                                {}, {});
      if (!p2.ok) return "prepare local: " + p2.error;
    }
    auto cm = coord->svc->api_txn_commit(txn_id);
    if (!cm.ok) return "commit: " + cm.error;
    return {};
  };

  std::vector<std::future<std::string>> futs;
  for (int round = 0; round < 3; ++round) {
    futs.push_back(std::async(std::launch::async, run_txn, a, b));
    futs.push_back(std::async(std::launch::async, run_txn, b, a));
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  for (auto& f : futs) {
    ASSERT_EQ(f.wait_until(deadline), std::future_status::ready) << "txn deadlocked";
    EXPECT_EQ(f.get(), "");
  }
}

// ---- OBJ-4: repair honors delete markers -----------------------------------------

TEST(Review2Object, RepairPropagatesDeleteMarkerInsteadOfResurrecting) {
  using namespace aios;
  Cluster c(2, /*replica_count=*/2, /*write_quorum=*/1);
  auto* a = c.nodes[0].get();
  auto* b = c.nodes[1].get();
  const auto oid = c.oid_with_primary("r2/repair-del-", a);
  ASSERT_FALSE(oid.empty());
  const auto data = payload(700);
  ASSERT_TRUE(a->svc->api_put(oid, data.data(), data.size(), {}, true, {}).ok);
  {
    std::string err;
    ASSERT_TRUE(b->stores.get(b->aios_path)->stat(oid, err).has_value());
  }

  // Delete while the replica is down; only the primary gets the marker.
  b->stop();
  auto del = a->svc->api_del(oid, {});
  ASSERT_TRUE(del.ok) << del.error;
  b->start(c.map);
  {
    std::string err;
    EXPECT_FALSE(a->stores.get(a->aios_path)->stat(oid, err).has_value());
    EXPECT_TRUE(b->stores.get(b->aios_path)->stat(oid, err).has_value()) << "stale live copy";
  }

  // Repair from both sides: neither may resurrect the object.
  run_repair(b->cfg, b->addr, c.map, b->stores, 1000);
  run_repair(a->cfg, a->addr, c.map, a->stores, 1000);
  for (auto* n : {a, b}) {
    std::string err;
    EXPECT_FALSE(n->stores.get(n->aios_path)->stat(oid, err).has_value())
        << n->id << " resurrected";
    std::uint64_t tip = 0;
    ASSERT_TRUE(n->stores.get(n->aios_path)->tip_seq(oid, tip, err));
    EXPECT_EQ(tip, 2u) << n->id;
    auto v = n->stores.get(n->aios_path)->stat(oid, tip, err);
    ASSERT_TRUE(v.has_value()) << n->id;
    EXPECT_TRUE(v->is_delete) << n->id;
  }
  EXPECT_FALSE(a->svc->api_get(oid, std::nullopt, std::nullopt, {}).ok);
  EXPECT_FALSE(b->svc->api_get(oid, std::nullopt, std::nullopt, {}).ok);
}

// ---- OBJ-7: repair temp file named by hash --------------------------------------

TEST(Review2Object, RepairRemoteSourceWithTraversalOidStaysInTemp) {
  using namespace aios;
  Cluster c(3, /*replica_count=*/3, /*write_quorum=*/1);
  // Traversal-looking oid; primary must be node-a so node-a's repair runs.
  std::string oid;
  for (int i = 0; i < 10000 && oid.empty(); ++i) {
    const std::string cand = "a/../../r2-evil-" + std::to_string(i) + "/../x";
    if (c.primary_for(cand) == c.nodes[0].get()) oid = cand;
  }
  ASSERT_FALSE(oid.empty());
  auto* a = c.nodes[0].get();
  auto p = place(oid, c.map, "nvme");
  ASSERT_EQ(p.acting_set.size(), 3u);
  auto* holder = c.by_id(p.acting_set[1].node_id);
  ASSERT_NE(holder, nullptr);

  const auto v1 = payload(300 * 1024, 1);
  ASSERT_TRUE(a->svc->api_put(oid, v1.data(), v1.size(), {}, true, {}).ok);

  // A newer version exists only on one replica → repair on the primary must pull
  // it over the network (remote source, > 256 KiB → temp-file path).
  const auto v2 = payload(320 * 1024, 2);
  {
    auto* store = holder->stores.get(holder->aios_path);
    PreparedVersion pv;
    pv.oid = oid;
    pv.seq = 2;
    pv.prev_tip = 1;
    pv.size = v2.size();
    pv.crc32c = crc32c(v2.data(), v2.size());
    std::string err;
    ASSERT_TRUE(store->install_version(pv, v2.data(), v2.size(), {}, err)) << err;
    ASSERT_TRUE(store->publish_tip(oid, 2, err)) << err;
  }

  const auto tmp = std::filesystem::temp_directory_path();
  auto stats = run_repair(a->cfg, a->addr, c.map, a->stores, 1000);
  EXPECT_GE(stats.repaired, 1u) << "failed=" << stats.failed;

  for (const auto& t : p.acting_set) {
    auto* n = c.by_id(t.node_id);
    std::string err;
    auto got = n->stores.get(n->aios_path)->get(oid, err);
    ASSERT_TRUE(got.has_value()) << n->id << ": " << err;
    EXPECT_EQ(got->size(), v2.size()) << n->id;
    EXPECT_EQ(*got, v2) << n->id;
  }
  // Nothing escaped or lingered.
  std::error_code ec;
  EXPECT_FALSE(std::filesystem::exists(tmp.parent_path() / "x-2", ec));
  EXPECT_FALSE(std::filesystem::exists(tmp / "aios-repair-a", ec));
  for (const auto& e : std::filesystem::directory_iterator(tmp, ec)) {
    const auto name = e.path().filename().string();
    EXPECT_EQ(name.rfind("aios-repair-", 0), std::string::npos) << "leftover " << name;
  }
}

// ---- OBJ-4 companion: ObjectStat exposes delete markers on request ---------------

TEST(Review2Object, StatIncludeDeletedReportsMarker) {
  using namespace aios;
  Cluster c(1, 1, 1);
  auto* n = c.nodes[0].get();
  const std::string oid = "r2/stat-del";
  const auto d = payload(10);
  ASSERT_TRUE(n->svc->api_put(oid, d.data(), d.size(), {}, true, {}).ok);
  ASSERT_TRUE(n->svc->api_del(oid, {}).ok);
  auto plain = object_stat_remote(n->addr, "client", "127.0.0.1:1", kKey, 60000, c.map.epoch,
                                  n->aios_path, oid);
  EXPECT_FALSE(plain.ok);
  auto raw = object_stat_remote(n->addr, "client", "127.0.0.1:1", kKey, 60000, c.map.epoch,
                                n->aios_path, oid, /*include_deleted=*/true);
  ASSERT_TRUE(raw.ok) << raw.error;
  EXPECT_TRUE(raw.body.value("deleted", false));
  EXPECT_EQ(raw.body.value("seq", 0), 2);
}
