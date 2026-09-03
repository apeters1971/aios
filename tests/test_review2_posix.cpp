// Regression tests for the 2026-09-02 code review (posix slice).
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "client/changelog.hpp"
#include "client/error.hpp"
#include "client/map.hpp"
#include "client/mutex.hpp"
#include "client/session.hpp"
#include "http/http_auth.hpp"
#include "http/http_server.hpp"
#include "posix/aios_posix.h"
#include "posix/flock_owners.hpp"
#include "posix/posix_internal.hpp"
#include "posix/quota_ledger.hpp"
#include "util/auth.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <errno.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using aios::test::DualStoreFixture;
using tcp = boost::asio::ip::tcp;

struct HttpFixture {
  DualStoreFixture fx;
  int port_num;
  std::string host{"127.0.0.1"};
  std::string port;
  boost::asio::io_context ioc;
  std::unique_ptr<aios::HttpServer> http;
  std::thread th;

  explicit HttpFixture(const char* prefix, int base_port)
      : fx(prefix, 2, 2, "nvme"), port_num(base_port + static_cast<int>(::getpid() % 200)) {
    port = std::to_string(port_num);
    fx.cfg.http_listen = host + ":" + port;
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership);
    http->start();
    th = std::thread([this] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }

  ~HttpFixture() {
    ioc.stop();
    if (th.joinable()) th.join();
  }

  std::string endpoint() const { return host + ":" + port; }
  aios::SessionConfig session_cfg() const {
    return aios::SessionConfig{.endpoint = endpoint(), .cluster_key = fx.cfg.cluster_key};
  }
};

// One mount = one FsState (own inode/dir caches, own flusher thread).
struct Mount {
  std::string ep;
  std::string key;
  std::string vol;
  aios_posix_fs* fs{nullptr};

  Mount(const HttpFixture& http, const char* volume, uint64_t stripe_unit = 4096,
        int rstat_interval_ms = 0)
      : ep(http.endpoint()), key(http.fx.cfg.cluster_key), vol(volume) {
    aios_posix_config cfg{};
    cfg.endpoint = ep.c_str();
    cfg.cluster_key = key.c_str();
    cfg.volume = vol.c_str();
    cfg.stripe_unit = stripe_unit;
    cfg.stripe_width = 1;
    cfg.uid = 1000;
    cfg.gid = 1000;
    cfg.rstat_interval_ms = rstat_interval_ms;
    int err = 0;
    fs = aios_posix_mount(&cfg, &err);
    EXPECT_NE(fs, nullptr) << "mount err=" << err;
  }

  ~Mount() {
    if (fs) aios_posix_unmount(fs);
  }

  Mount(const Mount&) = delete;
  Mount& operator=(const Mount&) = delete;
};

// Minimal HTTP/1.1 stub: one thread per accepted connection so concurrent
// clients cannot deadlock the test. The handler may answer, answer-and-close,
// or drop the connection without a byte in response.
struct StubReply {
  std::string body;      // full HTTP response bytes; ignored when drop
  bool close{true};      // close the socket after answering
  bool drop{false};      // close without responding
};

struct StubServer {
  using Handler = std::function<StubReply(const std::string& request, int conn_index)>;

  boost::asio::io_context ioc;
  tcp::acceptor acc;
  std::thread th;
  std::vector<std::thread> workers;
  std::mutex workers_mu;
  std::string port;
  std::atomic<bool> stop{false};
  std::atomic<int> accept_count{0};
  std::atomic<int> request_count{0};
  std::mutex req_mu;
  std::vector<std::string> requests;
  std::mutex live_mu;
  std::vector<std::shared_ptr<tcp::socket>> live;
  Handler handler;

  explicit StubServer(Handler h) : acc(ioc, tcp::endpoint(tcp::v4(), 0)), handler(std::move(h)) {
    port = std::to_string(acc.local_endpoint().port());
    th = std::thread([this] {
      for (;;) {
        boost::system::error_code ec;
        auto sock = std::make_shared<tcp::socket>(ioc);
        acc.accept(*sock, ec);
        if (ec || stop.load()) return;
        const int idx = accept_count.fetch_add(1);
        {
          std::lock_guard lock(live_mu);
          live.push_back(sock);
        }
        std::lock_guard lock(workers_mu);
        workers.emplace_back([this, sock, idx] { serve(sock, idx); });
      }
    });
  }

  ~StubServer() {
    stop.store(true);
    // Closing worker sockets unblocks keep-alive read_some. Closing the
    // acceptor from this thread is not required to interrupt a blocking
    // accept() (POSIX); a dummy connect to the listen port is.
    {
      std::lock_guard lock(live_mu);
      for (auto& s : live) {
        boost::system::error_code ec;
        s->shutdown(tcp::socket::shutdown_both, ec);
        s->close(ec);
      }
    }
    boost::system::error_code ec;
    const auto listen_port = static_cast<unsigned short>(std::stoul(port));
    try {
      boost::asio::io_context poke_ioc;
      tcp::socket poke(poke_ioc);
      poke.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), listen_port), ec);
    } catch (...) {
    }
    acc.close(ec);
    if (th.joinable()) th.join();
    std::lock_guard lock(workers_mu);
    for (auto& w : workers) {
      if (w.joinable()) w.join();
    }
  }

  static bool read_request(tcp::socket& sock, std::string& out) {
    boost::system::error_code ec;
    std::string acc;
    char tmp[4096];
    while (acc.find("\r\n\r\n") == std::string::npos) {
      const auto n = sock.read_some(boost::asio::buffer(tmp), ec);
      if (ec || n == 0) return false;
      acc.append(tmp, tmp + n);
    }
    std::size_t content_length = 0;
    {
      std::string lower = acc;
      for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const auto p = lower.find("content-length:");
      if (p != std::string::npos) {
        const auto end = lower.find("\r\n", p);
        try {
          content_length = static_cast<std::size_t>(
              std::stoull(lower.substr(p + 15, end - (p + 15))));
        } catch (...) {
        }
      }
    }
    const auto hdr_end = acc.find("\r\n\r\n") + 4;
    while (acc.size() - hdr_end < content_length) {
      const auto n = sock.read_some(boost::asio::buffer(tmp), ec);
      if (ec || n == 0) return false;
      acc.append(tmp, tmp + n);
    }
    out = std::move(acc);
    return true;
  }

  void serve(std::shared_ptr<tcp::socket> sock, int idx) {
    for (;;) {
      std::string req;
      if (!read_request(*sock, req)) break;
      request_count.fetch_add(1);
      {
        std::lock_guard lock(req_mu);
        requests.push_back(req);
      }
      if (stop.load()) break;
      const StubReply reply = handler(req, idx);
      if (reply.drop) break;
      boost::system::error_code ec;
      boost::asio::write(*sock, boost::asio::buffer(reply.body), ec);
      if (ec || reply.close) break;
    }
    boost::system::error_code ec;
    sock->shutdown(tcp::socket::shutdown_both, ec);
    sock->close(ec);
  }

  int count_requests_with(const std::string& needle) {
    std::lock_guard lock(req_mu);
    int n = 0;
    for (const auto& r : requests) {
      if (r.find(needle) != std::string::npos) ++n;
    }
    return n;
  }

  std::string last_request() {
    std::lock_guard lock(req_mu);
    return requests.empty() ? std::string{} : requests.back();
  }
};

std::string http_response(int status, const std::string& body, bool keep_alive,
                          const std::vector<std::pair<std::string, std::string>>& extra = {}) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << status << " OK\r\n";
  oss << "Content-Length: " << body.size() << "\r\n";
  for (const auto& [k, v] : extra) oss << k << ": " << v << "\r\n";
  oss << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n\r\n";
  oss << body;
  return oss.str();
}

aios::SessionConfig stub_cfg(const StubServer& stub, int timeout_ms = 2000) {
  aios::SessionConfig cfg;
  cfg.endpoint = "127.0.0.1:" + stub.port;
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = timeout_ms;
  return cfg;
}

uint64_t create_file(aios_posix_fs* fs, uint64_t parent, const char* name) {
  aios_posix_stat st{};
  const int rc = aios_posix_create(fs, parent, name, 0644, &st);
  EXPECT_EQ(rc, 0) << "create " << name;
  return st.ino;
}

}  // namespace

// ---------------------------------------------------------------------------
// POS-2 — concurrent writers to one inode must merge size, not replace it
// ---------------------------------------------------------------------------

TEST(Review2Posix, EightConcurrentDisjointWritersKeepFullSize) {
  HttpFixture http("aios-r2p-pos2", 23000);
  Mount m(http, "pos2vol", 4096);
  const uint64_t ino = create_file(m.fs, 1, "wide");

  constexpr int kThreads = 8;
  constexpr size_t kRegion = 4096;
  std::vector<std::thread> ts;
  std::vector<int> rcs(kThreads, 1);
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      const std::string data(kRegion, static_cast<char>('a' + t));
      size_t wrote = 0;
      rcs[static_cast<size_t>(t)] =
          aios_posix_write(m.fs, ino, static_cast<uint64_t>(t) * kRegion, data.data(),
                           data.size(), &wrote);
    });
  }
  for (auto& t : ts) t.join();
  for (int t = 0; t < kThreads; ++t) EXPECT_EQ(rcs[static_cast<size_t>(t)], 0) << t;

  aios_posix_stat st{};
  ASSERT_EQ(aios_posix_getattr(m.fs, ino, &st), 0);
  EXPECT_EQ(st.size, kThreads * kRegion);

  // The deferred PUT must carry the merged size to a mount with a cold cache.
  ASSERT_EQ(aios_posix_fsync(m.fs, ino), 0);
  Mount peer(http, "pos2vol", 4096);
  aios_posix_stat pst{};
  ASSERT_EQ(aios_posix_getattr(peer.fs, ino, &pst), 0);
  EXPECT_EQ(pst.size, kThreads * kRegion);

  std::vector<char> buf(kThreads * kRegion);
  size_t got = 0;
  ASSERT_EQ(aios_posix_read(m.fs, ino, 0, buf.data(), buf.size(), &got), 0);
  ASSERT_EQ(got, buf.size());
  for (int t = 0; t < kThreads; ++t) {
    EXPECT_EQ(std::string(buf.data() + static_cast<size_t>(t) * kRegion, kRegion),
              std::string(kRegion, static_cast<char>('a' + t)))
        << "region " << t;
  }
}

// ---------------------------------------------------------------------------
// POS-1 — deferred inode PUT is flushed in the background
// ---------------------------------------------------------------------------

TEST(Review2Posix, DeferredSizeVisibleToSecondMountWithoutFsync) {
  HttpFixture http("aios-r2p-pos1", 23020);
  Mount a(http, "pos1vol", 4096);
  const uint64_t ino = create_file(a.fs, 1, "nofsync");
  const std::string data(3000, 'x');
  size_t wrote = 0;
  ASSERT_EQ(aios_posix_write(a.fs, ino, 0, data.data(), data.size(), &wrote), 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  Mount b(http, "pos1vol", 4096);
  aios_posix_stat st{};
  ASSERT_EQ(aios_posix_getattr(b.fs, ino, &st), 0);
  EXPECT_EQ(st.size, data.size());
  EXPECT_GT(st.mtime_ns, 0u);
}

// ---------------------------------------------------------------------------
// POS-3 — inode cache TTL: a peer's chmod becomes visible after the TTL
// ---------------------------------------------------------------------------

TEST(Review2Posix, InodeCacheRevalidatesAfterTtl) {
  HttpFixture http("aios-r2p-pos3", 23040);
  Mount a(http, "pos3vol", 4096);
  const uint64_t ino = create_file(a.fs, 1, "modes");
  aios_posix_stat st{};
  ASSERT_EQ(aios_posix_getattr(a.fs, ino, &st), 0);  // cached on A
  EXPECT_EQ(st.mode & 0777u, 0644u);

  Mount b(http, "pos3vol", 4096);
  aios_posix_stat ps{};
  ps.mode = 0600;
  ASSERT_EQ(aios_posix_setattr(b.fs, ino, &ps, AIOS_POSIX_SET_MODE), 0);

  std::this_thread::sleep_for(aios::posix::kInodeCacheTtl + std::chrono::milliseconds(150));
  ASSERT_EQ(aios_posix_getattr(a.fs, ino, &st), 0);
  EXPECT_EQ(st.mode & 0777u, 0600u);

  // Super is re-read on the same TTL: a freeze taken elsewhere is observed.
  {
    aios::Session s(http.session_cfg());
    auto snap = s.get_object(aios::posix::super_oid("pos3vol"));
    ASSERT_TRUE(snap.exists);
    auto j = nlohmann::json::parse(snap.body);
    j["frozen"] = true;
    s.put_bytes(aios::posix::super_oid("pos3vol"), j.dump(), snap.attrs, std::nullopt);
  }
  std::this_thread::sleep_for(aios::posix::kInodeCacheTtl + std::chrono::milliseconds(150));
  aios_posix_stat cs{};
  EXPECT_EQ(aios_posix_create(a.fs, 1, "frozen-create", 0644, &cs), -EBUSY);
  {
    aios::Session s(http.session_cfg());
    auto snap = s.get_object(aios::posix::super_oid("pos3vol"));
    auto j = nlohmann::json::parse(snap.body);
    j["frozen"] = false;
    s.put_bytes(aios::posix::super_oid("pos3vol"), j.dump(), snap.attrs, std::nullopt);
  }
}

// ---------------------------------------------------------------------------
// POS-6 — store_inode conflict reapply keeps a pending deferred size
// ---------------------------------------------------------------------------

TEST(Review2Posix, ConflictReapplyPreservesDeferredSize) {
  HttpFixture http("aios-r2p-pos6", 23060);
  Mount a(http, "pos6vol", 4096);
  const uint64_t ino = create_file(a.fs, 1, "reapply");

  Mount b(http, "pos6vol", 4096);
  aios_posix_stat warm{};
  ASSERT_EQ(aios_posix_getattr(b.fs, ino, &warm), 0);

  const std::string data(2500, 'q');
  size_t wrote = 0;
  ASSERT_EQ(aios_posix_write(a.fs, ino, 0, data.data(), data.size(), &wrote), 0);

  // B bumps the inode cas behind A's back (A's cached record is now stale).
  aios_posix_stat pb{};
  pb.mode = 0640;
  ASSERT_EQ(aios_posix_setattr(b.fs, ino, &pb, AIOS_POSIX_SET_MODE), 0);

  // A's setattr conflicts and rebuilds from the server copy (size 0 there unless
  // the flusher already landed it); the pending size must survive either way.
  aios_posix_stat pa{};
  pa.mode = 0604;
  ASSERT_EQ(aios_posix_setattr(a.fs, ino, &pa, AIOS_POSIX_SET_MODE), 0);
  ASSERT_EQ(aios_posix_fsync(a.fs, ino), 0);

  Mount c(http, "pos6vol", 4096);
  aios_posix_stat st{};
  ASSERT_EQ(aios_posix_getattr(c.fs, ino, &st), 0);
  EXPECT_EQ(st.size, data.size());
  EXPECT_EQ(st.mode & 0777u, 0604u);
}

// ---------------------------------------------------------------------------
// POS-7 — chunk cache never regresses to an older version
// ---------------------------------------------------------------------------

TEST(Review2Posix, ChunkCacheKeepsNewerVersion) {
  aios::posix::ChunkCache cache;
  cache.store(7, 0, std::string("v2"), 6);
  cache.store(7, 0, std::string("v1"), 5);  // late reader with the older body
  uint64_t cas = 0;
  auto body = cache.lookup(7, 0, cas);
  ASSERT_TRUE(body);
  EXPECT_EQ(*body, "v2");
  EXPECT_EQ(cas, 6u);

  cache.store(7, 0, std::string("v3"), 7);
  body = cache.lookup(7, 0, cas);
  ASSERT_TRUE(body);
  EXPECT_EQ(*body, "v3");
  EXPECT_EQ(cas, 7u);

  // Lookups share the buffer instead of copying it.
  auto again = cache.lookup(7, 0, cas);
  EXPECT_EQ(body.get(), again.get());

  cache.drop(7, 0);
  EXPECT_FALSE(cache.lookup(7, 0, cas));
  EXPECT_EQ(cache.cached_cas(7, 0), 0u);
}

// ---------------------------------------------------------------------------
// POS-8 — redirect allowlist is safe under concurrent refresh
// ---------------------------------------------------------------------------

TEST(Review2Posix, RedirectAllowlistConcurrentRefresh) {
  StubServer peer([](const std::string&, int) {
    return StubReply{http_response(200, "peer-ok", false), true, false};
  });
  const std::string peer_hp = "127.0.0.1:" + peer.port;
  StubServer stub([&](const std::string& req, int) {
    if (req.find("GET /admin/cluster ") != std::string::npos) {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      const std::string body =
          std::string(R"({"node_id":"n0","admin_peers":[{"http_addr":")") + peer_hp + R"("}]})";
      return StubReply{http_response(200, body, false), true, false};
    }
    return StubReply{http_response(307, R"({"code":"not_primary"})", false,
                                   {{"Location", "http://" + peer_hp + "/o/x"}}),
                     true, false};
  });

  aios::Session s(stub_cfg(stub));
  constexpr int kThreads = 16;
  std::atomic<int> ok{0};
  std::atomic<int> rejected{0};
  std::atomic<int> other{0};
  std::vector<std::thread> ts;
  for (int i = 0; i < kThreads; ++i) {
    ts.emplace_back([&] {
      try {
        auto r = s.request("GET", "/o/x");
        if (r.status == 200 && r.body == "peer-ok") ok.fetch_add(1);
        else other.fetch_add(1);
      } catch (const aios::client_error& e) {
        if (std::string(e.what()).find("redirect target not in cluster") != std::string::npos) {
          rejected.fetch_add(1);
        } else {
          other.fetch_add(1);
        }
      }
    });
  }
  for (auto& t : ts) t.join();
  EXPECT_EQ(other.load(), 0);
  EXPECT_EQ(ok.load() + rejected.load(), kThreads);
  EXPECT_GE(ok.load(), 1);  // the refreshing thread follows the redirect
  // Exactly one one-shot refresh regardless of how many threads raced it.
  EXPECT_EQ(stub.count_requests_with("GET /admin/cluster "), 1);
  // After the refresh every thread is allowed through.
  auto r = s.request("GET", "/o/x");
  EXPECT_EQ(r.status, 200);
}

// ---------------------------------------------------------------------------
// POS-5 — keep-alive retry only for stale idle sockets and idempotent methods
// ---------------------------------------------------------------------------

TEST(Review2Posix, PostOnReusedConnectionIsNotReplayed) {
  StubServer stub([](const std::string& req, int) {
    if (req.rfind("POST ", 0) == 0) return StubReply{{}, true, true};  // read, then vanish
    return StubReply{http_response(200, "ok", true), false, false};
  });

  aios::Session s(stub_cfg(stub));
  auto warm = s.request("GET", "/o/warm");
  ASSERT_EQ(warm.status, 200);

  try {
    s.request("POST", "/o/lock/lock", {}, "");
    FAIL() << "expected transport error";
  } catch (const aios::client_error& e) {
    EXPECT_EQ(e.code(), "http");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(stub.count_requests_with("POST "), 1) << "POST must execute at most once";
  EXPECT_EQ(stub.accept_count.load(), 1);
}

TEST(Review2Posix, GetOnStaleIdleConnectionIsRetried) {
  StubServer stub([](const std::string& req, int conn) {
    if (conn == 0 && req.find("/o/first") != std::string::npos) {
      // Answer keep-alive, then the server drops the idle socket.
      return StubReply{http_response(200, "first", true) , true, false};
    }
    return StubReply{http_response(200, "second", true), false, false};
  });

  aios::Session s(stub_cfg(stub));
  auto a = s.request("GET", "/o/first");
  ASSERT_EQ(a.status, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the close land
  auto b = s.request("GET", "/o/second");
  EXPECT_EQ(b.status, 200);
  EXPECT_EQ(b.body, "second");
  EXPECT_EQ(stub.accept_count.load(), 2);
}

TEST(Review2Posix, TimeoutOnReusedConnectionIsNotReplayed) {
  std::atomic<bool> hang{true};
  StubServer stub([&](const std::string& req, int) {
    if (req.find("/o/slow") != std::string::npos) {
      while (hang.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      return StubReply{{}, true, true};
    }
    return StubReply{http_response(200, "ok", true), false, false};
  });

  aios::Session s(stub_cfg(stub, 200));
  ASSERT_EQ(s.request("GET", "/o/warm").status, 200);
  try {
    s.request("PUT", "/o/slow", {}, "body");
    FAIL() << "expected timeout";
  } catch (const aios::client_error& e) {
    EXPECT_NE(std::string(e.what()).find("timeout"), std::string::npos) << e.what();
  }
  hang.store(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(stub.count_requests_with("PUT /o/slow"), 1) << "a timed-out PUT is not replayed";
}

// ---------------------------------------------------------------------------
// POS-11 — every body is signed with its real sha256 (no UNSIGNED-PAYLOAD)
// ---------------------------------------------------------------------------

TEST(Review2Posix, LargeBodyIsSignedWithRealSha256) {
  StubServer stub([](const std::string&, int) {
    return StubReply{http_response(200, "ok", false), true, false};
  });
  aios::Session s(stub_cfg(stub));
  const std::string body(aios::kHttpStreamBodyBytes + 1, 'A');
  s.request("PUT", "/o/1M", {}, body);

  auto req = stub.last_request();
  auto lower = req;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  EXPECT_EQ(lower.find("unsigned-payload"), std::string::npos);
  EXPECT_NE(req.find("x-aios-content-sha256: " + aios::sha256_hex(body)), std::string::npos);
}

// ---------------------------------------------------------------------------
// C6 — connect has a deadline
// ---------------------------------------------------------------------------

TEST(Review2Posix, ConnectToBlackholeFailsWithinTimeout) {
  aios::SessionConfig cfg;
  cfg.endpoint = "10.255.255.1:9";  // TEST-NET-adjacent non-routable address
  cfg.cluster_key = "550e8400-e29b-41d4-a716-446655440000";
  cfg.socket_timeout_ms = 500;
  aios::Session s(cfg);
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_THROW(s.request("GET", "/o/x"), aios::client_error);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  EXPECT_LT(ms, 5000) << "connect must honour socket_timeout_ms";
}

// ---------------------------------------------------------------------------
// POS-4 — changelog holes: pull skips aged holes, compact never destroys records
// ---------------------------------------------------------------------------

TEST(Review2Posix, ChangelogHoleIsSkippedAfterGraceAndCompactIsSafe) {
  using namespace aios;
  using namespace aios::changelog;
  HttpFixture http("aios-r2p-pos4", 23080);
  Session sess(http.session_cfg());
  Log log(sess, "map", "holes");
  log.set_gap_grace(std::chrono::milliseconds(400));

  ASSERT_EQ(log.append_op(Op::Put, {"a", "1"}, sync_mode::async), 1u);
  // Reserve id 2 (CAS meta) without appending: a writer that died mid-append.
  {
    Meta m = log.load_meta();
    ASSERT_TRUE(m.exists);
    ASSERT_EQ(m.next_op, 2u);
    m.next_op = 3;
    log.store_meta(m, sync_mode::async);
  }
  ASSERT_EQ(log.append_op(Op::Put, {"b", "2"}, sync_mode::async), 3u);
  ASSERT_EQ(log.append_op(Op::Put, {"c", "3"}, sync_mode::async), 4u);

  std::vector<std::uint64_t> applied;
  std::uint64_t applied_op = 0;
  auto apply = [&](const Record& r) { applied.push_back(r.op_id); };
  auto on_snapshot = [](const std::string&) {};

  log.pull(&applied_op, on_snapshot, apply);
  EXPECT_EQ(applied_op, 1u) << "hole not yet aged: stop before it";
  EXPECT_EQ(applied, (std::vector<std::uint64_t>{1}));

  // compact() must not truncate while records sit behind the hole.
  std::uint64_t rebuilt_to = 0;
  const bool compacted = log.compact(sync_mode::async, [&] {
    applied_op = 0;
    applied.clear();
    log.pull(&applied_op, on_snapshot, apply);
    rebuilt_to = applied_op;
    return std::make_pair(nlohmann::json{{"upto", applied_op}}.dump(), applied_op);
  });
  EXPECT_FALSE(compacted);
  EXPECT_EQ(rebuilt_to, 1u);
  EXPECT_EQ(log.max_logged_op(), 4u) << "records behind the hole survive";

  std::this_thread::sleep_for(std::chrono::milliseconds(450));
  applied_op = 1;
  applied.clear();
  log.pull(&applied_op, on_snapshot, apply);
  EXPECT_EQ(applied_op, 4u);
  EXPECT_EQ(applied, (std::vector<std::uint64_t>{3, 4}));

  // Now the hole is aged for this Log: compaction folds everything in.
  const bool compacted2 = log.compact(sync_mode::async, [&] {
    applied_op = 0;
    applied.clear();
    log.pull(&applied_op, on_snapshot, apply);
    return std::make_pair(nlohmann::json{{"upto", applied_op}}.dump(), applied_op);
  });
  EXPECT_TRUE(compacted2);
  EXPECT_EQ(applied, (std::vector<std::uint64_t>{1, 3, 4}));
  Meta after = log.load_meta();
  EXPECT_EQ(after.snapshot_op, 4u);
  EXPECT_EQ(after.log_bytes, 0u);
  const std::string want_snap = nlohmann::json{{"upto", 4}}.dump();
  EXPECT_EQ(log.load_snapshot_body(), want_snap);
}

// ---------------------------------------------------------------------------
// C2 — flush must not skip peer ops reserved since the last pull
// ---------------------------------------------------------------------------

TEST(Review2Posix, MapFlushSeesInterleavedPeerOp) {
  using namespace aios;
  HttpFixture http("aios-r2p-c2", 23100);
  Session sa(http.session_cfg());
  Session sb(http.session_cfg());

  aios::map a(sa, "c2map", sync_mode::async);
  a.load();  // applied_op = 0 (empty)
  aios::map b(sb, "c2map", sync_mode::sync);
  b.set("peer", "1");  // id 1, reserved and appended after A's pull

  a.set("mine", "2");
  a.flush();  // A's op gets id 2; the old code set applied_op=2 and skipped id 1
  EXPECT_TRUE(a.contains("mine"));
  EXPECT_TRUE(a.contains("peer")) << "peer op reserved between pull and append was skipped";
}

// ---------------------------------------------------------------------------
// POS-9 — unlink is conditional on the dentry still pointing at the inode
// ---------------------------------------------------------------------------

TEST(Review2Posix, StaleUnlinkDoesNotRemoveRecreatedFile) {
  HttpFixture http("aios-r2p-pos9", 23120);
  const std::string sz_new(1500, 'n');
  uint64_t new_ino = 0;
  bool new_removed = false;
  {
    Mount a(http, "pos9vol", 4096);
    const uint64_t old_ino = create_file(a.fs, 1, "f");
    const std::string sz_old(700, 'o');
    size_t wrote = 0;
    ASSERT_EQ(aios_posix_write(a.fs, old_ino, 0, sz_old.data(), sz_old.size(), &wrote), 0);
    ASSERT_EQ(aios_posix_fsync(a.fs, old_ino), 0);

    // A lists (warms its 250 ms directory cache).
    uint64_t off = 0;
    aios_posix_dirent ents[8];
    ASSERT_GE(aios_posix_readdir(a.fs, 1, &off, ents, 8), 1);

    // B removes f and recreates it as a different inode.
    Mount b(http, "pos9vol", 4096);
    ASSERT_EQ(aios_posix_unlink(b.fs, 1, "f"), 0);
    new_ino = create_file(b.fs, 1, "f");
    ASSERT_NE(new_ino, old_ino);
    ASSERT_EQ(aios_posix_write(b.fs, new_ino, 0, sz_new.data(), sz_new.size(), &wrote), 0);
    ASSERT_EQ(aios_posix_fsync(b.fs, new_ino), 0);

    // A's cached dentry still says f == old_ino. Either the stale removal is
    // refused (ENOENT), or — if A's 250 ms dir cache had already expired — A
    // saw and removed the new f. Both are consistent; a half state is not.
    const int rc = aios_posix_unlink(a.fs, 1, "f");
    aios_posix_stat st{};
    if (rc == 0) {
      new_removed = true;
      EXPECT_EQ(aios_posix_lookup(b.fs, 1, "f", &st), -ENOENT);
      // B's inode cache (1s TTL) may still hold new_ino; a fresh mount is the
      // source of truth for whether the inode object was actually removed.
    } else {
      EXPECT_EQ(rc, -ENOENT) << "stale dentry must not half-remove the peer's new file";
      ASSERT_EQ(aios_posix_getattr(b.fs, new_ino, &st), 0);
      EXPECT_EQ(st.size, sz_new.size());
      ASSERT_EQ(aios_posix_lookup(b.fs, 1, "f", &st), 0);
      EXPECT_EQ(st.ino, new_ino);
    }
  }
  // Quota usage equals the surviving bytes (no double credit for the old file).
  const std::int64_t expected = new_removed ? 0 : static_cast<std::int64_t>(sz_new.size());
  aios::Session s(http.session_cfg());
  auto snap = s.get_object(aios::posix::quota_usage_oid("pos9vol"));
  ASSERT_TRUE(snap.exists);
  auto usage = aios::posix::parse_quota_usage(snap.body, 0);
  EXPECT_EQ(usage.volume_uids[1000], expected);

  Mount c(http, "pos9vol", 4096);
  aios_posix_stat st{};
  if (new_removed) {
    EXPECT_EQ(aios_posix_lookup(c.fs, 1, "f", &st), -ENOENT);
    EXPECT_EQ(aios_posix_getattr(c.fs, new_ino, &st), -ENOENT);
  } else {
    ASSERT_EQ(aios_posix_lookup(c.fs, 1, "f", &st), 0);
    EXPECT_EQ(st.ino, new_ino);
    EXPECT_EQ(st.size, sz_new.size());
  }
}

TEST(Review2Posix, RmdirRechecksEmptinessUnderLock) {
  HttpFixture http("aios-r2p-pos9b", 23140);
  Mount a(http, "pos9bvol", 4096);
  aios_posix_stat d{};
  ASSERT_EQ(aios_posix_mkdir(a.fs, 1, "d", 0755, &d), 0);
  uint64_t off = 0;
  aios_posix_dirent ents[4];
  ASSERT_EQ(aios_posix_readdir(a.fs, d.ino, &off, ents, 4), 2);  // "." and ".." only, cached

  Mount b(http, "pos9bvol", 4096);
  create_file(b.fs, d.ino, "inner");

  EXPECT_EQ(aios_posix_rmdir(a.fs, 1, "d"), -ENOTEMPTY);
  aios_posix_stat st{};
  EXPECT_EQ(aios_posix_lookup(b.fs, d.ino, "inner", &st), 0);
}

// ---------------------------------------------------------------------------
// POS-12b — rename must reject moving a directory under its own descendant
// ---------------------------------------------------------------------------

TEST(Review2Posix, RenameIntoOwnDescendantIsRejected) {
  HttpFixture http("aios-r2p-pos12b", 23160);
  Mount m(http, "pos12bvol", 4096);
  aios_posix_stat a{}, b{}, c{};
  ASSERT_EQ(aios_posix_mkdir(m.fs, 1, "a", 0755, &a), 0);
  ASSERT_EQ(aios_posix_mkdir(m.fs, a.ino, "b", 0755, &b), 0);
  ASSERT_EQ(aios_posix_mkdir(m.fs, b.ino, "c", 0755, &c), 0);

  // a → a/b/c/a: new_parent (c) is a grandchild of the moved dir.
  EXPECT_EQ(aios_posix_rename(m.fs, 1, "a", c.ino, "a"), -EINVAL);
  // Direct child (one level) still rejected.
  EXPECT_EQ(aios_posix_rename(m.fs, 1, "a", b.ino, "a"), -EINVAL);
  // Unrelated destination still works.
  aios_posix_stat other{};
  ASSERT_EQ(aios_posix_mkdir(m.fs, 1, "other", 0755, &other), 0);
  EXPECT_EQ(aios_posix_rename(m.fs, 1, "a", other.ino, "a"), 0);
  aios_posix_stat st{};
  EXPECT_EQ(aios_posix_lookup(m.fs, other.ino, "a", &st), 0);
  EXPECT_EQ(st.ino, a.ino);
}

// ---------------------------------------------------------------------------
// POS-12c — contended sub-chunk writes do not exhaust CAS retries
// ---------------------------------------------------------------------------

TEST(Review2Posix, ContendedSubChunkWritesAllSucceed) {
  HttpFixture http("aios-r2p-pos12c", 23180);
  Mount m(http, "pos12cvol", 4096);
  const uint64_t ino = create_file(m.fs, 1, "hot");

  constexpr int kThreads = 8;
  constexpr int kWrites = 6;
  std::vector<std::thread> ts;
  std::atomic<int> failures{0};
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      const std::string data(512, static_cast<char>('A' + t));
      for (int w = 0; w < kWrites; ++w) {
        size_t wrote = 0;
        // Every thread hammers the same 4 KiB chunk at its own 512 B slot.
        const int rc = aios_posix_write(m.fs, ino, static_cast<uint64_t>(t) * 512, data.data(),
                                        data.size(), &wrote);
        if (rc != 0) failures.fetch_add(1);
      }
    });
  }
  for (auto& t : ts) t.join();
  EXPECT_EQ(failures.load(), 0);

  std::vector<char> buf(kThreads * 512);
  size_t got = 0;
  ASSERT_EQ(aios_posix_read(m.fs, ino, 0, buf.data(), buf.size(), &got), 0);
  ASSERT_EQ(got, buf.size());
  for (int t = 0; t < kThreads; ++t) {
    EXPECT_EQ(std::string(buf.data() + static_cast<size_t>(t) * 512, 512),
              std::string(512, static_cast<char>('A' + t)))
        << "slot " << t;
  }
}

// ---------------------------------------------------------------------------
// POS-12a — flock ownership is per open, not per inode
// ---------------------------------------------------------------------------

TEST(Review2Posix, FlockOwnersReleaseOnlyForAcquiringOpen) {
  aios::posix::FlockOwners owners;
  const uint64_t ino = 42;
  const uint64_t open_a = 0x1001, open_b = 0x1002;

  owners.note_locked(ino, open_a);
  EXPECT_TRUE(owners.holds(ino, open_a));
  EXPECT_FALSE(owners.holds(ino, open_b));

  // release() of the open that never locked must not drop the inode's lock.
  EXPECT_FALSE(owners.note_unlocked(ino, open_b));
  EXPECT_TRUE(owners.holds(ino, open_a));

  // The holder's release drops it exactly once.
  EXPECT_TRUE(owners.note_unlocked(ino, open_a));
  EXPECT_FALSE(owners.note_unlocked(ino, open_a));
  EXPECT_FALSE(owners.holds(ino, open_a));

  owners.note_locked(ino, open_a);
  owners.note_locked(ino + 1, open_a);
  owners.forget(ino);
  EXPECT_FALSE(owners.holds(ino, open_a));
  EXPECT_TRUE(owners.holds(ino + 1, open_a));
}

TEST(Review2Posix, FlockHeldByOneOpenSurvivesOtherOpenRelease) {
  HttpFixture http("aios-r2p-pos12a", 23200);
  Mount a(http, "pos12avol", 4096);
  const uint64_t ino = create_file(a.fs, 1, "locked");
  ASSERT_EQ(aios_posix_flock(a.fs, ino, LOCK_EX | LOCK_NB), 0);

  // Mirrors posix_release for a second open (owner B) of the same inode.
  aios::posix::FlockOwners owners;
  owners.note_locked(ino, /*owner_a=*/1);
  if (owners.note_unlocked(ino, /*owner_b=*/2)) {
    (void)aios_posix_flock(a.fs, ino, LOCK_UN);
  }
  // Peer mount still cannot take the lock: A's open keeps it.
  Mount b(http, "pos12avol", 4096);
  EXPECT_EQ(aios_posix_flock(b.fs, ino, LOCK_EX | LOCK_NB), -EWOULDBLOCK);

  // The acquiring open's release does drop it.
  if (owners.note_unlocked(ino, 1)) {
    ASSERT_EQ(aios_posix_flock(a.fs, ino, LOCK_UN), 0);
  }
  EXPECT_EQ(aios_posix_flock(b.fs, ino, LOCK_EX | LOCK_NB), 0);
  EXPECT_EQ(aios_posix_flock(b.fs, ino, LOCK_UN), 0);
}

// ---------------------------------------------------------------------------
// POS-12d — mutex lease is renewed on the holder's path
// ---------------------------------------------------------------------------

TEST(Review2Posix, MutexOwnsLockRenewsLease) {
  using namespace aios;
  HttpFixture http("aios-r2p-pos12d", 23220);
  Session a(http.session_cfg());
  Session b(http.session_cfg());

  mutex m1(a, "renewlock", /*ttl_ms=*/1200);
  ASSERT_TRUE(m1.try_lock());
  // Past 2/3 of the TTL: owns_lock() must renew rather than merely report.
  std::this_thread::sleep_for(std::chrono::milliseconds(900));
  EXPECT_TRUE(m1.owns_lock());
  // Beyond the original lease: still owned thanks to the renewal.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_TRUE(m1.owns_lock());
  mutex m2(b, "renewlock", 1200);
  EXPECT_FALSE(m2.try_lock()) << "server lease must have been extended too";
  m1.unlock();
  EXPECT_TRUE(m2.try_lock());
}

// ---------------------------------------------------------------------------
// POS-10 — mutating entry points check the freeze inside the exception barrier
// ---------------------------------------------------------------------------

TEST(Review2Posix, FrozenVolumeRejectsLinkAndRenameWithoutThrowing) {
  HttpFixture http("aios-r2p-pos10", 23240);
  Mount m(http, "pos10vol", 4096);
  const uint64_t ino = create_file(m.fs, 1, "src");
  (void)ino;
  {
    aios::Session s(http.session_cfg());
    auto snap = s.get_object(aios::posix::super_oid("pos10vol"));
    ASSERT_TRUE(snap.exists);
    auto j = nlohmann::json::parse(snap.body);
    j["frozen"] = true;
    s.put_bytes(aios::posix::super_oid("pos10vol"), j.dump(), snap.attrs, std::nullopt);
  }
  std::this_thread::sleep_for(aios::posix::kInodeCacheTtl + std::chrono::milliseconds(150));
  EXPECT_EQ(aios_posix_link(m.fs, 1, "src", 1, "hard"), -EBUSY);
  EXPECT_EQ(aios_posix_rename(m.fs, 1, "src", 1, "moved"), -EBUSY);
  {
    aios::Session s(http.session_cfg());
    auto snap = s.get_object(aios::posix::super_oid("pos10vol"));
    auto j = nlohmann::json::parse(snap.body);
    j["frozen"] = false;
    s.put_bytes(aios::posix::super_oid("pos10vol"), j.dump(), snap.attrs, std::nullopt);
  }
}
