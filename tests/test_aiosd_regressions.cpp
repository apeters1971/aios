// Regression tests for the aiosd functionality review. Each test fails against the
// pre-fix code and names the hazard it pins down.
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "cluster/place.hpp"
#include "ec/ec_attrs.hpp"
#include "http/http_auth.hpp"
#include "http/http_server.hpp"
#include "net/server.hpp"
#include "store/object_store.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

using tcp = boost::asio::ip::tcp;

int unique_port(int base) { return base + static_cast<int>(::getpid() % 900); }

// Sends a raw request and returns the status line code, or -1 if the peer closed
// without a response.
int raw_request(const std::string& port, const std::string& raw, int timeout_ms = 5000) {
  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  boost::system::error_code ec;
  auto endpoints = resolver.resolve("127.0.0.1", port, ec);
  if (ec) return -1;
  tcp::socket sock(ioc);
  for (int attempt = 0; attempt < 50; ++attempt) {
    boost::asio::connect(sock, endpoints, ec);
    if (!ec) break;
    sock = tcp::socket(ioc);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (ec) return -1;

  boost::asio::write(sock, boost::asio::buffer(raw), ec);
  if (ec) return -1;
  sock.shutdown(tcp::socket::shutdown_send, ec);

  std::atomic<int> status{-1};
  std::atomic<bool> done{false};
  std::thread reader([&] {
    boost::system::error_code rec;
    boost::asio::streambuf buf;
    boost::asio::read_until(sock, buf, "\r\n", rec);
    if (!rec || buf.size()) {
      std::istream is(&buf);
      std::string line;
      std::getline(is, line);
      std::istringstream ss(line);
      std::string ver;
      int code = -1;
      ss >> ver >> code;
      status.store(code);
    }
    done.store(true);
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!done.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!done.load()) {
    boost::system::error_code ignored;
    sock.close(ignored);
  }
  reader.join();
  return status.load();
}

aios::StorageTarget make_storage_target(const std::string& node, const std::string& path,
                                        int weight, const std::string& rack) {
  aios::StorageTarget t;
  t.node_id = node;
  t.addr = "127.0.0.1:7400";
  t.aios_path = path;
  t.mount = path;
  t.storage_class = "nvme";
  t.rack = rack;
  t.weight = weight;
  t.state = aios::LifecycleState::Up;
  return t;
}

}  // namespace

// ---------------------------------------------------------------------------
// Erasure coding
// ---------------------------------------------------------------------------

// commit_ec_put writes shard i to acting_set[i], but place() reorders the acting
// set on any topology change. Reading by position then feeds the codec mismatched
// shards; the authoritative index is the aios.ec.i attr stored with each shard.
TEST(EcShardIdentityRegression, ReadUsesStoredShardIndexNotActingSetPosition) {
  using namespace aios;
  aios::test::EcFixture fx("aios-ecident");
  fx.cfg.write_quorum = 3;
  const std::string oid = "ecident/obj-1";
  // Content must vary across the object, otherwise the shards are byte-identical
  // and swapping them is not observable.
  std::string body;
  body.reserve(8192);
  for (int i = 0; i < 8192; ++i) {
    body.push_back(static_cast<char>('a' + ((i * 31 + i / 7) % 26)));
  }

  auto put = fx.svc->api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                             {}, true, {});
  ASSERT_TRUE(put.ok) << put.error;

  const auto pl = place(oid, fx.map, fx.cfg.replica_count, fx.cfg.default_storage_class);
  ASSERT_GE(pl.acting_set.size(), 3u);

  // Move the shard held by acting_set[0] onto acting_set[1] and vice versa, each
  // keeping its own aios.ec.i. This is exactly what a reordered acting set looks
  // like to a reader that trusts position.
  struct Held {
    ObjectStore* store{nullptr};
    std::vector<std::uint8_t> body;
    std::unordered_map<std::string, std::string> attrs;
    std::uint64_t seq{0};
  };
  std::vector<Held> held;
  for (std::size_t i = 0; i < 2; ++i) {
    Held h;
    h.store = fx.stores.get(pl.acting_set[i].aios_path);
    ASSERT_NE(h.store, nullptr);
    std::string err;
    auto got = h.store->get(oid, err);
    ASSERT_TRUE(got.has_value()) << err;
    h.body = std::move(*got);
    h.attrs = h.store->list_attrs(oid, err);
    auto st = h.store->stat(oid, std::nullopt, err);
    ASSERT_TRUE(st.has_value()) << err;
    h.seq = st->seq;
    ASSERT_TRUE(h.attrs.count(kEcAttrI)) << "put must stamp the shard index";
    held.push_back(std::move(h));
  }
  ASSERT_NE(held[0].attrs[kEcAttrI], held[1].attrs[kEcAttrI]);

  auto install = [&](Held& dst, const Held& src) {
    PreparedVersion pv;
    pv.oid = oid;
    pv.seq = dst.seq + 1;
    pv.size = src.body.size();
    pv.crc32c = crc32c(src.body.data(), src.body.size());
    pv.inline_body = true;
    pv.is_delete = false;
    std::string err;
    ASSERT_TRUE(dst.store->install_version(pv, src.body.data(), src.body.size(), src.attrs, err))
        << err;
    ASSERT_TRUE(dst.store->publish_tip(oid, pv.seq, err)) << err;
  };
  install(held[0], held[1]);
  install(held[1], held[0]);

  auto got = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  ASSERT_TRUE(got.ok) << got.code << ": " << got.error;
  ASSERT_TRUE(got.data.has_value());
  const std::string round_trip(got.data->begin(), got.data->end());
  EXPECT_EQ(round_trip, body) << "shards must be decoded at their stored index";
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

// weight * vnodes_per_target was clamped to max_vnodes per target, so every target
// at or above 8 weight units (~8 TiB) got an identical ring share and capacity
// weighting silently stopped working on real hardware.
TEST(PlacementWeightRegression, CapacityWeightSurvivesTheVnodeClamp) {
  using namespace aios;
  ClusterMap map;
  map.replica_count = 1;
  map.placement = PlacementConfig{};  // defaults: 128 per unit, max 1024
  ASSERT_GT(16 * map.placement.vnodes_per_target, map.placement.max_vnodes)
      << "test only meaningful when both targets clamp pre-fix";
  map.targets = {make_storage_target("small", "/small/aios", 16, "r1"),
                 make_storage_target("large", "/large/aios", 200, "r2")};

  int small = 0;
  int large = 0;
  for (int i = 0; i < 4000; ++i) {
    const auto p = place("weight/obj-" + std::to_string(i), map, 1, "nvme");
    ASSERT_EQ(p.acting_set.size(), 1u);
    if (p.acting_set[0].node_id == "small") ++small;
    else ++large;
  }
  // True ratio is 12.5:1. Pre-fix both clamped to 1024 vnodes and this was ~1:1.
  EXPECT_GT(large, small * 5) << "large=" << large << " small=" << small;
}

// The third placement pass fills leftover slots from mounts on nodes already in
// the acting set, so a class with fewer nodes than copies silently loses failure
// domains. Placement must report what it actually covered.
TEST(PlacementDomainRegression, ReportsFailureDomainsWhenCopiesShareANode) {
  using namespace aios;
  ClusterMap map;
  map.replica_count = 3;
  map.placement = PlacementConfig{};
  map.targets = {make_storage_target("n1", "/n1/a/aios", 1, "r1"),
                 make_storage_target("n1", "/n1/b/aios", 1, "r1"),
                 make_storage_target("n2", "/n2/a/aios", 1, "r2")};

  const auto p = place("domain/obj-1", map, 3, "nvme");
  ASSERT_EQ(p.acting_set.size(), 3u) << "placement still fills the acting set";
  EXPECT_EQ(p.distinct_nodes, 2u) << "two copies share a node, so one loss removes two";
  EXPECT_LT(p.distinct_nodes, p.acting_set.size());
}

// ---------------------------------------------------------------------------
// Cluster membership and discovery
// ---------------------------------------------------------------------------

// An off target used to just stop being advertised. FsTable::merge is upsert-only,
// so peers kept the last "up" row forever and went on placing on a target that no
// longer exists on the owning node.
TEST(FsTableRegression, OffTargetIsAdvertisedAsTombstone) {
  using namespace aios;
  FsTable owner;
  FsTable peer;

  auto up = aios::test::make_target("/data/aios");
  owner.set_local("node-a", {up});
  peer.merge(owner.snapshot());
  ASSERT_EQ(peer.snapshot().size(), 1u);
  EXPECT_TRUE(peer.snapshot()[0].usable);

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  auto off = up;
  off.state = LifecycleState::Off;
  owner.set_local("node-a", {off});

  const auto advertised = owner.snapshot();
  ASSERT_EQ(advertised.size(), 1u) << "the off target must still be advertised, as a tombstone";
  EXPECT_FALSE(advertised[0].usable);

  peer.merge(advertised);
  ASSERT_EQ(peer.snapshot().size(), 1u);
  EXPECT_FALSE(peer.snapshot()[0].usable) << "peer must learn the target is gone";

  MembershipTable members;
  members.set_local("node-b", "127.0.0.1:7401");
  members.mark_alive("node-a", "127.0.0.1:7400", now_ms());
  const auto map = ClusterMap::build(members, peer, 1, PlacementConfig{});
  EXPECT_TRUE(map.targets.empty()) << "a tombstoned target must not enter the cluster map";
}

// last_seen_ms freezes when a node stops answering, so every peer holds the same
// value while each ages it at a different moment. Accepting a tie let a peer that
// had not aged yet overwrite the Offline we had just derived.
TEST(MembershipRegression, EqualTimestampGossipDoesNotResurrectDeadNode) {
  using namespace aios;
  MembershipTable table;
  table.set_local("node-a", "127.0.0.1:7400");
  const std::int64_t t0 = now_ms();
  table.mark_alive("node-b", "127.0.0.1:7401", t0);
  table.age(t0 + 20000, 5000, 15000);

  auto state_of = [&](const char* id) {
    for (const auto& m : table.snapshot()) {
      if (m.node_id == id) return m.state;
    }
    return MemberState::Online;
  };
  ASSERT_EQ(state_of("node-b"), MemberState::Offline);

  Member stale;
  stale.node_id = "node-b";
  stale.addr = "127.0.0.1:7401";
  stale.state = MemberState::Online;
  stale.last_seen_ms = t0;  // same observation we already aged out
  table.merge({stale}, t0 + 20000);

  EXPECT_EQ(state_of("node-b"), MemberState::Offline)
      << "a tie must not resurrect a node we already declared dead";
}

// last_seen_ms comes from the observer's system clock. A peer running ahead used to
// stamp a time age() could never reach, pinning a dead node Online cluster-wide.
TEST(MembershipRegression, FutureTimestampIsClampedToLocalClock) {
  using namespace aios;
  MembershipTable table;
  table.set_local("node-a", "127.0.0.1:7400");
  const std::int64_t now = now_ms();

  Member skewed;
  skewed.node_id = "node-b";
  skewed.addr = "127.0.0.1:7401";
  skewed.state = MemberState::Online;
  skewed.last_seen_ms = now + 3600000;  // an hour ahead
  table.merge({skewed}, now);
  table.age(now + 20000, 5000, 15000);

  for (const auto& m : table.snapshot()) {
    if (m.node_id != "node-b") continue;
    EXPECT_EQ(m.state, MemberState::Offline) << "a future timestamp must not defeat aging";
  }
}

// Nothing else ever dials an Offline member, so a node that comes back could only
// be rediscovered if it had seed peers of its own.
TEST(MembershipRegression, OfflinePeerIsStillProbed) {
  using namespace aios;
  MembershipTable table;
  table.set_local("node-a", "127.0.0.1:7400");
  const std::int64_t t0 = now_ms();
  table.mark_alive("node-b", "127.0.0.1:7401", t0);
  table.age(t0 + 20000, 5000, 15000);

  const auto peers = table.peers_for_gossip(3);
  bool found = false;
  for (const auto& p : peers) found = found || p.node_id == "node-b";
  EXPECT_TRUE(found) << "an offline peer must still be probed so it can rejoin";
}

// ---------------------------------------------------------------------------
// Daemon serving paths
// ---------------------------------------------------------------------------

struct HttpTestServer {
  aios::test::DualStoreFixture fx;
  std::string port;
  boost::asio::io_context ioc;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
  std::unique_ptr<aios::HttpServer> http;
  std::thread th;

  HttpTestServer(const char* prefix, int base_port)
      : fx(prefix), port(std::to_string(unique_port(base_port))),
        work(boost::asio::make_work_guard(ioc)) {
    fx.cfg.http_listen = "127.0.0.1:" + port;
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership);
    http->start();
    th = std::thread([this] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  ~HttpTestServer() {
    http->close_sessions();
    work.reset();
    ioc.stop();
    if (th.joinable()) th.join();
  }
};

// Chunked bodies were never decoded: with no Content-Length the request was
// dispatched as a zero-byte PUT that answered 204, and the chunk framing was then
// parsed as the next request.
TEST(HttpWireRegression, ChunkedUploadIsRejectedInsteadOfStoredEmpty) {
  using namespace aios;
  HttpTestServer srv("aios-chunked", 18700);

  std::ostringstream req;
  req << "PUT /o/chunked HTTP/1.1\r\nHost: 127.0.0.1:" << srv.port << "\r\n"
      << "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      << "5\r\nhello\r\n0\r\n\r\n";
  EXPECT_EQ(raw_request(srv.port, req.str()), 501)
      << "a chunked upload must be refused, not silently stored empty";

  auto got = srv.fx.svc->api_get("chunked", std::nullopt, std::nullopt, {});
  EXPECT_FALSE(got.ok) << "no object may be created from a rejected chunked upload";
}

// The publish handler rejected anything past the 256 KiB spill-to-disk threshold
// while reporting the limit as 1 MiB, which is what the service layer enforces.
TEST(PubsubWireRegression, PublishAboveSpillThresholdIsAccepted) {
  using namespace aios;
  HttpTestServer srv("aios-pubsize", 18800);

  const std::string body(512u * 1024u, 'p');
  const std::string date = std::to_string(now_ms());
  const std::string path = "/pubsub/topics/big/publish";
  std::unordered_map<std::string, std::string> h = {
      {"x-aios-date", date},
      {"x-aios-content-sha256", "UNSIGNED-PAYLOAD"},
      {"content-length", std::to_string(body.size())},
  };
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  const auto canon = http_canonical("POST", path, date, signed_headers, h, "UNSIGNED-PAYLOAD");
  h["authorization"] = "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" + signed_headers +
                       ", Signature=" + http_sign(srv.fx.cfg.cluster_key, canon);

  std::ostringstream req;
  req << "POST " << path << " HTTP/1.1\r\nHost: 127.0.0.1:" << srv.port
      << "\r\nConnection: close\r\n";
  for (const auto& [k, v] : h) req << k << ": " << v << "\r\n";
  req << "\r\n" << body;

  const int status = raw_request(srv.port, req.str());
  EXPECT_NE(status, 413) << "512 KiB is under the documented 1 MiB publish limit";
  EXPECT_GE(status, 200);
  EXPECT_LT(status, 300) << "status " << status;
}

// TcpServer sessions read synchronously and stay open for keep-alive. Posting them
// to the shared io_context meant one idle peer parked the daemon's only io thread,
// stalling accepts, gossip and every timer.
TEST(TcpServerRegression, IdleSessionDoesNotBlockTheIoContextThread) {
  using namespace aios;
  boost::asio::io_context ioc;
  auto work = boost::asio::make_work_guard(ioc);
  const std::string port = std::to_string(unique_port(19400));

  RpcHandlers handlers;
  handlers.local_node_id = "node-a";
  handlers.local_listen = "127.0.0.1:" + port;
  handlers.cluster_key = "550e8400-e29b-41d4-a716-446655440000";

  TcpServer server(ioc, "127.0.0.1", port, handlers);
  server.start();
  // A single io thread, exactly as aiosd runs it.
  std::thread io([&] { ioc.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // Connect and stay silent: the session parks in a blocking frame read.
  boost::asio::io_context client_ioc;
  tcp::resolver resolver(client_ioc);
  boost::system::error_code ec;
  auto endpoints = resolver.resolve("127.0.0.1", port, ec);
  ASSERT_FALSE(ec);
  tcp::socket idle(client_ioc);
  boost::asio::connect(idle, endpoints, ec);
  ASSERT_FALSE(ec);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // The io_context must still be able to run work: this is what the gossip and
  // scan timers need.
  std::atomic<bool> ran{false};
  boost::asio::post(ioc, [&] { ran.store(true); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!ran.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(ran.load()) << "an idle RPC session must not own the io_context thread";

  boost::system::error_code ignored;
  idle.close(ignored);
  server.close();
  work.reset();
  ioc.stop();
  io.join();
}

// Long polls answer from a detached thread holding a raw ObjectService pointer.
// Shutdown used to walk straight past them and free the service underneath.
TEST(HttpServerRegression, ShutdownDrainsLongPollHandlers) {
  using namespace aios;
  auto srv = std::make_unique<HttpTestServer>("aios-watchdrain", 18900);

  const std::string date = std::to_string(now_ms());
  const std::string path = "/watch";
  const std::string query = "?prefix=drain/&timeout_ms=120000";
  std::unordered_map<std::string, std::string> h = {
      {"x-aios-date", date},
      {"x-aios-content-sha256", "UNSIGNED-PAYLOAD"},
      {"content-length", "0"},
  };
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  // The server canonicalises the request target, so the query has to be signed too.
  const auto canon =
      http_canonical("GET", path + query, date, signed_headers, h, "UNSIGNED-PAYLOAD");
  h["authorization"] = "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" + signed_headers +
                       ", Signature=" + http_sign(srv->fx.cfg.cluster_key, canon);

  std::ostringstream req;
  req << "GET " << path << query << " HTTP/1.1\r\nHost: 127.0.0.1:" << srv->port
      << "\r\nConnection: close\r\n";
  for (const auto& [k, v] : h) req << k << ": " << v << "\r\n";
  req << "\r\n";

  // The watch would otherwise hold its thread for the full 120 s timeout.
  std::atomic<bool> replied{false};
  std::thread watcher([&, body = req.str()] {
    raw_request(srv->port, body, 30000);
    replied.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  ASSERT_FALSE(replied.load())
      << "the request must actually be parked in the long poll for this test to mean anything";

  const auto start = std::chrono::steady_clock::now();
  srv.reset();  // close_sessions() + destructor
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 10)
      << "shutdown must release long polls rather than wait out their timeout";

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!replied.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(replied.load()) << "the long-poll handler must finish before the service is gone";
  watcher.join();
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// parse_cli runs before main's try block, so std::stoi on a bad value terminated
// the process instead of printing a usage error.
TEST(ConfigRegression, NonNumericIntegerArgumentIsAUsageError) {
  using namespace aios;
  Config cfg;
  std::string err;
  bool help = false;
  const char* argv[] = {"aiosd", "--replica-count", "three"};
  EXPECT_FALSE(parse_cli(3, const_cast<char**>(argv), cfg, err, help));
  EXPECT_NE(err.find("--replica-count"), std::string::npos) << err;
}

// The gossip and scan timers re-arm straight from these values and, unlike the
// repair timers, are not guarded by a > 0 check at start.
TEST(ConfigRegression, NonPositiveTimerIntervalsAreRejected) {
  using namespace aios;
  std::string err;
  {
    Config cfg;
    cfg.gossip_interval_ms = 0;
    EXPECT_FALSE(normalize_config(cfg, err)) << "a 0 ms gossip interval spins the io thread";
  }
  {
    Config cfg;
    cfg.scan_interval_ms = 0;
    EXPECT_FALSE(normalize_config(cfg, err)) << "a 0 ms scan interval spins the io thread";
  }
  {
    Config cfg;
    cfg.suspect_after_ms = cfg.dead_after_ms + 1;
    EXPECT_FALSE(normalize_config(cfg, err)) << "Suspect would be unreachable";
  }
}

// quorum_need() clamps to the acting-set size, so an over-large write_quorum was
// silently satisfied by fewer copies than the operator asked for.
TEST(ConfigRegression, WriteQuorumAboveReplicaCountIsRejected) {
  using namespace aios;
  Config cfg;
  cfg.replica_count = 2;
  cfg.write_quorum = 9;
  std::string err;
  EXPECT_FALSE(normalize_config(cfg, err));
  EXPECT_NE(err.find("write_quorum"), std::string::npos) << err;
}
