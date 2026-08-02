#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aios {

struct Config {
  std::string node_id;
  std::string listen{"0.0.0.0:7400"};
  std::vector<std::string> peers;
  // Shared cluster secret (UUID recommended). Required for Hello/Gossip HMAC.
  std::string cluster_key;
  int auth_skew_ms{60000};  // reject |now-ts| larger than this
  int gossip_interval_ms{1000};
  int suspect_after_ms{5000};
  int dead_after_ms{15000};
  int scan_interval_ms{5000};
  std::string status_file;
  // Server-side replication: primary fans out to replica_count targets.
  int replica_count{3};
  // Successful copies required for Put/Del ACK (including primary). 0 => replica_count.
  int write_quorum{0};
  // Default layout when a PUT omits x-aios-layout ("replica" or "ec").
  // Alias name in docs: default_layout; YAML/CLI still use `durability`.
  std::string durability{"replica"};
  // Default EC parameters when layout is ec (request may override).
  int ec_k{2};
  int ec_m{1};
  // EC codec: "" (auto: xor if m==1 else isal), "xor", or "isal".
  std::string ec_codec;
  // Safety caps for per-request layout (0 = no cap).
  int max_ec_k{16};
  int max_ec_m{16};
  int max_replica_count{64};
  int repair_interval_ms{30000};
  // Max oids scanned per local store each repair tick.
  int repair_batch_oids{256};
  // HTTP object API listen address; empty disables HTTP front-end.
  std::string http_listen{"0.0.0.0:7480"};
  // Body durability for ranged FS puts: none | data | full (informational; store fsyncs).
  std::string http_body_sync{"data"};
  // Retain newest N object versions per oid (default 16).
  int max_versions{16};
  // If true, FS COW requires reflink/clonefile; if false, allow full-copy fallback.
  bool clone_required{true};
  // Max object body size for HTTP streaming PUT (bytes). Default 64 GiB.
  std::uint64_t max_object_bytes{64ull * 1024ull * 1024ull * 1024ull};
};

// Build dialable http host:port from TCP advertise + http_listen.
std::string derive_http_addr(const std::string& advertise_tcp, const std::string& http_listen);

// Load YAML config file (optional). Returns false on parse failure.
bool load_config_file(const std::string& path, Config& cfg, std::string& err);

// Apply argv overrides. Returns false on bad args / --help.
bool parse_cli(int argc, char** argv, Config& cfg, std::string& err, bool& help);

// Validate durability/EC knobs and derive replica_count for EC profiles.
bool normalize_config(Config& cfg, std::string& err);

// Split "host:port" into host and port. IPv6 not supported in v0 (host may not contain ':').
bool split_host_port(const std::string& addr, std::string& host, std::string& port);

}  // namespace aios
