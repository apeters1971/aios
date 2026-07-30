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
};

// Load YAML config file (optional). Returns false on parse failure.
bool load_config_file(const std::string& path, Config& cfg, std::string& err);

// Apply argv overrides. Returns false on bad args / --help.
bool parse_cli(int argc, char** argv, Config& cfg, std::string& err, bool& help);

// Split "host:port" into host and port. IPv6 not supported in v0 (host may not contain ':').
bool split_host_port(const std::string& addr, std::string& host, std::string& port);

}  // namespace aios
