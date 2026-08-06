// Regression tests for a batch of correctness/security fixes. Each test fails
// against the pre-fix code and documents the exact hazard in its name.
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "cluster/place.hpp"
#include "ec/ec_attrs.hpp"
#include "http/http_auth.hpp"
#include "http/http_server.hpp"
#include "http/s3_server.hpp"
#include "net/server.hpp"
#include "object/object_layout.hpp"
#include "object/transition.hpp"
#include "store/object_store.hpp"
#include "util/log.hpp"

#include <algorithm>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

using tcp = boost::asio::ip::tcp;
namespace fs = std::filesystem;

int unique_port(int base) { return base + static_cast<int>(::getpid() % 900); }

std::string rfc7231_date(std::time_t t) {
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
  return std::string(buf);
}

// Sends a raw request and returns the status line code, or -1 if the peer closed
// without a response. Deliberately not signed: the checks under test run before auth.
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

using aios::test::EcFixture;

}  // namespace

// An EC object needs k shards to decode. Publishing after only `write_quorum`
// shards installed (which can be < k) yields a permanently unreadable object.
TEST(EcQuorumRegression, PutIsRejectedWhenFewerThanKShardsInstall) {
  using namespace aios;
  EcFixture fx;
  const std::string oid = "ec/quorum-1";

  auto pl = place(oid, fx.map, fx.cfg.replica_count, fx.cfg.default_storage_class);
  ASSERT_GE(pl.acting_set.size(), 3u) << "acting set must cover k+m";

  // Leave only the primary store open so shards 1..m cannot install.
  fx.stores.sync_paths({pl.acting_set[0].aios_path}, EcFixture::opts());

  const std::string body(4096, 'E');
  auto put = fx.svc->api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                             {}, true, {});
  EXPECT_FALSE(put.ok) << "EC put must fail when fewer than k shards installed";
  EXPECT_EQ(put.code, "quorum_failed");

  // The tip must not have been published: a readable object here would mean the
  // pre-fix behaviour (undecodable object advertised as committed).
  auto got = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  EXPECT_FALSE(got.ok) << "no tip should be published for a failed EC put";
}

// Draining an EC object is attribute-only bookkeeping. The replicated drain path
// writes one shard body (plus its aios.ec.i) to every target, which leaves the
// acting set holding k+m copies of the same shard and destroys the object.
TEST(EcTransitionRegression, DrainKeepsDistinctShardsPerTarget) {
  using namespace aios;
  EcFixture fx;
  fx.cfg.write_quorum = 2;
  const std::string oid = "ectrans/obj-1";
  const std::string body(4096, 'T');

  auto put = fx.svc->api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                             {}, true, {});
  ASSERT_TRUE(put.ok) << put.error;

  const auto sc = fx.cfg.default_storage_class;
  const std::vector<std::string> paths{fx.p1, fx.p2, fx.p3};

  // Collect the shard index each target legitimately holds after the EC put.
  std::vector<std::string> before;
  for (const auto& p : paths) {
    auto* s = fx.stores.get(p);
    ASSERT_NE(s, nullptr);
    std::string err;
    auto a = s->list_attrs(oid, err);
    if (a.count(kEcAttrI)) before.push_back(a[kEcAttrI]);
  }
  ASSERT_EQ(before.size(), 3u) << "each target should hold one shard";
  std::sort(before.begin(), before.end());
  ASSERT_EQ(before, (std::vector<std::string>{"0", "1", "2"})) << "distinct shards after put";

  // Put the object into the mid-transition state a drain pass acts on, keeping
  // every target's own shard body and index intact.
  for (const auto& p : paths) {
    auto* s = fx.stores.get(p);
    std::string err;
    auto a = s->list_attrs(oid, err);
    auto shard = s->get(oid, err);
    ASSERT_TRUE(shard.has_value()) << err;
    a[kStorageClassAttr] = sc;
    a[kStorageClassPrevAttr] = "ssd";
    a[kTransitionAttr] = "copying";
    PreparedVersion pv;
    ASSERT_TRUE(s->prepare_put(oid, shard->data(), shard->size(), a, true, std::nullopt, pv, err))
        << err;
    ASSERT_TRUE(s->publish_tip(oid, pv.seq, err)) << err;
  }

  TransitionRule rule;
  rule.prefix = "ectrans/";
  rule.from = "ssd";
  rule.to = sc;
  fx.cfg.transition_rules = {rule};

  auto stats = run_transitions(fx.cfg, "127.0.0.1:7400", fx.map, fx.stores, 100);
  EXPECT_EQ(stats.drained, 1u) << "drain should run for the staged object";

  std::vector<std::string> after;
  for (const auto& p : paths) {
    auto* s = fx.stores.get(p);
    std::string err;
    auto a = s->list_attrs(oid, err);
    EXPECT_EQ(a[kTransitionAttr], "done") << "drain finalizes the transition attr on " << p;
    EXPECT_EQ(a.count(kStorageClassPrevAttr), 0u) << "prev class cleared on " << p;
    if (a.count(kEcAttrI)) after.push_back(a[kEcAttrI]);
  }
  std::sort(after.begin(), after.end());
  EXPECT_EQ(after, (std::vector<std::string>{"0", "1", "2"}))
      << "every target must keep its own shard index after drain";

  auto got = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  ASSERT_TRUE(got.ok && got.data.has_value()) << "object must still decode after drain";
  EXPECT_EQ(std::string(got.data->begin(), got.data->end()), body);
}

// A short pread must surface as an error; returning the partial buffer silently
// hands truncated data to the caller, which never checks the returned length.
TEST(StoreRangeRegression, TruncatedBackingFileFailsInsteadOfReturningShortData) {
  using namespace aios;
  const auto root = fs::temp_directory_path() / ("aios-shortread-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);

  ObjectStore store;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.inline_max_bytes = 64;  // force an fs-backed body
  std::string err;
  ASSERT_TRUE(store.open(root.string(), opts, err)) << err;

  const std::string body(4096, 'A');
  ASSERT_TRUE(store.put("trunc", body, {}, true, err)) << err;

  auto full = store.get_range("trunc", 0, body.size(), err);
  ASSERT_TRUE(full.has_value()) << "baseline full range read";
  EXPECT_EQ(full->size(), body.size());

  auto path = store.fs_body_path("trunc", err);
  ASSERT_TRUE(path.has_value()) << "expected fs-backed body: " << err;
  ASSERT_EQ(::truncate(path->c_str(), 100), 0) << "truncate backing file";

  err.clear();
  auto ranged = store.get_range("trunc", 0, body.size(), err);
  EXPECT_FALSE(ranged.has_value()) << "short read must not be reported as success";
  EXPECT_NE(err.find("short read"), std::string::npos) << "err was: " << err;

  fs::remove_all(root);
}

namespace {

// Builds a correctly signed request whose date header is `date`.
aios::HttpAuthResult verify_with_date(const std::string& date, int skew_ms) {
  using namespace aios;
  const std::string key = "550e8400-e29b-41d4-a716-446655440000";
  std::unordered_map<std::string, std::string> h = {
      {"x-aios-date", date},
      {"x-aios-content-sha256", "UNSIGNED-PAYLOAD"},
  };
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  const auto canon = http_canonical("GET", "/o/x", date, signed_headers, h, "UNSIGNED-PAYLOAD");
  h["authorization"] = "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" + signed_headers +
                       ", Signature=" + http_sign(key, canon);
  return http_auth_verify("GET", "/o/x", h, "UNSIGNED-PAYLOAD", key, skew_ms);
}

}  // namespace

// Skew was only enforced for all-digit dates, so any calendar-formatted date
// (what curl and the AWS SDKs send) made a signed request replayable forever.
TEST(HttpAuthRegression, CalendarDateIsSkewCheckedNotExempted) {
  const auto now = std::time(nullptr);

  auto stale = verify_with_date(rfc7231_date(now - 3600), 60'000);
  EXPECT_FALSE(stale.ok) << "an hour-old RFC 7231 date must be rejected";
  EXPECT_EQ(stale.error, "date skew too large");

  // Calendar dates must still work inside the window (the reason they were allowed).
  auto fresh = verify_with_date(rfc7231_date(now), 60'000);
  EXPECT_TRUE(fresh.ok) << "current RFC 7231 date should verify: " << fresh.error;

  auto iso = verify_with_date("20260806T120000Z", 60'000);
  EXPECT_FALSE(iso.ok) << "stale ISO 8601 basic date must be skew-checked";

  auto garbage = verify_with_date("not-a-date", 60'000);
  EXPECT_FALSE(garbage.ok) << "unparsable date must fail closed";
  EXPECT_EQ(garbage.error, "unparsable date");

  // Numeric unix-ms dates keep their existing behaviour.
  EXPECT_TRUE(verify_with_date(std::to_string(aios::now_ms()), 60'000).ok);
  EXPECT_FALSE(verify_with_date(std::to_string(aios::now_ms() - 3'600'000), 60'000).ok);
}

// `end - start + 1` wraps to 0 for end==UINT64_MAX and matched an empty body.
TEST(HttpWireRegression, ContentRangeLengthOverflowIsRejected) {
  using namespace aios;
  using aios::test::DualStoreFixture;
  DualStoreFixture fx("aios-crange");
  const std::string port = std::to_string(unique_port(18600));
  fx.cfg.http_listen = "127.0.0.1:" + port;

  boost::asio::io_context ioc;
  auto work = boost::asio::make_work_guard(ioc);
  HttpServer http(ioc, fx.cfg, *fx.svc, fx.membership);
  http.start();
  std::thread th([&] { ioc.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  const std::string date = std::to_string(now_ms());
  std::unordered_map<std::string, std::string> h = {
      {"x-aios-date", date},
      {"x-aios-content-sha256", "UNSIGNED-PAYLOAD"},
      {"content-range", "bytes 0-18446744073709551615/*"},
      {"content-length", "0"},
  };
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  const auto canon =
      http_canonical("PUT", "/o/crange", date, signed_headers, h, "UNSIGNED-PAYLOAD");
  h["authorization"] = "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" + signed_headers +
                       ", Signature=" + http_sign(fx.cfg.cluster_key, canon);

  std::ostringstream req;
  req << "PUT /o/crange HTTP/1.1\r\nHost: 127.0.0.1:" << port << "\r\nConnection: close\r\n";
  for (const auto& [k, v] : h) req << k << ": " << v << "\r\n";
  req << "\r\n";

  EXPECT_EQ(raw_request(port, req.str()), 400) << "overflowing Content-Range must be a 400";

  http.close_sessions();
  work.reset();
  ioc.stop();
  th.join();
}

// Header and body limits must be enforced before the request is authenticated.
TEST(S3WireRegression, OversizeHeadersAndBodyAreRejectedBeforeAuth) {
  using namespace aios;
  using aios::test::DualStoreFixture;
  DualStoreFixture fx("aios-s3caps");
  const std::string http_port = std::to_string(unique_port(18700));
  const std::string s3_port = std::to_string(unique_port(19700));
  fx.cfg.http_listen = "127.0.0.1:" + http_port;
  fx.cfg.s3_listen = "127.0.0.1:" + s3_port;
  fx.cfg.s3_volume = "s3";
  fx.cfg.s3_access_key = "aios";
  fx.cfg.max_object_bytes = 4096;

  boost::asio::io_context ioc;
  auto work = boost::asio::make_work_guard(ioc);
  std::thread th([&] { ioc.run(); });
  HttpServer http(ioc, fx.cfg, *fx.svc, fx.membership);
  http.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto s3 = std::make_unique<S3Server>(ioc, fx.cfg, "127.0.0.1:" + http_port);
  s3->start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Content-Length beyond max_object_bytes must not be allocated, signed or not.
  {
    std::ostringstream req;
    req << "PUT /bucket/key HTTP/1.1\r\nHost: 127.0.0.1:" << s3_port << "\r\n"
        << "Content-Length: 1000000\r\n\r\n";
    EXPECT_EQ(raw_request(s3_port, req.str()), 413) << "oversize body must be 413 before auth";
  }

  // A header section with no terminator must be bounded, not grown without limit.
  {
    std::ostringstream req;
    req << "GET /bucket HTTP/1.1\r\nHost: 127.0.0.1:" << s3_port << "\r\n";
    for (int i = 0; i < 400; ++i) {
      req << "X-Pad-" << i << ": " << std::string(512, 'p') << "\r\n";
    }
    // Note: no terminating blank line, so read_until must stop on the size cap.
    const int status = raw_request(s3_port, req.str());
    EXPECT_TRUE(status == 431 || status == -1)
        << "oversize header section must be refused, got " << status;
  }

  s3->stop();
  s3.reset();
  http.close_sessions();
  work.reset();
  ioc.stop();
  th.join();
}

// close() must drop live sessions, otherwise a session blocked in a synchronous
// read keeps running against handlers_ that the shutdown path is about to free.
TEST(TcpServerRegression, CloseDrainsLiveSessions) {
  using namespace aios;
  const std::string port = std::to_string(unique_port(18800));

  boost::asio::io_context ioc;
  auto work = boost::asio::make_work_guard(ioc);
  RpcHandlers handlers;
  handlers.local_node_id = "node-a";
  handlers.local_listen = "127.0.0.1:" + port;
  handlers.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  handlers.on_object = [](const Frame&) { return Frame{}; };

  auto server = std::make_unique<TcpServer>(ioc, "127.0.0.1", port, std::move(handlers));
  server->start();
  std::thread th([&] { ioc.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect but never send a full frame: the session parks in a blocking read.
  tcp::resolver resolver(ioc);
  boost::system::error_code ec;
  auto endpoints = resolver.resolve("127.0.0.1", port, ec);
  ASSERT_FALSE(ec) << ec.message();
  tcp::socket client(ioc);
  boost::asio::connect(client, endpoints, ec);
  ASSERT_FALSE(ec) << ec.message();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  server->close();

  // The server side of the connection must now be gone. `reader_done` flips only
  // when the read returns on its own; closing the client here would end the read
  // regardless and mask an undrained session, so it happens after the verdict.
  std::atomic<bool> reader_done{false};
  std::thread waiter([&] {
    char tmp[64];
    boost::system::error_code rec;
    client.read_some(boost::asio::buffer(tmp), rec);
    reader_done.store(true);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!reader_done.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const bool drained = reader_done.load();
  if (!drained) {
    boost::system::error_code ignored;
    client.close(ignored);
  }
  waiter.join();
  EXPECT_TRUE(drained) << "close() must tear down live sessions";

  work.reset();
  ioc.stop();
  th.join();
  server.reset();  // safe only once the io_context thread is joined
}

// The cluster map is swapped wholesale while HTTP workers read it by reference,
// so the swap has to go through the lock that serializes request handling. The
// race itself only reproduces reliably under a sanitizer; what is checked here is
// the contract that makes it safe: publishing goes through ObjectService, and a
// published map is visible to subsequent requests.
TEST(ClusterMapRegression, PublishIsVisibleAndSafeUnderConcurrentRequests) {
  using namespace aios;
  using aios::test::DualStoreFixture;
  DualStoreFixture fx("aios-mapswap");

  const std::string oid = "map/obj";
  const std::string body(512, 'M');
  auto seed = fx.svc->api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()),
                              body.size(), {}, true, {});
  ASSERT_TRUE(seed.ok) << seed.error;

  std::atomic<bool> stop{false};
  std::atomic<int> reads{0};
  std::vector<std::thread> workers;
  for (int i = 0; i < 2; ++i) {
    workers.emplace_back([&] {
      while (!stop.load()) {
        auto r = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
        if (r.ok) reads.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
  }

  PlacementConfig pc;
  for (int i = 0; i < 50; ++i) {
    fx.svc->update_cluster_map(
        ClusterMap::build(fx.membership, fx.fs_table, fx.cfg.replica_count, pc));
  }
  stop.store(true);
  for (auto& t : workers) t.join();

  EXPECT_GT(reads.load(), 0) << "readers should have made progress";

  // A published map must be what subsequent requests observe.
  auto published = ClusterMap::build(fx.membership, fx.fs_table, fx.cfg.replica_count, pc);
  const auto expected_targets = published.targets.size();
  const auto expected_epoch = published.epoch;
  fx.svc->update_cluster_map(std::move(published));
  EXPECT_EQ(fx.svc->map().targets.size(), expected_targets);
  EXPECT_EQ(fx.svc->map().epoch, expected_epoch);
  EXPECT_FALSE(fx.svc->map().targets.empty()) << "map remains intact after concurrent swaps";

  auto after = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  EXPECT_TRUE(after.ok) << "requests still serve correctly after a map swap";
}
