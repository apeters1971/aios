#include "config.hpp"

#include "node_id.hpp"

#include <yaml-cpp/yaml.h>

#include <string>

namespace aios {

bool split_host_port(const std::string& addr, std::string& host, std::string& port) {
  const auto pos = addr.rfind(':');
  if (pos == std::string::npos || pos == 0 || pos + 1 >= addr.size()) {
    return false;
  }
  host = addr.substr(0, pos);
  port = addr.substr(pos + 1);
  return !host.empty() && !port.empty();
}

std::string derive_http_addr(const std::string& advertise_tcp, const std::string& http_listen) {
  if (http_listen.empty()) return {};
  std::string adv_host, adv_port, http_host, http_port;
  if (!split_host_port(http_listen, http_host, http_port)) return {};
  if (!advertise_tcp.empty() && split_host_port(advertise_tcp, adv_host, adv_port) &&
      !adv_host.empty() && adv_host != "0.0.0.0" && adv_host != "::") {
    return adv_host + ":" + http_port;
  }
  if (http_host == "0.0.0.0" || http_host == "::") {
    return "127.0.0.1:" + http_port;
  }
  return http_host + ":" + http_port;
}

bool load_config_file(const std::string& path, Config& cfg, std::string& err) {
  try {
    YAML::Node root = YAML::LoadFile(path);
    if (!root || !root.IsMap()) {
      err = "config root must be a mapping";
      return false;
    }
    if (root["node_id"]) cfg.node_id = root["node_id"].as<std::string>();
    if (root["listen"]) cfg.listen = root["listen"].as<std::string>();
    if (root["gossip_interval_ms"])
      cfg.gossip_interval_ms = root["gossip_interval_ms"].as<int>();
    if (root["suspect_after_ms"])
      cfg.suspect_after_ms = root["suspect_after_ms"].as<int>();
    if (root["dead_after_ms"]) cfg.dead_after_ms = root["dead_after_ms"].as<int>();
    if (root["scan_interval_ms"])
      cfg.scan_interval_ms = root["scan_interval_ms"].as<int>();
    if (root["status_file"]) cfg.status_file = root["status_file"].as<std::string>();
    if (root["cluster_key"]) cfg.cluster_key = root["cluster_key"].as<std::string>();
    if (root["auth_skew_ms"]) cfg.auth_skew_ms = root["auth_skew_ms"].as<int>();
    if (root["replica_count"]) cfg.replica_count = root["replica_count"].as<int>();
    if (root["write_quorum"]) cfg.write_quorum = root["write_quorum"].as<int>();
    if (root["repair_interval_ms"])
      cfg.repair_interval_ms = root["repair_interval_ms"].as<int>();
    if (root["repair_batch_oids"])
      cfg.repair_batch_oids = root["repair_batch_oids"].as<int>();
    if (root["http_listen"]) cfg.http_listen = root["http_listen"].as<std::string>();
    if (root["http_body_sync"])
      cfg.http_body_sync = root["http_body_sync"].as<std::string>();
    if (root["max_versions"]) cfg.max_versions = root["max_versions"].as<int>();
    if (root["clone_required"]) cfg.clone_required = root["clone_required"].as<bool>();
    if (root["max_object_bytes"])
      cfg.max_object_bytes = root["max_object_bytes"].as<std::uint64_t>();
    if (root["peers"]) {
      cfg.peers.clear();
      for (const auto& p : root["peers"]) {
        cfg.peers.push_back(p.as<std::string>());
      }
    }
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
  return true;
}

bool parse_cli(int argc, char** argv, Config& cfg, std::string& err, bool& help) {
  help = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        err = std::string("missing value for ") + name;
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      help = true;
      continue;
    }
    if (arg == "--config" || arg == "-c") {
      const char* v = need("--config");
      if (!v) return false;
      if (!load_config_file(v, cfg, err)) return false;
      continue;
    }
    if (arg == "--listen") {
      const char* v = need("--listen");
      if (!v) return false;
      cfg.listen = v;
      continue;
    }
    if (arg == "--peer") {
      const char* v = need("--peer");
      if (!v) return false;
      cfg.peers.emplace_back(v);
      continue;
    }
    if (arg == "--node-id") {
      const char* v = need("--node-id");
      if (!v) return false;
      cfg.node_id = v;
      continue;
    }
    if (arg == "--status-file") {
      const char* v = need("--status-file");
      if (!v) return false;
      cfg.status_file = v;
      continue;
    }
    if (arg == "--cluster-key") {
      const char* v = need("--cluster-key");
      if (!v) return false;
      cfg.cluster_key = v;
      continue;
    }
    if (arg == "--replica-count") {
      const char* v = need("--replica-count");
      if (!v) return false;
      cfg.replica_count = std::stoi(v);
      continue;
    }
    if (arg == "--write-quorum") {
      const char* v = need("--write-quorum");
      if (!v) return false;
      cfg.write_quorum = std::stoi(v);
      continue;
    }
    if (arg == "--http-listen") {
      const char* v = need("--http-listen");
      if (!v) return false;
      cfg.http_listen = v;
      continue;
    }
    err = "unknown argument: " + arg;
    return false;
  }
  if (help) {
    return true;
  }
  if (cfg.node_id.empty()) {
    cfg.node_id = default_hostname();
  }
  if (cfg.cluster_key.empty()) {
    err = "cluster_key is required (--cluster-key or config cluster_key)";
    return false;
  }
  return true;
}

}  // namespace aios
