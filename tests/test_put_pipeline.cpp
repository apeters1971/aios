#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include "cluster/place.hpp"
#include "http/http_auth.hpp"
#include "http/http_server.hpp"
#include "net/object_client.hpp"
#include "net/server.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using tcp = boost::asio::ip::tcp;
using aios::test::DualStoreFixture;
using aios::test::temp_root;

std::vector<std::uint8_t> make_payload(std::size_t n, std::uint8_t seed = 0x5A) {
  std::vector<std::uint8_t> out(n);
  for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>(seed + (i % 251));
  return out;
}

struct HttpResponse {
  int status{0};
  std::string body;
};

void add_auth(std::unordered_map<std::string, std::string>& headers, const std::string& method,
              const std::string& target, const std::string& cluster_key) {
  const std::string date = std::to_string(aios::now_ms());
  headers["x-aios-date"] = date;
  headers["x-aios-content-sha256"] = "UNSIGNED-PAYLOAD";
  const std::string signed_headers = "x-aios-content-sha256;x-aios-date";
  const auto canon =
      aios::http_canonical(method, target, date, signed_headers, headers, "UNSIGNED-PAYLOAD");
  const auto sig = aios::http_sign(cluster_key, canon);
  headers["authorization"] = "AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=" +
                             signed_headers + ", Signature=" + sig;
}

HttpResponse http_put_large(const std::string& host, const std::string& port,
                            const std::string& target, const std::vector<std::uint8_t>& body,
                            const std::string& cluster_key) {
  HttpResponse resp;
  std::unordered_map<std::string, std::string> headers;
  headers["content-length"] = std::to_string(body.size());
  add_auth(headers, "PUT", target, cluster_key);

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  boost::system::error_code ec;
  auto endpoints = resolver.resolve(host, port, ec);
  if (ec) {
    resp.status = -1;
    resp.body = ec.message();
    return resp;
  }
  tcp::socket sock(ioc);
  for (int attempt = 0; attempt < 50; ++attempt) {
    boost::asio::connect(sock, endpoints, ec);
    if (!ec) break;
    sock = tcp::socket(ioc);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (ec) {
    resp.status = -1;
    resp.body = ec.message();
    return resp;
  }

  std::ostringstream req;
  req << "PUT " << target << " HTTP/1.1\r\n";
  req << "Host: " << host << ':' << port << "\r\n";
  req << "Connection: close\r\n";
  for (const auto& [k, v] : headers) req << k << ": " << v << "\r\n";
  req << "\r\n";
  boost::asio::write(sock, boost::asio::buffer(req.str()), ec);
  if (!ec && !body.empty()) {
    boost::asio::write(sock, boost::asio::buffer(body), ec);
  }
  if (ec) {
    resp.status = -1;
    resp.body = ec.message();
    return resp;
  }

  boost::asio::streambuf buf;
  boost::asio::read_until(sock, buf, "\r\n\r\n", ec);
  if (ec && ec != boost::asio::error::eof) {
    resp.status = -1;
    resp.body = ec.message();
    return resp;
  }
  std::istream is(&buf);
  std::string status_line;
  std::getline(is, status_line);
  if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();
  {
    std::istringstream ss(status_line);
    std::string ver, reason;
    ss >> ver >> resp.status;
  }
  std::string line;
  std::size_t content_length = 0;
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto name = line.substr(0, colon);
    auto value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name == "content-length") {
      try {
        content_length = static_cast<std::size_t>(std::stoull(value));
      } catch (...) {
      }
    }
  }
  std::string already(std::istreambuf_iterator<char>(is), {});
  resp.body = already;
  while (content_length > 0 && resp.body.size() < content_length) {
    char tmp[4096];
    const auto n = sock.read_some(boost::asio::buffer(tmp), ec);
    if (n > 0) resp.body.append(tmp, tmp + n);
    if (ec) break;
  }
  return resp;
}

// Minimal 3-node RPC cluster (copied pattern from mini_cluster tests).
struct MiniNode {
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
    EXPECT_TRUE(aios::split_host_port(addr, host, port));
    aios::RpcHandlers handlers;
    handlers.local_node_id = id;
    handlers.local_listen = addr;
    handlers.cluster_key = cfg.cluster_key;
    handlers.auth_skew_ms = cfg.auth_skew_ms;
    handlers.on_object = [this](const aios::Frame& req) { return svc->handle(req); };
    server = std::make_unique<aios::TcpServer>(ioc, host, port, std::move(handlers));
    server->start();
    work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
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
    work.reset();
    ioc.stop();
    if (thr.joinable()) thr.join();
    server.reset();
    svc.reset();
    running = false;
  }
};

struct MiniCluster {
  std::filesystem::path root;
  aios::ClusterMap map;
  std::vector<std::unique_ptr<MiniNode>> nodes;
  std::string cluster_key{"550e8400-e29b-41d4-a716-446655440000"};

  MiniCluster(int n, int replica_count, int write_quorum) {
    root = temp_root("aios-pipe-cluster");
    const int base_port = 19300 + static_cast<int>(::getpid() % 400);
    aios::MembershipTable membership;
    aios::FsTable fs_table;
    std::vector<aios::FsEntry> remotes;
    for (int i = 0; i < n; ++i) {
      auto node = std::make_unique<MiniNode>();
      node->id = "node-" + std::string(1, static_cast<char>('a' + i));
      node->addr = "127.0.0.1:" + std::to_string(base_port + i);
      const auto path = root / node->id / "aios";
      std::filesystem::create_directories(path);
      node->aios_path = path.string();
      node->cfg.node_id = node->id;
      node->cfg.listen = node->addr;
      node->cfg.cluster_key = cluster_key;
      node->cfg.replica_count = replica_count;
      node->cfg.write_quorum = write_quorum;
      node->cfg.auth_skew_ms = 60000;
      node->cfg.clone_required = false;
      node->cfg.max_versions = 16;
      node->cfg.compression = "none";
      if (i == 0) {
        membership.set_local(node->id, node->addr);
        aios::AiosTarget t;
        t.mount = path.string();
        t.target_path = path.string();
        t.aios_path = node->aios_path;
        t.storage_class = "nvme";
        t.usable = true;
        t.bavail = 1000;
        fs_table.set_local(node->id, {t});
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

  ~MiniCluster() {
    for (auto& node : nodes) node->stop();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  MiniNode* by_id(const std::string& id) {
    for (auto& n : nodes) {
      if (n->id == id) return n.get();
    }
    return nullptr;
  }

  MiniNode* primary_for(const std::string& oid) {
    auto p = aios::place(oid, map, "nvme");
    if (p.acting_set.empty()) return nullptr;
    return by_id(p.acting_set[0].node_id);
  }
};

}  // namespace

TEST(ObjectStorePipeline, PeekAndPrepareAtSeq) {
  using namespace aios;
  const auto base = temp_root("aios-pipe-store");
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.inline_max_bytes = 64;
  ObjectStore store;
  std::string err;
  ASSERT_TRUE(store.open(base.string(), opts, err)) << err;

  std::uint64_t seq = 0, tip = 0;
  ASSERT_TRUE(store.peek_next_seq("pipe-a", seq, tip, err)) << err;
  EXPECT_EQ(tip, 0u);
  EXPECT_EQ(seq, 1u);

  std::string staging;
  ASSERT_TRUE(store.create_staging_file("pipe-a", staging, err)) << err;
  const auto payload = make_payload(4096);
  {
    std::ofstream out(staging, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));
  }
  const auto crc = crc32c(payload.data(), payload.size());
  PreparedVersion pv;
  ASSERT_TRUE(store.prepare_put_file_at_seq("pipe-a", seq, tip, staging, payload.size(), crc, {},
                                            true, std::nullopt, pv, err))
      << err;
  EXPECT_EQ(pv.seq, seq);
  ASSERT_TRUE(store.publish_tip("pipe-a", pv.seq, err)) << err;

  std::uint64_t seq2 = 0, tip2 = 0;
  ASSERT_TRUE(store.peek_next_seq("pipe-a", seq2, tip2, err)) << err;
  EXPECT_EQ(tip2, seq);
  EXPECT_EQ(seq2, seq + 1);

  // Tip changed → at_seq rejects stale prev_tip.
  std::string staging2;
  ASSERT_TRUE(store.create_staging_file("pipe-a", staging2, err)) << err;
  {
    std::ofstream out(staging2, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));
  }
  PreparedVersion bad;
  EXPECT_FALSE(store.prepare_put_file_at_seq("pipe-a", seq2, tip /*stale*/, staging2,
                                             payload.size(), crc, {}, true, std::nullopt, bad,
                                             err));
  EXPECT_EQ(err, "tip changed during upload");

  store.close();
  std::filesystem::remove_all(base);
}

TEST(PutPipeline, LocalReplicaHappyPathAndConflict) {
  using namespace aios;
  DualStoreFixture fx("aios-pipe-local");
  fx.cfg.compression = "none";
  const std::string oid = "pipe/local-1";
  const auto payload = make_payload(512 * 1024);
  const auto crc = crc32c(payload.data(), payload.size());

  std::string staging;
  auto br = fx.svc->api_begin_put_pipeline(oid, {}, payload.size(), staging);
  ASSERT_TRUE(br.ok) << br.code << " " << br.error;
  EXPECT_FALSE(staging.empty());

  auto conflict = fx.svc->api_begin_put_pipeline(oid, {}, payload.size(), staging);
  EXPECT_FALSE(conflict.ok);
  EXPECT_EQ(conflict.code, "conflict");

  constexpr std::size_t kChunk = 64 * 1024;
  for (std::size_t off = 0; off < payload.size(); off += kChunk) {
    const auto n = std::min(kChunk, payload.size() - off);
    auto dr = fx.svc->api_put_pipeline_data(oid, off, payload.data() + off, n);
    ASSERT_TRUE(dr.ok) << dr.code << " " << dr.error << " off=" << off;
  }

  auto fin = fx.svc->api_put_pipeline_finish(oid, {{"k", "v"}}, true, {}, crc);
  ASSERT_TRUE(fin.ok) << fin.code << " " << fin.error;
  EXPECT_GE(fin.replicas, 2);
  ASSERT_TRUE(fin.info);
  EXPECT_EQ(fin.info->size, payload.size());
  EXPECT_EQ(fin.info->crc32c, crc);

  auto got = fx.svc->api_get(oid, std::nullopt, std::nullopt, {});
  ASSERT_TRUE(got.ok) << got.code << " " << got.error;
  if (got.data) {
    EXPECT_EQ(*got.data, payload);
  } else {
    ASSERT_FALSE(got.body_path.empty());
    std::ifstream in(got.body_path, std::ios::binary);
    std::vector<std::uint8_t> disk((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(disk, payload);
  }
}

TEST(PutPipeline, AbortCleansUp) {
  using namespace aios;
  DualStoreFixture fx("aios-pipe-abort");
  const std::string oid = "pipe/abort-1";
  const auto payload = make_payload(300 * 1024);

  std::string staging;
  ASSERT_TRUE(fx.svc->api_begin_put_pipeline(oid, {}, payload.size(), staging).ok);
  ASSERT_TRUE(fx.svc->api_put_pipeline_data(oid, 0, payload.data(), 64 * 1024).ok);
  ASSERT_TRUE(fx.svc->api_put_pipeline_abort(oid).ok);

  // New pipeline can start after abort.
  staging.clear();
  auto br = fx.svc->api_begin_put_pipeline(oid, {}, payload.size(), staging);
  ASSERT_TRUE(br.ok) << br.code << " " << br.error;
  for (std::size_t off = 0; off < payload.size();) {
    const auto n = std::min<std::size_t>(64 * 1024, payload.size() - off);
    ASSERT_TRUE(fx.svc->api_put_pipeline_data(oid, off, payload.data() + off, n).ok);
    off += n;
  }
  auto fin = fx.svc->api_put_pipeline_finish(oid, {}, true, {});
  ASSERT_TRUE(fin.ok) << fin.code << " " << fin.error;
}

TEST(PutPipeline, EcNotSupported) {
  using namespace aios;
  aios::test::EcFixture fx("aios-pipe-ec");
  std::string staging;
  auto br = fx.svc->api_begin_put_pipeline("pipe/ec", {}, 1024, staging);
  EXPECT_FALSE(br.ok);
  EXPECT_EQ(br.code, "not_supported");
}

TEST(PutPipeline, MiniClusterRemotePeers) {
  using namespace aios;
  MiniCluster cluster(3, 3, 2);
  const std::string oid = "pipe/remote-1";
  auto* primary = cluster.primary_for(oid);
  ASSERT_NE(primary, nullptr);
  const auto payload = make_payload(400 * 1024, 0x11);
  const auto crc = crc32c(payload.data(), payload.size());

  std::string staging;
  auto br = primary->svc->api_begin_put_pipeline(oid, {}, payload.size(), staging);
  ASSERT_TRUE(br.ok) << br.code << " " << br.error;

  constexpr std::size_t kChunk = 32 * 1024;
  for (std::size_t off = 0; off < payload.size(); off += kChunk) {
    const auto n = std::min(kChunk, payload.size() - off);
    auto dr = primary->svc->api_put_pipeline_data(oid, off, payload.data() + off, n);
    ASSERT_TRUE(dr.ok) << dr.code << " " << dr.error;
  }
  auto fin = primary->svc->api_put_pipeline_finish(oid, {{"via", "pipe"}}, true, {}, crc);
  ASSERT_TRUE(fin.ok) << fin.code << " " << fin.error;
  EXPECT_GE(fin.replicas, 2);

  auto placement = place(oid, cluster.map, "nvme");
  for (const auto& t : placement.acting_set) {
    auto* n = cluster.by_id(t.node_id);
    ASSERT_NE(n, nullptr);
    std::string err;
    auto st = n->stores.get(t.aios_path)->stat(oid, err);
    ASSERT_TRUE(st.has_value()) << t.node_id << " " << err;
    EXPECT_EQ(st->size, payload.size());
    EXPECT_EQ(st->crc32c, crc);
  }
}

TEST(PutPipeline, HttpLargePut) {
  using namespace aios;
  DualStoreFixture fx("aios-pipe-http");
  fx.cfg.compression = "none";
  const int port_num = 18200 + static_cast<int>(::getpid() % 1000);
  const std::string port = std::to_string(port_num);
  fx.cfg.http_listen = "127.0.0.1:" + port;

  boost::asio::io_context ioc;
  HttpServer http(ioc, fx.cfg, *fx.svc, fx.membership);
  http.start();
  std::thread th([&] { ioc.run(); });

  const auto payload = make_payload(300 * 1024, 0x22);
  auto r = http_put_large("127.0.0.1", port, "/o/pipe-http-1", payload, fx.cfg.cluster_key);
  EXPECT_EQ(r.status, 204) << r.body;

  auto got = fx.svc->api_get("pipe-http-1", std::nullopt, std::nullopt, {});
  ASSERT_TRUE(got.ok) << got.code << " " << got.error;
  if (got.data) {
    EXPECT_EQ(*got.data, payload);
  } else {
    ASSERT_FALSE(got.body_path.empty());
    std::ifstream in(got.body_path, std::ios::binary);
    std::vector<std::uint8_t> disk((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(disk, payload);
  }

  ioc.stop();
  if (th.joinable()) th.join();
}
