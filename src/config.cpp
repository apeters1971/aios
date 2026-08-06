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

bool valid_storage_class_name(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (char c : s) {
    if (std::islower(static_cast<unsigned char>(c)) ||
        std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
      continue;
    }
    return false;
  }
  return true;
}

bool validate_posix_layout_spec(PosixLayoutSpec& spec, const Config& cfg, const char* which,
                                std::string& err) {
  if (!spec.layout.empty()) {
    spec.layout = lower_copy(spec.layout);
    if (spec.layout != "replica" && spec.layout != "ec") {
      err = std::string("posix_layout_rules ") + which + " layout must be 'replica' or 'ec'";
      return false;
    }
  }
  if (spec.storage_class) {
    *spec.storage_class = lower_copy(*spec.storage_class);
    if (!valid_storage_class_name(*spec.storage_class)) {
      err = std::string("posix_layout_rules ") + which + " storage_class must match [a-z0-9_-]+";
      return false;
    }
  }
  if (spec.layout == "ec") {
    const int k = spec.ec_k.value_or(cfg.ec_k);
    const int m = spec.ec_m.value_or(cfg.ec_m);
    if (k < 1 || m < 1) {
      err = std::string("posix_layout_rules ") + which + " ec_k and ec_m must be >= 1";
      return false;
    }
    std::string codec = spec.ec_codec.value_or(cfg.ec_codec);
    if (!validate_ec_codec_choice(m, codec, err)) {
      err = std::string("posix_layout_rules ") + which + ": " + err;
      return false;
    }
    if (spec.ec_codec.has_value()) spec.ec_codec = codec;
  }
  return true;
}

bool validate_posix_layout_rule(PosixLayoutRule& rule, const Config& cfg, std::string& err) {
  if (rule.path.empty() || rule.path[0] != '/') {
    err = "posix_layout_rules path must start with /";
    return false;
  }
  while (rule.path.size() > 1 && rule.path.back() == '/') rule.path.pop_back();
  if (rule.volume) *rule.volume = lower_copy(*rule.volume);
  if (!validate_posix_layout_spec(rule.meta, cfg, "meta", err)) return false;
  if (!validate_posix_layout_spec(rule.data, cfg, "data", err)) return false;
  return true;
}

bool parse_posix_layout_spec_yaml(const YAML::Node& node, PosixLayoutSpec& spec, std::string& err) {
  if (!node || !node.IsMap()) {
    err = "posix_layout_rules meta/data must be a mapping";
    return false;
  }
  if (node["layout"]) spec.layout = node["layout"].as<std::string>();
  if (node["storage_class"]) spec.storage_class = node["storage_class"].as<std::string>();
  if (node["ec_k"]) spec.ec_k = node["ec_k"].as<int>();
  if (node["ec_m"]) spec.ec_m = node["ec_m"].as<int>();
  if (node["ec_codec"]) spec.ec_codec = node["ec_codec"].as<std::string>();
  return true;
}

bool validate_layout_rule(LayoutRule& rule, const Config& cfg, std::string& err) {
  rule.layout = lower_copy(rule.layout);
  if (rule.layout != "replica" && rule.layout != "ec") {
    err = "layout_rules entry layout must be 'replica' or 'ec'";
    return false;
  }
  if (rule.storage_class) {
    *rule.storage_class = lower_copy(*rule.storage_class);
    if (!valid_storage_class_name(*rule.storage_class)) {
      err = "layout_rules storage_class must match [a-z0-9_-]+";
      return false;
    }
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
  if (rule.ec_codec.has_value()) {
    rule.ec_codec = codec;
  }
  return true;
}

bool validate_transition_rule(TransitionRule& rule, const Config& cfg, std::string& err) {
  rule.from = lower_copy(rule.from);
  rule.to = lower_copy(rule.to);
  if (!valid_storage_class_name(rule.from) || !valid_storage_class_name(rule.to)) {
    err = "transition_rules from/to must match [a-z0-9_-]+";
    return false;
  }
  if (rule.from == rule.to) {
    err = "transition_rules from and to must differ";
    return false;
  }
  if (rule.layout) {
    LayoutRule lr;
    lr.prefix = rule.prefix;
    lr.layout = *rule.layout;
    lr.ec_k = rule.ec_k;
    lr.ec_m = rule.ec_m;
    lr.ec_codec = rule.ec_codec;
    if (!validate_layout_rule(lr, cfg, err)) {
      err = "transition_rules: " + err;
      return false;
    }
    rule.layout = lr.layout;
    if (lr.ec_codec) rule.ec_codec = lr.ec_codec;
  }
  return true;
}

bool validate_archive_rule(ArchiveRule& rule, std::string& err) {
  rule.from = lower_copy(rule.from);
  rule.staging_class = lower_copy(rule.staging_class);
  rule.tape_sink = lower_copy(rule.tape_sink);
  if (!valid_storage_class_name(rule.from) || !valid_storage_class_name(rule.staging_class)) {
    err = "archive_rules from/staging_class must match [a-z0-9_-]+";
    return false;
  }
  if (rule.min_bag_bytes == 0) {
    err = "archive_rules min_bag_bytes must be > 0";
    return false;
  }
  if (rule.max_bag_bytes > 0 && rule.max_bag_bytes < rule.min_bag_bytes) {
    err = "archive_rules max_bag_bytes must be >= min_bag_bytes";
    return false;
  }
  if (!rule.tape_sink.empty() && rule.tape_sink != "none" && rule.tape_sink != "external" &&
      rule.tape_sink != "s3" && rule.tape_sink != "xrdcp") {
    err = "archive_rules tape_sink must be empty, none, external, s3, or xrdcp";
    return false;
  }
  if (rule.tape_sink == "none") rule.tape_sink.clear();
  if (rule.tape_sink == "external" && rule.tape_root.empty()) {
    err = "archive_rules tape_sink=external requires tape_root";
    return false;
  }
  if (rule.tape_sink == "s3") {
    if (rule.tape_uri_prefix.rfind("s3://", 0) != 0) {
      err = "archive_rules tape_sink=s3 requires tape_uri_prefix starting with s3://";
      return false;
    }
  }
  if (rule.tape_sink == "xrdcp") {
    if (rule.tape_uri_prefix.rfind("root://", 0) != 0 &&
        rule.tape_uri_prefix.rfind("xroot://", 0) != 0) {
      err = "archive_rules tape_sink=xrdcp requires tape_uri_prefix starting with root:// "
            "or xroot://";
      return false;
    }
  }
  if ((!rule.tape_put_cmd.empty() || !rule.tape_get_cmd.empty()) &&
      rule.tape_sink != "external") {
    err = "archive_rules tape_put_cmd/tape_get_cmd require tape_sink=external";
    return false;
  }
  if (!rule.tape_s3_endpoint.empty() && rule.tape_sink != "s3") {
    err = "archive_rules tape_s3_endpoint requires tape_sink=s3";
    return false;
  }
  rule.bag_compression = lower_copy(rule.bag_compression);
  rule.bag_encryption = lower_copy(rule.bag_encryption);
  if (rule.bag_compression.empty()) rule.bag_compression = "none";
  if (rule.bag_encryption.empty()) rule.bag_encryption = "none";
  if (rule.bag_compression != "none" && rule.bag_compression != "zstd") {
    err = "archive_rules bag_compression must be none or zstd";
    return false;
  }
  if (rule.bag_encryption != "none" && rule.bag_encryption != "aes-256-gcm") {
    err = "archive_rules bag_encryption must be none or aes-256-gcm";
    return false;
  }
  if (rule.bag_compression_level < 0 || rule.bag_compression_level > 22) {
    err = "archive_rules bag_compression_level must be 0..22";
    return false;
  }
  return true;
}

bool validate_backup_rule(BackupRule& rule, std::string& err) {
  rule.kind = lower_copy(rule.kind);
  rule.tape_sink = lower_copy(rule.tape_sink);
  rule.from = lower_copy(rule.from);
  rule.staging_class = lower_copy(rule.staging_class);
  if (rule.kind != "posix" && rule.kind != "vbd") {
    err = "backup_rules kind must be posix or vbd";
    return false;
  }
  if (rule.kind == "posix" && rule.volume.empty()) {
    err = "backup_rules posix requires volume";
    return false;
  }
  if (rule.kind == "vbd" && (rule.pool.empty() || rule.name.empty())) {
    err = "backup_rules vbd requires pool and name";
    return false;
  }
  if (!rule.staging_class.empty() && !valid_storage_class_name(rule.staging_class)) {
    err = "backup_rules staging_class must match [a-z0-9_-]+";
    return false;
  }
  if (!rule.from.empty() && !valid_storage_class_name(rule.from)) {
    err = "backup_rules from must match [a-z0-9_-]+";
    return false;
  }
  ArchiveRule tape;
  tape.tape_sink = rule.tape_sink;
  tape.tape_root = rule.tape_root;
  tape.tape_uri_prefix = rule.tape_uri_prefix;
  tape.tape_bin = rule.tape_bin;
  tape.tape_s3_endpoint = rule.tape_s3_endpoint;
  tape.tape_put_cmd = rule.tape_put_cmd;
  tape.tape_get_cmd = rule.tape_get_cmd;
  tape.bag_compression = rule.bag_compression;
  tape.bag_compression_level = rule.bag_compression_level;
  tape.bag_encryption = rule.bag_encryption;
  tape.from = rule.from.empty() ? "nvme" : rule.from;
  tape.staging_class = rule.staging_class.empty() ? "archive" : rule.staging_class;
  tape.min_bag_bytes = 1;
  if (!validate_archive_rule(tape, err)) {
    const std::string pref = "archive_rules";
    if (err.rfind(pref, 0) == 0) err.replace(0, pref.size(), "backup_rules");
    return false;
  }
  rule.tape_sink = tape.tape_sink;
  rule.bag_compression = tape.bag_compression;
  rule.bag_encryption = tape.bag_encryption;
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
    if (root["default_storage_class"])
      cfg.default_storage_class = root["default_storage_class"].as<std::string>();
    if (root["vnodes_per_target"]) cfg.vnodes_per_target = root["vnodes_per_target"].as<int>();
    if (root["min_vnodes"]) cfg.min_vnodes = root["min_vnodes"].as<int>();
    if (root["max_vnodes"]) cfg.max_vnodes = root["max_vnodes"].as<int>();
    if (root["placement"] && root["placement"].IsMap()) {
      const auto& p = root["placement"];
      if (p["vnodes_per_target"]) cfg.vnodes_per_target = p["vnodes_per_target"].as<int>();
      if (p["min_vnodes"]) cfg.min_vnodes = p["min_vnodes"].as<int>();
      if (p["max_vnodes"]) cfg.max_vnodes = p["max_vnodes"].as<int>();
    }
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
    if (root["transition_interval_ms"])
      cfg.transition_interval_ms = root["transition_interval_ms"].as<int>();
    if (root["transition_batch_oids"])
      cfg.transition_batch_oids = root["transition_batch_oids"].as<int>();
    if (root["archive_interval_ms"])
      cfg.archive_interval_ms = root["archive_interval_ms"].as<int>();
    if (root["archive_batch_oids"]) cfg.archive_batch_oids = root["archive_batch_oids"].as<int>();
    if (root["backup_interval_ms"])
      cfg.backup_interval_ms = root["backup_interval_ms"].as<int>();
    if (root["backup_batch_oids"]) cfg.backup_batch_oids = root["backup_batch_oids"].as<int>();
    if (root["http_listen"]) cfg.http_listen = root["http_listen"].as<std::string>();
    if (root["s3_listen"]) cfg.s3_listen = root["s3_listen"].as<std::string>();
    if (root["s3_volume"]) cfg.s3_volume = root["s3_volume"].as<std::string>();
    if (root["s3_access_key"]) cfg.s3_access_key = root["s3_access_key"].as<std::string>();
    if (root["cuobject_listen"]) cfg.cuobject_listen = root["cuobject_listen"].as<std::string>();
    if (root["http_body_sync"])
      cfg.http_body_sync = root["http_body_sync"].as<std::string>();
    if (root["max_versions"]) cfg.max_versions = root["max_versions"].as<int>();
    if (root["clone_required"]) cfg.clone_required = root["clone_required"].as<bool>();
    if (root["max_object_bytes"])
      cfg.max_object_bytes = root["max_object_bytes"].as<std::uint64_t>();
    if (root["admin"]) cfg.admin = root["admin"].as<bool>();
    if (root["admin_metrics_public"])
      cfg.admin_metrics_public = root["admin_metrics_public"].as<bool>();
    if (root["compression"]) cfg.compression = root["compression"].as<std::string>();
    if (root["compression_level"]) cfg.compression_level = root["compression_level"].as<int>();
    if (root["compression_min_bytes"])
      cfg.compression_min_bytes = root["compression_min_bytes"].as<std::uint64_t>();
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
        if (node["storage_class"])
          rule.storage_class = node["storage_class"].as<std::string>();
        if (node["ec_k"]) rule.ec_k = node["ec_k"].as<int>();
        if (node["ec_m"]) rule.ec_m = node["ec_m"].as<int>();
        if (node["ec_codec"]) rule.ec_codec = node["ec_codec"].as<std::string>();
        cfg.layout_rules.push_back(std::move(rule));
      }
    }
    if (root["posix_layout_rules"]) {
      if (!root["posix_layout_rules"].IsSequence()) {
        err = "posix_layout_rules must be a sequence";
        return false;
      }
      cfg.posix_layout_rules.clear();
      for (const auto& node : root["posix_layout_rules"]) {
        if (!node.IsMap()) {
          err = "posix_layout_rules entries must be mappings";
          return false;
        }
        if (!node["path"]) {
          err = "posix_layout_rules entries require path";
          return false;
        }
        PosixLayoutRule rule;
        rule.path = node["path"].as<std::string>();
        if (node["volume"]) rule.volume = node["volume"].as<std::string>();
        if (node["meta"]) {
          if (!parse_posix_layout_spec_yaml(node["meta"], rule.meta, err)) return false;
        }
        if (node["data"]) {
          if (!parse_posix_layout_spec_yaml(node["data"], rule.data, err)) return false;
        }
        cfg.posix_layout_rules.push_back(std::move(rule));
      }
    }
    if (root["transition_rules"]) {
      if (!root["transition_rules"].IsSequence()) {
        err = "transition_rules must be a sequence";
        return false;
      }
      cfg.transition_rules.clear();
      for (const auto& node : root["transition_rules"]) {
        if (!node.IsMap()) {
          err = "transition_rules entries must be mappings";
          return false;
        }
        if (!node["prefix"] || !node["from"] || !node["to"]) {
          err = "transition_rules entries require prefix, from, and to";
          return false;
        }
        TransitionRule rule;
        rule.prefix = node["prefix"].as<std::string>();
        rule.from = node["from"].as<std::string>();
        rule.to = node["to"].as<std::string>();
        if (node["layout"]) rule.layout = node["layout"].as<std::string>();
        if (node["ec_k"]) rule.ec_k = node["ec_k"].as<int>();
        if (node["ec_m"]) rule.ec_m = node["ec_m"].as<int>();
        if (node["ec_codec"]) rule.ec_codec = node["ec_codec"].as<std::string>();
        cfg.transition_rules.push_back(std::move(rule));
      }
    }
    if (root["archive_rules"]) {
      if (!root["archive_rules"].IsSequence()) {
        err = "archive_rules must be a sequence";
        return false;
      }
      cfg.archive_rules.clear();
      for (const auto& node : root["archive_rules"]) {
        if (!node.IsMap()) {
          err = "archive_rules entries must be mappings";
          return false;
        }
        if (!node["prefix"] || !node["from"]) {
          err = "archive_rules entries require prefix and from";
          return false;
        }
        ArchiveRule rule;
        rule.prefix = node["prefix"].as<std::string>();
        rule.from = node["from"].as<std::string>();
        if (node["staging_class"]) rule.staging_class = node["staging_class"].as<std::string>();
        if (node["min_age_days"]) rule.min_age_days = node["min_age_days"].as<int>();
        if (node["min_bag_bytes"])
          rule.min_bag_bytes = node["min_bag_bytes"].as<std::uint64_t>();
        if (node["max_bag_bytes"])
          rule.max_bag_bytes = node["max_bag_bytes"].as<std::uint64_t>();
        if (node["max_members"]) rule.max_members = node["max_members"].as<int>();
        if (node["max_open_ms"]) rule.max_open_ms = node["max_open_ms"].as<int>();
        if (node["tape_sink"]) rule.tape_sink = node["tape_sink"].as<std::string>();
        if (node["tape_root"]) rule.tape_root = node["tape_root"].as<std::string>();
        if (node["tape_uri_prefix"])
          rule.tape_uri_prefix = node["tape_uri_prefix"].as<std::string>();
        if (node["tape_bin"]) rule.tape_bin = node["tape_bin"].as<std::string>();
        if (node["tape_s3_endpoint"])
          rule.tape_s3_endpoint = node["tape_s3_endpoint"].as<std::string>();
        if (node["tape_put_cmd"]) rule.tape_put_cmd = node["tape_put_cmd"].as<std::string>();
        if (node["tape_get_cmd"]) rule.tape_get_cmd = node["tape_get_cmd"].as<std::string>();
        if (node["bag_compression"])
          rule.bag_compression = node["bag_compression"].as<std::string>();
        if (node["bag_compression_level"])
          rule.bag_compression_level = node["bag_compression_level"].as<int>();
        if (node["bag_encryption"]) rule.bag_encryption = node["bag_encryption"].as<std::string>();
        cfg.archive_rules.push_back(std::move(rule));
      }
    }
    if (root["backup_rules"]) {
      if (!root["backup_rules"].IsSequence()) {
        err = "backup_rules must be a sequence";
        return false;
      }
      cfg.backup_rules.clear();
      for (const auto& node : root["backup_rules"]) {
        if (!node.IsMap()) {
          err = "backup_rules entries must be mappings";
          return false;
        }
        if (!node["kind"]) {
          err = "backup_rules entries require kind";
          return false;
        }
        BackupRule rule;
        rule.kind = node["kind"].as<std::string>();
        if (node["volume"]) rule.volume = node["volume"].as<std::string>();
        if (node["pool"]) rule.pool = node["pool"].as<std::string>();
        if (node["name"]) rule.name = node["name"].as<std::string>();
        if (node["retain_snaps"]) rule.retain_snaps = node["retain_snaps"].as<int>();
        if (node["from"]) rule.from = node["from"].as<std::string>();
        if (node["staging_class"]) rule.staging_class = node["staging_class"].as<std::string>();
        if (node["max_bag_bytes"])
          rule.max_bag_bytes = node["max_bag_bytes"].as<std::uint64_t>();
        if (node["max_members"]) rule.max_members = node["max_members"].as<int>();
        if (node["tape_sink"]) rule.tape_sink = node["tape_sink"].as<std::string>();
        if (node["tape_root"]) rule.tape_root = node["tape_root"].as<std::string>();
        if (node["tape_uri_prefix"])
          rule.tape_uri_prefix = node["tape_uri_prefix"].as<std::string>();
        if (node["tape_bin"]) rule.tape_bin = node["tape_bin"].as<std::string>();
        if (node["tape_s3_endpoint"])
          rule.tape_s3_endpoint = node["tape_s3_endpoint"].as<std::string>();
        if (node["tape_put_cmd"]) rule.tape_put_cmd = node["tape_put_cmd"].as<std::string>();
        if (node["tape_get_cmd"]) rule.tape_get_cmd = node["tape_get_cmd"].as<std::string>();
        if (node["bag_compression"])
          rule.bag_compression = node["bag_compression"].as<std::string>();
        if (node["bag_compression_level"])
          rule.bag_compression_level = node["bag_compression_level"].as<int>();
        if (node["bag_encryption"]) rule.bag_encryption = node["bag_encryption"].as<std::string>();
        cfg.backup_rules.push_back(std::move(rule));
      }
    }
    if (root["bag_encryption_key"])
      cfg.bag_encryption_key = root["bag_encryption_key"].as<std::string>();
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
    if (arg == "--s3-listen") {
      const char* v = need("--s3-listen");
      if (!v) return false;
      cfg.s3_listen = v;
      continue;
    }
    if (arg == "--s3-volume") {
      const char* v = need("--s3-volume");
      if (!v) return false;
      cfg.s3_volume = v;
      continue;
    }
    if (arg == "--s3-access-key") {
      const char* v = need("--s3-access-key");
      if (!v) return false;
      cfg.s3_access_key = v;
      continue;
    }
    if (arg == "--cuobject-listen") {
      const char* v = need("--cuobject-listen");
      if (!v) return false;
      cfg.cuobject_listen = v;
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
    if (arg == "--compression") {
      const char* v = need("--compression");
      if (!v) return false;
      cfg.compression = v;
      continue;
    }
    if (arg == "--compression-level") {
      const char* v = need("--compression-level");
      if (!v) return false;
      cfg.compression_level = std::stoi(v);
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

  cfg.default_storage_class = lower_copy(cfg.default_storage_class);
  if (!valid_storage_class_name(cfg.default_storage_class)) {
    err = "default_storage_class must match [a-z0-9_-]+";
    return false;
  }
  if (cfg.vnodes_per_target < 1) {
    err = "vnodes_per_target must be >= 1";
    return false;
  }
  if (cfg.min_vnodes < 1) {
    err = "min_vnodes must be >= 1";
    return false;
  }
  if (cfg.max_vnodes < cfg.min_vnodes) {
    err = "max_vnodes must be >= min_vnodes";
    return false;
  }

  for (auto& rule : cfg.layout_rules) {
    if (!validate_layout_rule(rule, cfg, err)) return false;
  }
  for (auto& rule : cfg.posix_layout_rules) {
    if (!validate_posix_layout_rule(rule, cfg, err)) return false;
  }
  for (auto& rule : cfg.transition_rules) {
    if (!validate_transition_rule(rule, cfg, err)) return false;
  }
  for (auto& rule : cfg.archive_rules) {
    if (!validate_archive_rule(rule, err)) return false;
  }
  for (auto& rule : cfg.backup_rules) {
    if (!validate_backup_rule(rule, err)) return false;
  }
  if (!cfg.s3_listen.empty()) {
    if (cfg.http_listen.empty()) {
      err = "s3_listen requires http_listen (S3 mounts libaios_posix via loopback HTTP)";
      return false;
    }
    if (cfg.s3_volume.empty()) {
      err = "s3_volume must be non-empty when s3_listen is set";
      return false;
    }
    if (cfg.s3_access_key.empty()) {
      err = "s3_access_key must be non-empty when s3_listen is set";
      return false;
    }
  }
  cfg.compression = lower_copy(cfg.compression);
  if (cfg.compression.empty()) cfg.compression = "none";
  if (cfg.compression != "none" && cfg.compression != "zstd") {
    err = "compression must be 'none' or 'zstd'";
    return false;
  }
  if (cfg.compression == "zstd") {
#if !defined(AIOS_HAVE_ZSTD) || !AIOS_HAVE_ZSTD
    err = "compression=zstd requires libzstd at build time";
    return false;
#endif
    if (cfg.compression_level < 1 || cfg.compression_level > 22) {
      err = "compression_level must be 1..22";
      return false;
    }
  }
  bool want_bag_enc = false;
  bool want_bag_zstd = false;
  for (const auto& r : cfg.archive_rules) {
    if (r.bag_encryption == "aes-256-gcm") want_bag_enc = true;
    if (r.bag_compression == "zstd") want_bag_zstd = true;
  }
  for (const auto& r : cfg.backup_rules) {
    if (r.bag_encryption == "aes-256-gcm") want_bag_enc = true;
    if (r.bag_compression == "zstd") want_bag_zstd = true;
  }
  if (want_bag_zstd) {
#if !defined(AIOS_HAVE_ZSTD) || !AIOS_HAVE_ZSTD
    err = "bag_compression=zstd requires libzstd at build time";
    return false;
#endif
  }
  if (!cfg.bag_encryption_key.empty()) {
    if (cfg.bag_encryption_key.size() != 64) {
      err = "bag_encryption_key must be 64 hex characters";
      return false;
    }
    for (unsigned char c : cfg.bag_encryption_key) {
      if (!std::isxdigit(c)) {
        err = "bag_encryption_key must be hex";
        return false;
      }
    }
  } else if (want_bag_enc) {
    err = "bag_encryption=aes-256-gcm requires bag_encryption_key";
    return false;
  }
  return true;
}

}  // namespace aios
