#include "config.hpp"

#include "ec/codec_factory.hpp"
#include "node_id.hpp"

#include <yaml-cpp/yaml.h>

#include <cctype>
#include <string>

namespace aios {
namespace {

std::string lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool validate_ec_codec_choice(int m, std::string& codec, std::string& err) {
  if (codec.empty()) codec = (m == 1) ? "xor" : "isal";
  if (codec == "xor") {
    if (m != 1) {
      err = "ec_codec=xor requires ec_m=1";
      return false;
    }
  } else if (codec == "isal" || codec == "rs") {
    if (!isal_ec_available()) {
      err = "ec_codec=isal requires a build with ISA-L (AIOS_WITH_ISAL + libisal)";
      return false;
    }
    codec = "isal";
  } else {
    err = "ec_codec must be empty, 'xor', or 'isal'";
    return false;
  }
  return true;
}

bool validate_layout_rule(LayoutRule& rule, const Config& cfg, std::string& err) {
  rule.layout = lower_copy(rule.layout);
  if (rule.layout != "replica" && rule.layout != "ec") {
    err = "layout_rules entry layout must be 'replica' or 'ec'";
    return false;
  }
  if (rule.layout == "replica") return true;

  const int k = rule.ec_k.value_or(cfg.ec_k);
  const int m = rule.ec_m.value_or(cfg.ec_m);
  if (k < 1 || m < 1) {
    err = "layout_rules ec_k and ec_m must be >= 1";
    return false;
  }
  if (cfg.max_ec_k > 0 && k > cfg.max_ec_k) {
    err = "layout_rules ec_k exceeds max_ec_k";
    return false;
  }
  if (cfg.max_ec_m > 0 && m > cfg.max_ec_m) {
    err = "layout_rules ec_m exceeds max_ec_m";
    return false;
  }
  std::string codec = rule.ec_codec.value_or(cfg.ec_codec);
  if (!validate_ec_codec_choice(m, codec, err)) {
    err = "layout_rules: " + err;
    return false;
  }
  if (cfg.max_replica_count > 0 && k + m > cfg.max_replica_count) {
    err = "layout_rules k+m exceeds max_replica_count";
    return false;
  }
  // Normalize resolved codec onto the rule when the rule omitted it.
  if (!rule.ec_codec.has_value()) {
    // leave unset so resolve can still inherit cluster codec; validation already ok
  } else {
    rule.ec_codec = codec;
  }
  return true;
}

}  // namespace

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
    if (root["durability"]) cfg.durability = root["durability"].as<std::string>();
    if (root["default_layout"]) cfg.durability = root["default_layout"].as<std::string>();
    if (root["ec_k"]) cfg.ec_k = root["ec_k"].as<int>();
    if (root["ec_m"]) cfg.ec_m = root["ec_m"].as<int>();
    if (root["ec_codec"]) cfg.ec_codec = root["ec_codec"].as<std::string>();
    if (root["default_ec_k"]) cfg.ec_k = root["default_ec_k"].as<int>();
    if (root["default_ec_m"]) cfg.ec_m = root["default_ec_m"].as<int>();
    if (root["default_ec_codec"]) cfg.ec_codec = root["default_ec_codec"].as<std::string>();
    if (root["max_ec_k"]) cfg.max_ec_k = root["max_ec_k"].as<int>();
    if (root["max_ec_m"]) cfg.max_ec_m = root["max_ec_m"].as<int>();
    if (root["max_replica_count"]) cfg.max_replica_count = root["max_replica_count"].as<int>();
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
    if (root["admin"]) cfg.admin = root["admin"].as<bool>();
    if (root["admin_metrics_public"])
      cfg.admin_metrics_public = root["admin_metrics_public"].as<bool>();
    if (root["peers"]) {
      cfg.peers.clear();
      for (const auto& p : root["peers"]) {
        cfg.peers.push_back(p.as<std::string>());
      }
    }
    if (root["layout_rules"]) {
      if (!root["layout_rules"].IsSequence()) {
        err = "layout_rules must be a sequence";
        return false;
      }
      cfg.layout_rules.clear();
      for (const auto& node : root["layout_rules"]) {
        if (!node.IsMap()) {
          err = "layout_rules entries must be mappings";
          return false;
        }
        if (!node["prefix"] || !node["layout"]) {
          err = "layout_rules entries require prefix and layout";
          return false;
        }
        LayoutRule rule;
        rule.prefix = node["prefix"].as<std::string>();
        rule.layout = node["layout"].as<std::string>();
        if (node["ec_k"]) rule.ec_k = node["ec_k"].as<int>();
        if (node["ec_m"]) rule.ec_m = node["ec_m"].as<int>();
        if (node["ec_codec"]) rule.ec_codec = node["ec_codec"].as<std::string>();
        cfg.layout_rules.push_back(std::move(rule));
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
    if (arg == "--durability") {
      const char* v = need("--durability");
      if (!v) return false;
      cfg.durability = v;
      continue;
    }
    if (arg == "--ec-k") {
      const char* v = need("--ec-k");
      if (!v) return false;
      cfg.ec_k = std::stoi(v);
      continue;
    }
    if (arg == "--ec-m") {
      const char* v = need("--ec-m");
      if (!v) return false;
      cfg.ec_m = std::stoi(v);
      continue;
    }
    if (arg == "--ec-codec") {
      const char* v = need("--ec-codec");
      if (!v) return false;
      cfg.ec_codec = v;
      continue;
    }
    if (arg == "--http-listen") {
      const char* v = need("--http-listen");
      if (!v) return false;
      cfg.http_listen = v;
      continue;
    }
    if (arg == "--admin") {
      cfg.admin = true;
      continue;
    }
    if (arg == "--admin-metrics-public") {
      cfg.admin_metrics_public = true;
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
  return normalize_config(cfg, err);
}

bool normalize_config(Config& cfg, std::string& err) {
  if (cfg.durability.empty() || cfg.durability == "replica") {
    cfg.durability = "replica";
  } else if (cfg.durability != "ec") {
    err = "durability must be 'replica' or 'ec'";
    return false;
  } else {
    if (cfg.ec_k < 1 || cfg.ec_m < 1) {
      err = "ec_k and ec_m must be >= 1";
      return false;
    }
    std::string codec = cfg.ec_codec;
    if (!validate_ec_codec_choice(cfg.ec_m, codec, err)) return false;
    cfg.ec_codec = codec;
    cfg.replica_count = cfg.ec_k + cfg.ec_m;
    if (cfg.write_quorum == 0) cfg.write_quorum = cfg.replica_count;
  }

  for (auto& rule : cfg.layout_rules) {
    if (!validate_layout_rule(rule, cfg, err)) return false;
  }
  return true;
}

}  // namespace aios
