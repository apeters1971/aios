// Process-level smoke test: spawns the real `aiosd` and `aios` binaries and
// drives them over the wire. Everything else in this suite is in-process, so
// this is the only place CLI parsing, daemon startup/shutdown, the status file,
// on-disk durability across a restart and the CLI tool are exercised end to end.
#include "test_helpers.hpp"
#include <gtest/gtest.h>

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

// Ask the kernel for a free TCP port. Tests in this binary run serially, so the
// window between close() and the daemon's bind() is not contended.
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

// A spawned child with stdout+stderr captured to a file.
struct Child {
  pid_t pid{-1};
  std::filesystem::path log;

  static Child spawn(const std::string& exe, const std::vector<std::string>& args,
                     const std::filesystem::path& log) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, log.c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
    Child c;
    c.log = log;
    if (posix_spawn(&c.pid, exe.c_str(), &fa, nullptr, argv.data(), environ) != 0) c.pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    return c;
  }

  bool running() const {
    if (pid <= 0) return false;
    int status = 0;
    return ::waitpid(pid, &status, WNOHANG) == 0;
  }

  // Wait up to `timeout` for exit; returns the raw wait status or -1 on timeout.
  int wait(std::chrono::milliseconds timeout) {
    if (pid <= 0) return -1;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const pid_t r = ::waitpid(pid, &status, WNOHANG);
      if (r == pid) {
        pid = -1;
        return status;
      }
      std::this_thread::sleep_for(20ms);
    }
    return -1;
  }

  void kill_hard() {
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      int status = 0;
      ::waitpid(pid, &status, 0);
      pid = -1;
    }
  }
};

// Runs a short-lived command to completion; returns its exit code (or -1).
int run_and_wait(const std::string& exe, const std::vector<std::string>& args,
                 const std::filesystem::path& log) {
  auto c = Child::spawn(exe, args, log);
  if (c.pid <= 0) return -1;
  const int status = c.wait(30s);
  if (status < 0) {
    c.kill_hard();
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

struct Daemon {
  std::filesystem::path root;
  std::filesystem::path scan_root;
  std::filesystem::path status_file;
  std::filesystem::path config_file;
  std::filesystem::path log;
  int rpc_port{0};
  int http_port{0};
  Child child;

  explicit Daemon(const std::filesystem::path& root) : root(root) {
    scan_root = root / "disk";
    status_file = root / "status.json";
    log = root / "aiosd.log";
    std::filesystem::create_directories(scan_root);
    std::ofstream(scan_root / ".aios") << "storage_class: nvme\nweight: 1\nstate: up\n";
    // Every client of this daemon (Session, the aios CLI) must sign its body
    // digest; UNSIGNED-PAYLOAD is refused.
    config_file = root / "aiosd.yaml";
    std::ofstream(config_file) << "http_require_signed_payload: true\n";
    rpc_port = free_port();
    http_port = free_port();
  }

  std::string endpoint() const { return "127.0.0.1:" + std::to_string(http_port); }

  void start() {
    child = Child::spawn(AIOS_TEST_AIOSD_PATH,
                         {"--config", config_file.string(),
                          "--cluster-key", kClusterKey, "--node-id", "smoke",
                          "--listen", "127.0.0.1:" + std::to_string(rpc_port),
                          "--http-listen", endpoint(),
                          "--status-file", status_file.string(),
                          "--scan-root", scan_root.string(),
                          "--replica-count", "1", "--write-quorum", "1",
                          "--no-fsync"},
                         log);
    ASSERT_GT(child.pid, 0) << "posix_spawn failed for " << AIOS_TEST_AIOSD_PATH;
  }

  // Poll the HTTP API until the daemon answers an authenticated request.
  bool wait_ready(std::chrono::milliseconds timeout = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    aios::SessionConfig cfg;
    cfg.endpoint = endpoint();
    cfg.cluster_key = kClusterKey;
    cfg.socket_timeout_ms = 1000;
    while (std::chrono::steady_clock::now() < deadline) {
      if (!child.running()) return false;
      try {
        aios::Session s(cfg);
        auto r = s.request("GET", "/o?prefix=smoke/&limit=1");
        if (r.status == 200) return true;
      } catch (const std::exception&) {
      }
      std::this_thread::sleep_for(100ms);
    }
    return false;
  }

  // SIGTERM and expect a clean exit(0) within the timeout.
  int stop_gracefully(std::chrono::milliseconds timeout = 15s) {
    if (child.pid <= 0) return -1;
    ::kill(child.pid, SIGTERM);
    const int status = child.wait(timeout);
    if (status < 0) {
      child.kill_hard();
      return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }

  ~Daemon() { child.kill_hard(); }
};

std::unique_ptr<aios::Session> make_session(const Daemon& d) {
  aios::SessionConfig cfg;
  cfg.endpoint = d.endpoint();
  cfg.cluster_key = kClusterKey;
  cfg.socket_timeout_ms = 5000;
  return std::make_unique<aios::Session>(cfg);
}

}  // namespace

TEST(ProcessSmoke, VersionFlagsPrintAndExitZero) {
  const auto root = aios::test::temp_root("aios-smoke-version");
  ASSERT_EQ(run_and_wait(AIOS_TEST_AIOSD_PATH, {"--version"}, root / "aiosd.txt"), 0);
  ASSERT_EQ(run_and_wait(AIOS_TEST_AIOS_CLI_PATH, {"--version"}, root / "aios.txt"), 0);
  EXPECT_EQ(read_file(root / "aiosd.txt").rfind("aiosd ", 0), 0u) << read_file(root / "aiosd.txt");
  EXPECT_EQ(read_file(root / "aios.txt").rfind("aios ", 0), 0u) << read_file(root / "aios.txt");

  // A usage error must be reported, not crash: exit 2 and no daemon left behind.
  EXPECT_EQ(run_and_wait(AIOS_TEST_AIOSD_PATH, {"--replica-count", "many"}, root / "bad.txt"), 2);
  EXPECT_NE(read_file(root / "bad.txt").find("invalid integer"), std::string::npos);
  std::filesystem::remove_all(root);
}

TEST(ProcessSmoke, DaemonServesObjectsWritesStatusAndShutsDownCleanly) {
  const auto root = aios::test::temp_root("aios-smoke-daemon");
  Daemon d(root);
  ASSERT_NO_FATAL_FAILURE(d.start());
  ASSERT_TRUE(d.wait_ready()) << "daemon did not become ready:\n" << read_file(d.log);

  const std::string oid = "smoke/hello";
  const std::string body(300 * 1024, 'S');
  {
    auto s = make_session(d);
    s->put_bytes(oid, body);
    auto got = s->get_object(oid);
    ASSERT_TRUE(got.exists);
    EXPECT_EQ(got.body, body);

    auto listed = s->list_prefix("smoke/");
    ASSERT_EQ(listed.objects.size(), 1u);
    EXPECT_EQ(listed.objects[0].oid, oid);
    EXPECT_EQ(listed.objects[0].size, body.size());

    // Wrong key must be refused by the daemon, not just fail to parse.
    aios::SessionConfig bad = s->config();
    bad.cluster_key = "00000000-0000-0000-0000-000000000000";
    aios::Session unauth(bad);
    EXPECT_EQ(unauth.request("GET", "/o/" + oid).status, 401);
  }

  // The CLI tool round-trips a file through the same daemon.
  {
    const auto in = root / "in.bin";
    const auto out = root / "out.bin";
    std::ofstream(in, std::ios::binary) << std::string(70000, 'c');
    const std::vector<std::string> common{"--cluster-key", kClusterKey, "--endpoint", d.endpoint()};
    auto put_args = common;
    put_args.insert(put_args.end(), {"put", "smoke/cli", in.string()});
    ASSERT_EQ(run_and_wait(AIOS_TEST_AIOS_CLI_PATH, put_args, root / "cli-put.txt"), 0)
        << read_file(root / "cli-put.txt");
    auto get_args = common;
    get_args.insert(get_args.end(), {"get", "smoke/cli", "-o", out.string()});
    ASSERT_EQ(run_and_wait(AIOS_TEST_AIOS_CLI_PATH, get_args, root / "cli-get.txt"), 0)
        << read_file(root / "cli-get.txt");
    EXPECT_EQ(read_file(out), std::string(70000, 'c'));
  }

  // Ticket auth through the real binaries: create a principal with the shared
  // key, then drive the daemon as that principal (ticket grant + session key
  // signing in the CLI), including a cap violation and a wrong key.
  std::string principal_key;
  {
    const std::vector<std::string> shared{"--cluster-key", kClusterKey, "--endpoint", d.endpoint()};
    auto create = shared;
    create.insert(create.end(), {"admin", "principal", "create", "client.smoke", "--caps", "smoke/"});
    ASSERT_EQ(run_and_wait(AIOS_TEST_AIOS_CLI_PATH, create, root / "principal-create.txt"), 0)
        << read_file(root / "principal-create.txt");
    const auto created = read_file(root / "principal-create.txt");
    const auto kpos = created.find("key:   ");
    ASSERT_NE(kpos, std::string::npos) << created;
    const std::string key = created.substr(kpos + 7, 64);
    ASSERT_EQ(key.size(), 64u);
    principal_key = key;

    const std::vector<std::string> as_smoke{"--principal", "client.smoke", "--key", key,
                                            "--endpoint", d.endpoint()};
    const auto in = root / "smoke-in.bin";
    const auto out = root / "smoke-out.bin";
    std::ofstream(in, std::ios::binary) << std::string(1234, 'p');
    auto put_args = as_smoke;
    put_args.insert(put_args.end(), {"put", "smoke/principal", in.string()});
    ASSERT_EQ(run_and_wait(AIOS_TEST_AIOS_CLI_PATH, put_args, root / "p-put.txt"), 0)
        << read_file(root / "p-put.txt");
    auto get_args = as_smoke;
    get_args.insert(get_args.end(), {"get", "smoke/principal", "-o", out.string()});
    ASSERT_EQ(run_and_wait(AIOS_TEST_AIOS_CLI_PATH, get_args, root / "p-get.txt"), 0)
        << read_file(root / "p-get.txt");
    EXPECT_EQ(read_file(out), std::string(1234, 'p'));

    // Outside the caps: refused by the daemon, reported by the CLI.
    auto denied = as_smoke;
    denied.insert(denied.end(), {"put", "other/x", in.string()});
    EXPECT_NE(run_and_wait(AIOS_TEST_AIOS_CLI_PATH, denied, root / "p-denied.txt"), 0);
    EXPECT_NE(read_file(root / "p-denied.txt").find("403"), std::string::npos)
        << read_file(root / "p-denied.txt");

    // Wrong key: no ticket, clear message, non-zero exit.
    std::vector<std::string> wrong{"--principal", "client.smoke", "--key", std::string(64, '0'),
                                   "--endpoint", d.endpoint(), "stat", "smoke/principal"};
    EXPECT_NE(run_and_wait(AIOS_TEST_AIOS_CLI_PATH, wrong, root / "p-wrong.txt"), 0);
    EXPECT_NE(read_file(root / "p-wrong.txt").find("authentication as client.smoke failed"),
              std::string::npos)
        << read_file(root / "p-wrong.txt");

    auto list = shared;
    list.insert(list.end(), {"admin", "principal", "list"});
    ASSERT_EQ(run_and_wait(AIOS_TEST_AIOS_CLI_PATH, list, root / "p-list.txt"), 0);
    EXPECT_NE(read_file(root / "p-list.txt").find("client.smoke  role=client  caps=smoke/"),
              std::string::npos)
        << read_file(root / "p-list.txt");
  }

  // Status file is written atomically and lists this node as online.
  {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    nlohmann::json st;
    while (std::chrono::steady_clock::now() < deadline) {
      if (std::filesystem::exists(d.status_file)) {
        try {
          st = nlohmann::json::parse(read_file(d.status_file));
          if (st.contains("membership") && st["membership"].contains("members")) break;
        } catch (const std::exception&) {
        }
      }
      std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(st.contains("membership") && st["membership"].contains("members")) << st.dump();
    EXPECT_EQ(st.value("node_id", ""), "smoke");
    EXPECT_GE(st["cluster_map"].value("targets", nlohmann::json::array()).size(), 1u)
        << "scan root was not advertised as a target: " << st["cluster_map"].dump();
    bool seen = false;
    for (const auto& m : st["membership"]["members"]) {
      if (m.value("node_id", "") == "smoke") {
        seen = true;
        EXPECT_EQ(m.value("state", ""), "online");
      }
    }
    EXPECT_TRUE(seen) << st.dump();
  }

  // Graceful shutdown: exit 0 on SIGTERM, and the object survives a restart on
  // the same disk — the durability path no in-process test can cover.
  ASSERT_EQ(d.stop_gracefully(), 0) << read_file(d.log);
  {
    const auto log = read_file(d.log);
    EXPECT_EQ(log.find("fatal"), std::string::npos) << log;
    EXPECT_NE(log.find("aiosd stopped"), std::string::npos) << log;
  }

  ASSERT_NO_FATAL_FAILURE(d.start());
  ASSERT_TRUE(d.wait_ready()) << "daemon did not restart:\n" << read_file(d.log);
  {
    auto s = make_session(d);
    auto got = s->get_object(oid);
    ASSERT_TRUE(got.exists) << "object lost across restart";
    EXPECT_EQ(got.body, body);
    EXPECT_EQ(s->get_object("smoke/cli").body, std::string(70000, 'c'));

    s->delete_object(oid);
    EXPECT_FALSE(s->head_object(oid).exists);
    auto listed = s->list_prefix("smoke/");
    ASSERT_EQ(listed.objects.size(), 2u);
    EXPECT_EQ(listed.objects[0].oid, "smoke/cli");
    EXPECT_EQ(listed.objects[1].oid, "smoke/principal");

    // The keyring survived the restart too: the principal can still log in.
    aios::SessionConfig pc;
    pc.endpoint = d.endpoint();
    pc.principal = "client.smoke";
    pc.principal_key = principal_key;
    aios::Session ps(pc);
    EXPECT_EQ(ps.get_object("smoke/principal").body, std::string(1234, 'p'));
  }
  ASSERT_EQ(d.stop_gracefully(), 0) << read_file(d.log);
  std::filesystem::remove_all(root);
}
