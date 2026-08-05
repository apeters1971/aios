#include "object/archive_tape.hpp"

#include "cluster/place.hpp"
#include "object/archive_bag.hpp"
#include "object/object_io.hpp"
#include "object/object_layout.hpp"
#include "util/log.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace aios {
namespace {

namespace fs = std::filesystem;

bool tape_sink_drains(const std::string& sink) {
  return sink == "external" || sink == "s3" || sink == "xrdcp";
}

std::string safe_bag_filename(const std::string& bag_id) {
  std::string out;
  out.reserve(bag_id.size());
  for (char c : bag_id) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.') {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
  }
  if (out.empty()) out = "bag";
  return out;
}

std::string join_uri_prefix(std::string prefix, const std::string& name) {
  if (prefix.empty()) return name;
  if (prefix.back() != '/') prefix.push_back('/');
  return prefix + name;
}

std::string attr_or(const std::unordered_map<std::string, std::string>& attrs, const char* key,
                    const std::string& fallback) {
  auto it = attrs.find(key);
  if (it != attrs.end() && !it->second.empty()) return it->second;
  return fallback;
}

bool write_file_atomic(const fs::path& dest, const std::uint8_t* data, std::size_t len,
                       std::string& err) {
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  if (ec) {
    err = "mkdir tape path: " + ec.message();
    return false;
  }
  const fs::path tmp = dest.string() + ".tmp." + std::to_string(getpid());
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      err = "open tape temp failed";
      return false;
    }
    if (len > 0) {
      out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
      if (!out) {
        err = "write tape temp failed";
        fs::remove(tmp, ec);
        return false;
      }
    }
    out.close();
    if (!out) {
      err = "close tape temp failed";
      fs::remove(tmp, ec);
      return false;
    }
  }
  fs::rename(tmp, dest, ec);
  if (ec) {
    err = "rename tape object: " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

bool read_file_bytes(const fs::path& path, std::vector<std::uint8_t>& out, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "open tape object failed";
    return false;
  }
  in.seekg(0, std::ios::end);
  const auto sz = in.tellg();
  if (sz < 0) {
    err = "stat tape object failed";
    return false;
  }
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<std::size_t>(sz));
  if (!out.empty()) {
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!in) {
      err = "read tape object failed";
      return false;
    }
  }
  return true;
}

// argv = {cmd, args...}. Uses execvp so PATH binaries (aws, xrdcp) work.
bool run_external(const std::string& cmd, const std::vector<std::string>& args,
                  std::string& out_line, std::string& err) {
  if (cmd.empty()) {
    err = "empty tape command";
    return false;
  }
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    err = "pipe failed";
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    err = "fork failed";
    return false;
  }
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    // Keep stderr for operator logs; parent only checks exit status.
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(cmd.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(cmd.c_str(), argv.data());
    _exit(127);
  }
  close(pipefd[1]);
  std::string captured;
  char buf[4096];
  ssize_t n;
  while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
    captured.append(buf, static_cast<std::size_t>(n));
  }
  close(pipefd[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    err = "waitpid failed";
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    err = "tape command failed (exit " + std::to_string(code) + "): " + cmd;
    return false;
  }
  const auto nl = captured.find('\n');
  out_line = nl == std::string::npos ? captured : captured.substr(0, nl);
  while (!out_line.empty() && (out_line.back() == '\r' || out_line.back() == ' ')) {
    out_line.pop_back();
  }
  return true;
}

fs::path resolve_tape_path(const std::string& tape_root, const std::string& uri) {
  fs::path p(uri);
  if (p.is_absolute()) return p;
  return fs::path(tape_root) / p;
}

fs::path scratch_base(const std::unordered_map<std::string, std::string>& bag_attrs,
                      const ArchiveRule* rule) {
  const std::string root =
      attr_or(bag_attrs, kTapeRootAttr, rule ? rule->tape_root : std::string{});
  if (!root.empty()) return fs::path(root) / ".staging";
  return fs::temp_directory_path() / "aios-tape-scratch";
}

bool local_bag_tip(LocalStores& stores, const std::string& bag_id, ObjectInfo& info,
                   std::unordered_map<std::string, std::string>& attrs, std::string& err) {
  for (const auto& path : stores.paths()) {
    auto* s = stores.get(path);
    if (!s) continue;
    auto st = s->stat(bag_id, err);
    if (!st || st->is_delete) continue;
    attrs = s->list_attrs(bag_id, err);
    info = *st;
    return true;
  }
  err = "bag tip not found locally";
  return false;
}

bool put_via_s3(const std::unordered_map<std::string, std::string>& bag_attrs,
                const std::string& bag_id, const fs::path& local, const ArchiveRule* rule,
                std::string& uri_out, std::string& err) {
  const std::string prefix =
      attr_or(bag_attrs, kTapeUriPrefixAttr, rule ? rule->tape_uri_prefix : std::string{});
  if (prefix.empty()) {
    err = "tape_uri_prefix not set";
    return false;
  }
  const std::string uri = join_uri_prefix(prefix, safe_bag_filename(bag_id));
  const std::string bin =
      attr_or(bag_attrs, kTapeBinAttr, rule && !rule->tape_bin.empty() ? rule->tape_bin : "aws");
  const std::string endpoint =
      attr_or(bag_attrs, kTapeS3EndpointAttr, rule ? rule->tape_s3_endpoint : std::string{});
  std::vector<std::string> args{"s3", "cp", local.string(), uri};
  if (!endpoint.empty()) {
    args.push_back("--endpoint-url");
    args.push_back(endpoint);
  }
  std::string line;
  if (!run_external(bin, args, line, err)) return false;
  uri_out = uri;
  return true;
}

bool get_via_s3(const std::unordered_map<std::string, std::string>& bag_attrs,
                const std::string& uri, const fs::path& local, const ArchiveRule* rule,
                std::string& err) {
  const std::string bin =
      attr_or(bag_attrs, kTapeBinAttr, rule && !rule->tape_bin.empty() ? rule->tape_bin : "aws");
  const std::string endpoint =
      attr_or(bag_attrs, kTapeS3EndpointAttr, rule ? rule->tape_s3_endpoint : std::string{});
  std::vector<std::string> args{"s3", "cp", uri, local.string()};
  if (!endpoint.empty()) {
    args.push_back("--endpoint-url");
    args.push_back(endpoint);
  }
  std::string line;
  return run_external(bin, args, line, err);
}

bool put_via_xrdcp(const std::unordered_map<std::string, std::string>& bag_attrs,
                   const std::string& bag_id, const fs::path& local, const ArchiveRule* rule,
                   std::string& uri_out, std::string& err) {
  const std::string prefix =
      attr_or(bag_attrs, kTapeUriPrefixAttr, rule ? rule->tape_uri_prefix : std::string{});
  if (prefix.empty()) {
    err = "tape_uri_prefix not set";
    return false;
  }
  const std::string uri = join_uri_prefix(prefix, safe_bag_filename(bag_id));
  const std::string bin = attr_or(
      bag_attrs, kTapeBinAttr, rule && !rule->tape_bin.empty() ? rule->tape_bin : "xrdcp");
  // -f overwrite, -s silent
  std::string line;
  if (!run_external(bin, {"-f", "-s", local.string(), uri}, line, err)) return false;
  uri_out = uri;
  return true;
}

bool get_via_xrdcp(const std::unordered_map<std::string, std::string>& bag_attrs,
                   const std::string& uri, const fs::path& local, const ArchiveRule* rule,
                   std::string& err) {
  const std::string bin = attr_or(
      bag_attrs, kTapeBinAttr, rule && !rule->tape_bin.empty() ? rule->tape_bin : "xrdcp");
  std::string line;
  return run_external(bin, {"-f", "-s", uri, local.string()}, line, err);
}

}  // namespace

const ArchiveRule* find_tape_rule_for_attrs(
    const Config& cfg, const std::unordered_map<std::string, std::string>& attrs) {
  auto sink_it = attrs.find(kTapeSinkAttr);
  const std::string sink = sink_it == attrs.end() ? std::string{} : sink_it->second;
  auto root = attrs.find(kTapeRootAttr);
  auto prefix = attrs.find(kTapeUriPrefixAttr);
  const ArchiveRule* fallback = nullptr;
  for (const auto& rule : cfg.archive_rules) {
    if (!tape_sink_drains(rule.tape_sink)) continue;
    if (!sink.empty() && rule.tape_sink != sink) continue;
    if (!fallback) fallback = &rule;
    if (prefix != attrs.end() && !prefix->second.empty() &&
        rule.tape_uri_prefix == prefix->second) {
      return &rule;
    }
    if (root != attrs.end() && !root->second.empty() && rule.tape_root == root->second) {
      return &rule;
    }
  }
  return fallback;
}

bool tape_put_bag(const std::unordered_map<std::string, std::string>& bag_attrs,
                  const std::string& bag_id, const std::vector<std::uint8_t>& body,
                  const ArchiveRule* rule, std::string& uri_out, std::string& err) {
  const std::string sink =
      attr_or(bag_attrs, kTapeSinkAttr, rule ? rule->tape_sink : std::string{});

  if (sink == "s3" || sink == "xrdcp") {
    const fs::path scratch = scratch_base(bag_attrs, rule);
    const fs::path tmp = scratch / (safe_bag_filename(bag_id) + ".put");
    if (!write_file_atomic(tmp, body.data(), body.size(), err)) return false;
    bool ok = false;
    if (sink == "s3") ok = put_via_s3(bag_attrs, bag_id, tmp, rule, uri_out, err);
    else ok = put_via_xrdcp(bag_attrs, bag_id, tmp, rule, uri_out, err);
    std::error_code ec;
    fs::remove(tmp, ec);
    return ok;
  }

  // external: custom cmd or filesystem under tape_root
  const std::string tape_root =
      attr_or(bag_attrs, kTapeRootAttr, rule ? rule->tape_root : std::string{});
  if (tape_root.empty()) {
    err = "tape_root not set";
    return false;
  }
  const std::string put_cmd = rule ? rule->tape_put_cmd : std::string{};
  if (!put_cmd.empty()) {
    const fs::path tmp =
        fs::path(tape_root) / ".staging" / (safe_bag_filename(bag_id) + ".put");
    if (!write_file_atomic(tmp, body.data(), body.size(), err)) return false;
    std::string line;
    if (!run_external(put_cmd, {bag_id, tmp.string()}, line, err)) {
      std::error_code ec;
      fs::remove(tmp, ec);
      return false;
    }
    std::error_code ec;
    fs::remove(tmp, ec);
    if (line.empty()) {
      err = "tape_put_cmd produced empty URI";
      return false;
    }
    uri_out = line;
    return true;
  }

  const fs::path rel = fs::path("bags") / safe_bag_filename(bag_id);
  const fs::path dest = fs::path(tape_root) / rel;
  if (!write_file_atomic(dest, body.data(), body.size(), err)) return false;
  uri_out = rel.generic_string();
  return true;
}

bool tape_get_bag(const std::unordered_map<std::string, std::string>& bag_attrs,
                  const std::string& bag_id, const ArchiveRule* rule,
                  std::vector<std::uint8_t>& body_out, std::string& err) {
  auto uri_it = bag_attrs.find(kTapeUriAttr);
  if (uri_it == bag_attrs.end() || uri_it->second.empty()) {
    err = "missing aios.tape_uri";
    return false;
  }
  const std::string sink =
      attr_or(bag_attrs, kTapeSinkAttr, rule ? rule->tape_sink : std::string{});

  if (sink == "s3" || sink == "xrdcp") {
    const fs::path scratch = scratch_base(bag_attrs, rule);
    std::error_code ec;
    fs::create_directories(scratch, ec);
    const fs::path tmp = scratch / (safe_bag_filename(bag_id) + ".get");
    bool ok = false;
    if (sink == "s3") ok = get_via_s3(bag_attrs, uri_it->second, tmp, rule, err);
    else ok = get_via_xrdcp(bag_attrs, uri_it->second, tmp, rule, err);
    if (!ok) {
      fs::remove(tmp, ec);
      return false;
    }
    if (!read_file_bytes(tmp, body_out, err)) {
      fs::remove(tmp, ec);
      return false;
    }
    fs::remove(tmp, ec);
    return true;
  }

  const std::string tape_root =
      attr_or(bag_attrs, kTapeRootAttr, rule ? rule->tape_root : std::string{});
  const std::string get_cmd = rule ? rule->tape_get_cmd : std::string{};
  if (!get_cmd.empty()) {
    if (tape_root.empty()) {
      err = "tape_root not set for get staging";
      return false;
    }
    const fs::path tmp =
        fs::path(tape_root) / ".staging" / (safe_bag_filename(bag_id) + ".get");
    std::error_code ec;
    fs::create_directories(tmp.parent_path(), ec);
    std::string line;
    if (!run_external(get_cmd, {uri_it->second, tmp.string()}, line, err)) return false;
    if (!read_file_bytes(tmp, body_out, err)) {
      fs::remove(tmp, ec);
      return false;
    }
    fs::remove(tmp, ec);
    return true;
  }
  if (tape_root.empty() && !fs::path(uri_it->second).is_absolute()) {
    err = "tape_root not set";
    return false;
  }
  const fs::path path = resolve_tape_path(tape_root, uri_it->second);
  return read_file_bytes(path, body_out, err);
}

bool ensure_bag_on_staging(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                           LocalStores& stores, const std::string& bag_id,
                           std::unordered_map<std::string, std::string>& bag_attrs,
                           std::string& err) {
  ObjectInfo info;
  if (!local_bag_tip(stores, bag_id, info, bag_attrs, err)) return false;

  if (info.size > 0) return true;

  auto sink = bag_attrs.find(kTapeSinkAttr);
  if (sink == bag_attrs.end() || sink->second.empty()) {
    err = "bag body missing and no tape sink";
    return false;
  }
  const ArchiveRule* rule = find_tape_rule_for_attrs(cfg, bag_attrs);
  std::vector<std::uint8_t> body;
  if (!tape_get_bag(bag_attrs, bag_id, rule, body, err)) return false;
  if (body.empty()) {
    err = "tape restore returned empty body";
    return false;
  }
  auto expect = bag_attrs.find(kContentSha256Attr);
  if (expect != bag_attrs.end() && !expect->second.empty()) {
    if (sha256_hex_bytes(body.data(), body.size()) != expect->second) {
      err = "tape restore checksum mismatch";
      return false;
    }
  }

  const std::string sc = storage_class_for_attrs(bag_attrs, cfg.default_storage_class);
  const int n = placement_n_for_attrs(bag_attrs, map.replica_count);
  auto dest = place(bag_id, map, n, sc);
  if (dest.acting_set.empty() || dest.acting_set[0].node_id != cfg.node_id) {
    err = "not primary for bag";
    return false;
  }
  // Keep on_tape + tape_uri so a later drain tick can reclaim staging again.
  bag_attrs[kArchiveStateAttr] = kArchiveStateOnTape;
  if (!install_replica_version(cfg, advertise, map, stores, dest, bag_id, body, bag_attrs)) {
    err = "install restored bag failed";
    return false;
  }
  return true;
}

bool drain_one_bag(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                   LocalStores& stores, const std::string& bag_id) {
  std::string err;
  ObjectInfo info;
  std::unordered_map<std::string, std::string> attrs;
  if (!local_bag_tip(stores, bag_id, info, attrs, err)) return false;

  auto sink = attrs.find(kTapeSinkAttr);
  if (sink == attrs.end() || sink->second.empty()) return false;
  if (info.size == 0) {
    return attrs.count(kTapeUriAttr) > 0 && !attrs[kTapeUriAttr].empty();
  }

  const int n = placement_n_for_attrs(attrs, map.replica_count);
  const std::string sc = storage_class_for_attrs(attrs, cfg.default_storage_class);
  auto dest = place(bag_id, map, n, sc);
  if (dest.acting_set.empty() || dest.acting_set[0].node_id != cfg.node_id) return false;

  std::vector<std::uint8_t> body;
  std::unordered_map<std::string, std::string> loaded;
  if (!load_object_bytes(cfg, advertise, map, stores, dest, bag_id, body, loaded)) {
    AIOS_LOG_WARN("archive drain load ", bag_id, ": unavailable");
    return false;
  }
  if (!loaded.empty()) attrs = loaded;
  if (body.empty()) return false;

  const ArchiveRule* rule = find_tape_rule_for_attrs(cfg, attrs);
  auto uri_it = attrs.find(kTapeUriAttr);
  if (uri_it == attrs.end() || uri_it->second.empty()) {
    std::string uri;
    if (!tape_put_bag(attrs, bag_id, body, rule, uri, err)) {
      AIOS_LOG_WARN("archive drain put ", bag_id, ": ", err);
      return false;
    }
    attrs[kTapeUriAttr] = uri;
  }

  attrs[kArchiveStateAttr] = kArchiveStateOnTape;
  std::vector<std::uint8_t> empty;
  if (!install_replica_version(cfg, advertise, map, stores, dest, bag_id, empty, attrs)) {
    AIOS_LOG_WARN("archive drain reclaim ", bag_id, " failed");
    return false;
  }
  return true;
}

ArchiveDrainStats run_archive_drain(const Config& cfg, const std::string& advertise,
                                    const ClusterMap& map, LocalStores& stores,
                                    std::size_t max_oids_per_store) {
  ArchiveDrainStats stats;
  if (map.targets.empty()) return stats;

  for (const auto& path : stores.paths()) {
    auto* store = stores.get(path);
    if (!store) continue;
    std::string err;
    auto oids = store->list_oids(max_oids_per_store, err);
    if (!err.empty()) continue;
    for (const auto& oid : oids) {
      if (!is_archive_bag_oid(oid)) continue;
      ++stats.bags_scanned;
      auto info = store->stat(oid, err);
      if (!info || info->is_delete) continue;
      auto attrs = store->list_attrs(oid, err);
      auto sink = attrs.find(kTapeSinkAttr);
      if (sink == attrs.end() || sink->second.empty()) {
        ++stats.skipped;
        continue;
      }
      if (info->size == 0) {
        ++stats.skipped;
        continue;
      }
      if (drain_one_bag(cfg, advertise, map, stores, oid)) ++stats.drained;
      else ++stats.failed;
    }
  }
  return stats;
}

}  // namespace aios
