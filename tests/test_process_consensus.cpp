// Process-level test of the consensus-backed cluster map: three real `aiosd`
// processes configured as monitors. Checks that the map comes from the elected
// leader (small monotonic epoch, valid lease), that losing a node produces a new
// epoch and writes continue on the majority, and that a node left in the minority
// stops accepting writes (no lease) until the majority is back.
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "client/error.hpp"
#include "client/session.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {

using namespace std::chrono_literals;

const char* kClusterKey = "550e8400-e29b-41d4-a716-446655440000";

std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

int free_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  int port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
      port = ntohs(addr.sin_port);
    }
  }
  ::close(fd);
  return port;
}

struct Node {
  std::filesystem::path root;
  std::string id;
  int rpc_port{0};
  int http_port{0};
  pid_t pid{-1};

  std::string rpc() const { return "127.0.0.1:" + std::to_string(rpc_port); }
  std::string http() const { return "127.0.0.1:" + std::to_string(http_port); }

  void start(const std::vector<std::string>& monitors, const std::string& seed) {
    std::filesystem::create_directories(root / "disk");
    std::ofstream(root / "disk" / ".aios") << "storage_class: nvme\nweight: 1\nstate: up\n";
    std::ofstream(root / "aiosd.yaml")
        << "gossip_interval_ms: 500\nsuspect_after_ms: 2000\ndead_after_ms: 6000\n"
        << "map_lease_ms: 3000\nrepair_interval_ms: 0\n";
    std::vector<std::string> args{"--config", (root / "aiosd.yaml").string(),
                                  "--cluster-key", kClusterKey, "--node-id", id,
                                  "--listen", rpc(), "--http-listen", http(),
                                  "--status-file", (root / "status.json").string(),
                                  "--scan-root", (root / "disk").string(),
                                  "--replica-count", "2", "--write-quorum", "2",
                                  "--no-fsync"};
    for (const auto& m : monitors) args.insert(args.end(), {"--monitor", m});
    if (!seed.empty()) args.insert(args.end(), {"--peer", seed});
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(AIOS_TEST_AIOSD_PATH));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    const auto log = (root / "aiosd.log").string();
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, log.c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
    if (posix_spawn(&pid, AIOS_TEST_AIOSD_PATH, &fa, nullptr, argv.data(), environ) != 0) {
      pid = -1;
    }
    posix_spawn_file_actions_destroy(&fa);
  }

  void kill_hard() {
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      int status = 0;
      ::waitpid(pid, &status, 0);
      pid = -1;
    }
  }
  ~Node() { kill_hard(); }
};

std::vector<std::string> g_http_peers;

std::unique_ptr<aios::Session> session_for(const Node& n) {
  aios::SessionConfig cfg;
  cfg.endpoint = n.http();
  cfg.cluster_key = kClusterKey;
  cfg.socket_timeout_ms = 3000;
  cfg.redirect_peers = g_http_peers;  // no --admin here, so no allowlist refresh
  return std::make_unique<aios::Session>(cfg);
}

// GET /map as JSON, or null on any failure.
nlohmann::json get_map(const Node& n) {
  try {
    auto s = session_for(n);
    auto r = s->request("GET", "/map");
    if (r.status != 200) return nullptr;
    return nlohmann::json::parse(r.body);
  } catch (const std::exception&) {
    return nullptr;
  }
}

template <typename Pred>
bool wait_for(std::chrono::milliseconds timeout, Pred&& pred) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(200ms);
  }
  return pred();
}

}  // namespace

TEST(ProcessConsensus, LeaderPublishesMapAndMinorityLosesLease) {
  const auto root = aios::test::temp_root("aios-consensus");
  std::vector<std::unique_ptr<Node>> nodes;
  std::vector<std::string> monitors;
  for (int i = 0; i < 3; ++i) {
    auto n = std::make_unique<Node>();
    n->root = root / ("n" + std::to_string(i));
    n->id = "n" + std::to_string(i);
    n->rpc_port = free_port();
    n->http_port = free_port();
    monitors.push_back(n->rpc());
    g_http_peers.push_back(n->http());
    nodes.push_back(std::move(n));
  }
  for (int i = 0; i < 3; ++i) {
    nodes[i]->start(monitors, i == 0 ? std::string{} : nodes[0]->rpc());
    ASSERT_GT(nodes[i]->pid, 0);
  }
  auto logs = [&] {
    std::string all;
    for (auto& n : nodes) all += "---- " + n->id + "\n" + read_file(n->root / "aiosd.log");
    return all;
  };

  // A committed 3-target map with a valid lease on every node, and a small
  // monotonic epoch (the hash epochs were 64-bit random-looking numbers).
  ASSERT_TRUE(wait_for(30s, [&] {
    for (auto& n : nodes) {
      auto m = get_map(*n);
      if (m.is_null() || !m.value("consensus", false) || !m.value("lease_valid", false)) {
        return false;
      }
      if (!m.contains("targets") || m["targets"].size() != 3) return false;
      if (m.value("epoch", 0ull) == 0 || m.value("epoch", 0ull) > 1000) return false;
      if (m.value("active_epoch", 0ull) != m.value("epoch", 0ull)) return false;
    }
    return true;
  })) << logs();
  const auto m0 = get_map(*nodes[0]);
  const auto epoch0 = m0.value("epoch", 0ull);
  for (auto& n : nodes) EXPECT_EQ(get_map(*n).value("epoch", 0ull), epoch0) << n->id;

  // Writes through any node (redirects follow to the primary).
  try {
    auto s = session_for(*nodes[1]);
    for (int i = 0; i < 6; ++i) {
      const auto oid = "cons/obj" + std::to_string(i);
      s->put_bytes(oid, "v1-" + oid);
      EXPECT_EQ(s->get_object(oid).body, "v1-" + oid);
    }
  } catch (const std::exception& e) {
    FAIL() << "3-node writes: " << e.what() << "\nmap: " << get_map(*nodes[0]).dump() << "\n"
           << logs();
  }

  // Lose n2: after dead_after_ms the leader commits a 2-target map with a higher
  // epoch; the majority keeps a lease and keeps serving writes.
  nodes[2]->kill_hard();
  ASSERT_TRUE(wait_for(30s, [&] {
    for (int i = 0; i < 2; ++i) {
      auto m = get_map(*nodes[i]);
      if (m.is_null() || !m.value("lease_valid", false)) return false;
      if (!m.contains("targets") || m["targets"].size() != 2) return false;
      if (m.value("epoch", 0ull) <= epoch0) return false;
    }
    return true;
  })) << logs();
  try {
    auto s = session_for(*nodes[0]);
    for (int i = 0; i < 6; ++i) {
      const auto oid = "cons/after-loss" + std::to_string(i);
      s->put_bytes(oid, "v2");
      EXPECT_EQ(s->get_object(oid).body, "v2");
    }
  } catch (const std::exception& e) {
    FAIL() << "2-node writes: " << e.what() << "\nmap n0: " << get_map(*nodes[0]).dump()
           << "\nmap n1: " << get_map(*nodes[1]).dump() << "\n"
           << logs();
  }

  // Lose n1 too: n0 is a minority of one. Its lease lapses and it must refuse to
  // act as primary rather than keep writing on its own.
  nodes[1]->kill_hard();
  ASSERT_TRUE(wait_for(20s, [&] {
    auto m = get_map(*nodes[0]);
    return !m.is_null() && m.contains("lease_valid") && !m.value("lease_valid", true);
  })) << logs();
  {
    aios::SessionConfig cfg;
    cfg.endpoint = nodes[0]->http();
    cfg.cluster_key = kClusterKey;
    cfg.socket_timeout_ms = 3000;
    cfg.map_transition_wait_ms = 0;  // ask once, no retry loop
    cfg.redirect_peers = g_http_peers;
    aios::Session s(cfg);
    // n0 is primary for roughly half the keys; the others redirect to dead n1
    // (307, not followed with max_redirects=0). Find one that lands on n0.
    int saw_503 = 0;
    for (int i = 0; i < 32 && saw_503 == 0; ++i) {
      aios::HttpResponse r;
      try {
        r = s.request("PUT", "/o/cons%2Fminority" + std::to_string(i),
                      {{"content-type", "application/octet-stream"}}, "x", 0);
      } catch (const std::exception& e) {
        // Not primary here: 307 to dead n1, not followed with max_redirects=0.
        if (std::string(e.what()).find("too many redirects") != std::string::npos) continue;
        std::string peers;
        for (const auto& p : g_http_peers) peers += p + " ";
        FAIL() << "minority PUT: " << e.what() << "\npeers: " << peers
               << "\nmap n0: " << get_map(*nodes[0]).dump() << "\n"
               << logs();
      }
      if (r.status == 307) continue;
      EXPECT_EQ(r.status, 503) << r.body;
      EXPECT_NE(r.body.find("no_map_lease"), std::string::npos) << r.body;
      ++saw_503;
    }
    EXPECT_EQ(saw_503, 1);
  }

  // Bring n1 back: majority again, lease returns, writes resume.
  nodes[1]->pid = -1;
  nodes[1]->start(monitors, nodes[0]->rpc());
  ASSERT_GT(nodes[1]->pid, 0);
  ASSERT_TRUE(wait_for(40s, [&] {
    std::uint64_t e0 = 0;
    for (int i = 0; i < 2; ++i) {
      auto m = get_map(*nodes[i]);
      if (m.is_null() || !m.value("lease_valid", false) || !m.contains("targets") ||
          m["targets"].size() != 2 || m.value("active_epoch", 0ull) != m.value("epoch", 0ull)) {
        return false;
      }
      if (i == 0) e0 = m.value("epoch", 0ull);
      if (m.value("epoch", 0ull) != e0) return false;
    }
    return true;
  })) << logs();
  try {
    auto s = session_for(*nodes[0]);
    s->put_bytes("cons/recovered", "v3");
    EXPECT_EQ(s->get_object("cons/recovered").body, "v3");
    // Objects written by the two-node majority have both copies on n0/n1 and
    // are still there. (The three-node writes may have had a copy on n2; with
    // repair disabled in this test they are not necessarily readable here.)
    for (int i = 0; i < 6; ++i) {
      EXPECT_EQ(s->get_object("cons/after-loss" + std::to_string(i)).body, "v2") << i;
    }
  } catch (const std::exception& e) {
    std::this_thread::sleep_for(2s);
    FAIL() << "recovered writes: " << e.what() << "\nmap n0: " << get_map(*nodes[0]).dump()
           << "\nmap n1: " << get_map(*nodes[1]).dump() << "\nstatus n1: "
           << read_file(nodes[1]->root / "status.json") << "\n"
           << logs();
  }

  for (auto& n : nodes) n->kill_hard();
  std::filesystem::remove_all(root);
}
