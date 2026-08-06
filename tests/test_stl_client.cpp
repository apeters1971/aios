#include "test_helpers.hpp"

#include "client/changelog.hpp"
#include "client/stl.hpp"
#include "client/wire.hpp"
#include "http/http_server.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

using aios::test::DualStoreFixture;
using aios::test::expect;
using aios::test::failures;

struct HttpFixture {
  DualStoreFixture fx;
  int port_num;
  std::string host{"127.0.0.1"};
  std::string port;
  boost::asio::io_context ioc;
  std::unique_ptr<aios::HttpServer> http;
  std::thread th;

  explicit HttpFixture(const char* prefix)
      : fx(prefix), port_num(19050 + static_cast<int>(::getpid() % 200)) {
    port = std::to_string(port_num);
    fx.cfg.http_listen = host + ":" + port;
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership);
    http->start();
    th = std::thread([this] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  ~HttpFixture() {
    ioc.stop();
    if (th.joinable()) th.join();
  }

  aios::SessionConfig cfg() const {
    return aios::SessionConfig{.endpoint = host + ":" + port,
                               .cluster_key = fx.cfg.cluster_key};
  }
};

}  // namespace

int test_stl_client() {
  using namespace aios;
  failures() = 0;

  // ASYNC string: assign → flush → second session load
  {
    HttpFixture hf("aios-stl-str");
    Session a(hf.cfg());
    Session b(hf.cfg());
    string s1(a, "greeting", sync_mode::async, /*flush_on_destroy=*/false);
    s1.assign("hello");
    expect(s1.dirty(), "async string dirty after assign");
    s1.flush();
    expect(!s1.dirty(), "async string clean after flush");

    string s2(b, "greeting", sync_mode::async, false);
    s2.load();
    expect(s2.str() == "hello", "async string round-trip");
  }

  // SYNC map: writer insert visible without flush
  {
    HttpFixture hf("aios-stl-map");
    Session a(hf.cfg());
    Session b(hf.cfg());
    map m1(a, "users", sync_mode::sync, false);
    m1["alice"] = "1";
    expect(m1.size() == 1, "sync map size");

    map m2(b, "users", sync_mode::sync, false);
    expect(m2.at("alice") == "1", "sync map visible to peer");
  }

  // ASYNC set + SYNC unordered_map
  {
    HttpFixture hf("aios-stl-set-umap");
    Session a(hf.cfg());
    Session b(hf.cfg());

    set s1(a, "tags", sync_mode::async, false);
    expect(s1.insert("red"), "set insert red");
    expect(s1.insert("blue"), "set insert blue");
    expect(!s1.insert("red"), "set duplicate");
    s1.flush();

    set s2(b, "tags", sync_mode::async, false);
    s2.load();
    expect(s2.size() == 2 && s2.contains("red") && s2.contains("blue"), "set round-trip");
    expect(s2.erase("blue") == 1, "set erase");
    s2.flush();

    unordered_map u1(a, "kv", sync_mode::sync, false);
    u1["x"] = "1";
    u1.insert_or_assign("y", "2");
    expect(u1.size() == 2, "umap size");

    unordered_map u2(b, "kv", sync_mode::sync, false);
    expect(u2.at("x") == "1" && u2.at("y") == "2", "umap visible to peer");
    expect(u2.erase("x") == 1, "umap erase");
    expect(!u2.contains("x"), "umap erased");
  }

  // ASYNC list/deque round-trip + dirty load policy
  {
    HttpFixture hf("aios-stl-seq");
    Session a(hf.cfg());
    list l(a, "q", sync_mode::async, false);
    l.push_back("a");
    l.push_front("b");
    l.flush();

    list l2(a, "q", sync_mode::async, false);
    l2.load();
    expect(l2.size() == 2 && l2.at(0) == "b" && l2.at(1) == "a", "list round-trip");

    deque d(a, "d", sync_mode::async, false);
    d.push_back("x");
    d.push_front("y");
    expect(d.dirty(), "deque dirty");
    bool threw = false;
    try {
      d.load();
    } catch (const client_error& e) {
      threw = e.code() == "bad_request";
    }
    expect(threw, "dirty load rejected");
    d.flush();
    deque d2(a, "d", sync_mode::async, false);
    d2.load();
    expect(d2.size() == 2 && d2.at(0) == "y", "deque round-trip");
  }

  // Mode switch: dirty ASYNC → set_mode(sync) fails; after flush ok
  {
    HttpFixture hf("aios-stl-mode");
    Session s(hf.cfg());
    string str(s, "m", sync_mode::async, false);
    str.assign("x");
    bool threw = false;
    try {
      str.set_mode(sync_mode::sync);
    } catch (const client_error& e) {
      threw = e.code() == "bad_request";
    }
    expect(threw, "set_mode sync while dirty fails");
    str.flush();
    str.set_mode(sync_mode::sync);
    str.append("y");
    string peer(s, "m", sync_mode::sync, false);
    expect(peer.str() == "xy", "mode switch then sync write");
  }

  // mutex: try_lock exclusivity
  {
    HttpFixture hf("aios-stl-mx");
    Session a(hf.cfg());
    Session b(hf.cfg());
    mutex m1(a, "orders");
    mutex m2(b, "orders");
    expect(m1.try_lock(), "mutex first try_lock");
    expect(!m2.try_lock(), "mutex second try_lock fails");
    m1.unlock();
    expect(m2.try_lock(), "mutex second acquires after unlock");
    m2.unlock();
  }

  // Conflict: two ASYNC flushes with same cas (string still whole-doc)
  {
    HttpFixture hf("aios-stl-cas");
    Session a(hf.cfg());
    Session b(hf.cfg());
    string s1(a, "cas", sync_mode::async, false);
    s1.assign("v1");
    s1.flush();

    string s2(b, "cas", sync_mode::async, false);
    s2.load();
    s1.assign("from-a");
    s1.flush();
    s2.assign("from-b");
    bool conflict = false;
    try {
      s2.flush();
    } catch (const client_error& e) {
      conflict = e.code() == "conflict";
    }
    expect(conflict, "stale cas flush conflicts");
  }

  // Changelog: compact truncates log; peer still sees state
  {
    HttpFixture hf("aios-stl-compact");
    Session a(hf.cfg());
    Session b(hf.cfg());
    map m1(a, "clog", sync_mode::sync, false);
    for (int i = 0; i < 20; ++i) m1.set("k" + std::to_string(i), "v");
    m1.erase("k0");
    m1.compact();

    auto log_head = a.head_object(changelog::log_oid("map", "clog"));
    expect(!log_head.exists || log_head.size == 0, "compact truncates log");

    map m2(b, "clog", sync_mode::sync, false);
    expect(m2.size() == 19 && m2.at("k1") == "v" && !m2.contains("k0"),
           "peer sees compacted map");
  }

  // v1 migrate-on-open
  {
    HttpFixture hf("aios-stl-migrate");
    Session s(hf.cfg());
    const auto body = wire::make_map_doc({{"legacy", "1"}}, sync_mode::async).dump();
    s.put_object(Session::stl_oid("map", "old"), body, "map", 0);
    map m(s, "old", sync_mode::sync, false);
    expect(m.at("legacy") == "1", "v1 migrate preserves entries");
    m.set("new", "2");
    map m2(s, "old", sync_mode::sync, false);
    expect(m2.at("legacy") == "1" && m2.at("new") == "2", "post-migrate append works");
    auto meta = s.get_object(Session::stl_oid("map", "old"));
    expect(meta.exists && meta.body.find("\"aios_stl\":2") != std::string::npos,
           "meta rewritten to v2");
  }

  // Concurrent SYNC writers via append (both keys visible)
  {
    HttpFixture hf("aios-stl-race");
    Session a(hf.cfg());
    Session b(hf.cfg());
    map m1(a, "race", sync_mode::sync, false);
    map m2(b, "race", sync_mode::sync, false);
    m1.set("a", "1");
    m2.set("b", "2");
    expect(m1.at("a") == "1", "writer a local");
    // Re-read from cluster
    map m3(a, "race", sync_mode::sync, false);
    expect(m3.contains("a") && m3.contains("b"), "both sync inserts visible");
  }

  // Typed containers: numeric keys/values, wire still UTF-8 strings
  {
    HttpFixture hf("aios-stl-typed");
    Session a(hf.cfg());
    Session b(hf.cfg());

    basic_map<std::int64_t, std::int64_t> m1(a, "nums", sync_mode::async, false);
    m1.set(10, 100);
    m1.set(2, 20);
    m1.flush();
    auto snap = m1.snapshot();
    expect(snap.begin()->first == 2 && std::next(snap.begin())->first == 10,
           "typed map numeric key order");
    m1.compact();

    basic_map<std::int64_t, std::int64_t> m2(b, "nums", sync_mode::sync, false);
    expect(m2.at(10) == 100 && m2.at(2) == 20, "typed map peer load");
    // Peer string map sees decimal keys
    map sm(b, "nums", sync_mode::sync, false);
    expect(sm.at("10") == "100" && sm.at("2") == "20", "typed map wire is decimal strings");

    basic_unordered_map<std::int64_t, std::int64_t> um(a, "uhash", sync_mode::sync, false);
    um.set(7, 77);
    basic_unordered_map<std::int64_t, std::int64_t> um2(b, "uhash", sync_mode::sync, false);
    expect(um2.at(7) == 77, "typed umap round-trip");

    basic_set<std::int64_t> s1(a, "ints", sync_mode::sync, false);
    expect(s1.insert(5) && s1.insert(1), "typed set insert");
    basic_set<std::int64_t> s2(b, "ints", sync_mode::sync, false);
    expect(s2.contains(5) && s2.contains(1), "typed set peer");
    auto ssnap = s2.snapshot();
    expect(*ssnap.begin() == 1, "typed set numeric order");

    basic_list<std::int64_t> l1(a, "ilist", sync_mode::async, false);
    l1.push_back(3);
    l1.push_back(9);
    l1.flush();
    basic_list<std::int64_t> l2(b, "ilist", sync_mode::sync, false);
    expect(l2.size() == 2 && l2.at(0) == 3 && l2.at(1) == 9, "typed list round-trip");

    basic_deque<double> d1(a, "dvals", sync_mode::sync, false);
    d1.push_back(1.5);
    d1.set_at(0, 2.25);
    basic_deque<double> d2(b, "dvals", sync_mode::sync, false);
    expect(d2.size() == 1 && d2.at(0) == 2.25, "typed deque double round-trip");
  }

  return failures();
}
