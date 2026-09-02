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

// Sink parameters come from the operator-configured rule only. Object attrs are
// client-influenced metadata and must never choose a binary, root, or endpoint.
std::string rule_sink(const ArchiveRule* rule) { return rule ? rule->tape_sink : std::string{}; }
std::string rule_root(const ArchiveRule* rule) { return rule ? rule->tape_root : std::string{}; }
std::string rule_prefix(const ArchiveRule* rule) {
  return rule ? rule->tape_uri_prefix : std::string{};
}
std::string rule_endpoint(const ArchiveRule* rule) {
  return rule ? rule->tape_s3_endpoint : std::string{};
}
std::string rule_bin(const ArchiveRule* rule, const char* dflt) {
  return rule && !rule->tape_bin.empty() ? rule->tape_bin : std::string(dflt);
}

constexpr std::uint64_t kDefaultMaxTapeObject = 64ull * 1024ull * 1024ull * 1024ull;

std::uint64_t max_tape_object_bytes(const ArchiveRule* rule) {
  if (rule && rule->max_bag_bytes > 0) return rule->max_bag_bytes;
  return kDefaultMaxTapeObject;
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

bool read_file_bytes(const fs::path& path, std::uint64_t max_bytes, std::vector<std::uint8_t>& out,
                     std::string& err) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) {
    err = "tape object is not a regular file";
    return false;
  }
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
  if (static_cast<std::uint64_t>(sz) > max_bytes) {
    err = "tape object exceeds maximum bag size";
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

// A filesystem tape_uri is a relative path under tape_root; anything else (absolute,
// leading "..", or normalizing outside the root) is refused.
bool resolve_tape_path(const std::string& tape_root, const std::string& uri, fs::path& out,
                       std::string& err) {
  if (uri.empty()) {
    err = "empty tape_uri";
    return false;
  }
  if (tape_root.empty()) {
    err = "tape_root not set";
    return false;
  }
  fs::path p(uri);
  if (p.is_absolute()) {
    err = "absolute tape_uri refused";
    return false;
  }
  const fs::path norm = p.lexically_normal();
  if (norm.empty() || norm.begin() == norm.end()) {
    err = "empty tape_uri";
    return false;
  }
  const std::string head = norm.begin()->string();
  if (head == ".." || head == "." || head.empty()) {
    err = "tape_uri escapes tape_root";
    return false;
  }
  for (const auto& part : norm) {
    if (part == "..") {
      err = "tape_uri escapes tape_root";
      return false;
    }
  }
  const fs::path root = fs::path(tape_root).lexically_normal();
  const fs::path full = (root / norm).lexically_normal();
  const std::string rs = root.generic_string();
  const std::string fsx = full.generic_string();
  const bool under = fsx.size() > rs.size() && fsx.compare(0, rs.size(), rs) == 0 &&
                     (rs.empty() || rs.back() == '/' || fsx[rs.size()] == '/');
  if (!under) {
    err = "tape_uri escapes tape_root";
    return false;
  }
  out = full;
  return true;
}

bool uri_matches_prefix(const std::string& uri, const std::string& prefix) {
  return !prefix.empty() && uri.size() > prefix.size() && uri.compare(0, prefix.size(), prefix) == 0;
}

fs::path scratch_base(const ArchiveRule* rule) {
  const std::string root = rule_root(rule);
  if (!root.empty()) return fs::path(root) / ".staging";
  return fs::temp_directory_path() / "aios-tape-scratch";
}

bool local_bag_tip(LocalStores& stores, const std::string& bag_id, ObjectInfo& info,
                   std::unordered_map<std::string, std::string>& attrs, std::string& err) {
  for (const auto& path : stores.paths()) {
    auto s = stores.get_shared(path);
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

bool put_via_s3(const std::string& bag_id, const fs::path& local, const ArchiveRule* rule,
                std::string& uri_out, std::string& err) {
  const std::string prefix = rule_prefix(rule);
  if (prefix.empty()) {
    err = "tape_uri_prefix not set";
    return false;
  }
  const std::string uri = join_uri_prefix(prefix, safe_bag_filename(bag_id));
  const std::string endpoint = rule_endpoint(rule);
  std::vector<std::string> args{"s3", "cp", local.string(), uri};
  if (!endpoint.empty()) {
    args.push_back("--endpoint-url");
    args.push_back(endpoint);
  }
  std::string line;
  if (!run_external(rule_bin(rule, "aws"), args, line, err)) return false;
  uri_out = uri;
  return true;
}

bool get_via_s3(const std::string& uri, const fs::path& local, const ArchiveRule* rule,
                std::string& err) {
  const std::string endpoint = rule_endpoint(rule);
  std::vector<std::string> args{"s3", "cp", uri, local.string()};
  if (!endpoint.empty()) {
    args.push_back("--endpoint-url");
    args.push_back(endpoint);
  }
  std::string line;
  return run_external(rule_bin(rule, "aws"), args, line, err);
}

bool put_via_xrdcp(const std::string& bag_id, const fs::path& local, const ArchiveRule* rule,
                   std::string& uri_out, std::string& err) {
  const std::string prefix = rule_prefix(rule);
  if (prefix.empty()) {
    err = "tape_uri_prefix not set";
    return false;
  }
  const std::string uri = join_uri_prefix(prefix, safe_bag_filename(bag_id));
  // -f overwrite, -s silent
  std::string line;
  if (!run_external(rule_bin(rule, "xrdcp"), {"-f", "-s", local.string(), uri}, line, err)) {
    return false;
  }
  uri_out = uri;
  return true;
}

bool get_via_xrdcp(const std::string& uri, const fs::path& local, const ArchiveRule* rule,
                   std::string& err) {
  std::string line;
  return run_external(rule_bin(rule, "xrdcp"), {"-f", "-s", uri, local.string()}, line, err);
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
  (void)bag_attrs;
  if (!rule) {
    err = "no tape rule configured";
    return false;
  }
  const std::string sink = rule_sink(rule);
  if (!tape_sink_drains(sink)) {
    err = "rule has no tape sink";
    return false;
  }

  if (sink == "s3" || sink == "xrdcp") {
    const fs::path scratch = scratch_base(rule);
    const fs::path tmp = scratch / (safe_bag_filename(bag_id) + ".put");
    if (!write_file_atomic(tmp, body.data(), body.size(), err)) return false;
    bool ok = false;
    if (sink == "s3") ok = put_via_s3(bag_id, tmp, rule, uri_out, err);
    else ok = put_via_xrdcp(bag_id, tmp, rule, uri_out, err);
    std::error_code ec;
    fs::remove(tmp, ec);
    return ok;
  }

  // external: custom cmd or filesystem under tape_root
  const std::string tape_root = rule_root(rule);
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
  const std::string& uri = uri_it->second;
  if (!rule) {
    err = "no tape rule configured";
    return false;
  }
  const std::string sink = rule_sink(rule);
  if (!tape_sink_drains(sink)) {
    err = "rule has no tape sink";
    return false;
  }
  const std::uint64_t max_bytes = max_tape_object_bytes(rule);

  if (sink == "s3" || sink == "xrdcp") {
    // The recorded URI must be one this rule produced; otherwise `aws s3 cp` /
    // `xrdcp` would happily copy an arbitrary local path into the object.
    if (!uri_matches_prefix(uri, rule_prefix(rule))) {
      err = "tape_uri does not match rule tape_uri_prefix";
      return false;
    }
    const fs::path scratch = scratch_base(rule);
    std::error_code ec;
    fs::create_directories(scratch, ec);
    const fs::path tmp = scratch / (safe_bag_filename(bag_id) + ".get");
    bool ok = false;
    if (sink == "s3") ok = get_via_s3(uri, tmp, rule, err);
    else ok = get_via_xrdcp(uri, tmp, rule, err);
    if (!ok) {
      fs::remove(tmp, ec);
      return false;
    }
    if (!read_file_bytes(tmp, max_bytes, body_out, err)) {
      fs::remove(tmp, ec);
      return false;
    }
    fs::remove(tmp, ec);
    return true;
  }

  const std::string tape_root = rule_root(rule);
  const std::string get_cmd = rule->tape_get_cmd;
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
    if (!run_external(get_cmd, {uri, tmp.string()}, line, err)) return false;
    if (!read_file_bytes(tmp, max_bytes, body_out, err)) {
      fs::remove(tmp, ec);
      return false;
    }
    fs::remove(tmp, ec);
    return true;
  }
  fs::path path;
  if (!resolve_tape_path(tape_root, uri, path, err)) return false;
  return read_file_bytes(path, max_bytes, body_out, err);
}

bool ensure_bag_on_staging(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                           LocalStores& stores, const std::string& bag_id,
                           std::unordered_map<std::string, std::string>& bag_attrs,
                           std::string& err) try {
  ObjectInfo info;
  if (!local_bag_tip(stores, bag_id, info, bag_attrs, err)) return false;

  if (info.size > 0) return true;

  auto sink = bag_attrs.find(kTapeSinkAttr);
  if (sink == bag_attrs.end() || sink->second.empty()) {
    err = "bag body missing and no tape sink";
    return false;
  }
  const ArchiveRule* rule = find_tape_rule_for_attrs(cfg, bag_attrs);
  if (!rule) {
    err = "no tape rule configured for bag";
    return false;
  }
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
  bool tip_moved = false;
  if (!install_replica_version(cfg, advertise, map, stores, dest, bag_id, body, bag_attrs,
                               info.seq, nullptr, &tip_moved)) {
    err = tip_moved ? "bag tip changed during restore" : "install restored bag failed";
    return false;
  }
  return true;
} catch (const std::exception& e) {
  AIOS_LOG_ERROR("tape restore ", bag_id, ": ", e.what());
  err = std::string("tape restore failed: ") + e.what();
  return false;
}

bool drain_one_bag(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                   LocalStores& stores, const std::string& bag_id) try {
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
  if (!rule) {
    AIOS_LOG_WARN("archive drain ", bag_id, ": no tape rule configured");
    return false;
  }
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
  bool tip_moved = false;
  if (!install_replica_version(cfg, advertise, map, stores, dest, bag_id, empty, attrs, info.seq,
                               nullptr, &tip_moved)) {
    AIOS_LOG_WARN("archive drain reclaim ", bag_id,
                  tip_moved ? " skipped: bag tip changed" : " failed");
    return false;
  }
  return true;
} catch (const std::exception& e) {
  AIOS_LOG_ERROR("archive drain ", bag_id, ": ", e.what());
  return false;
}

ArchiveDrainStats run_archive_drain(const Config& cfg, const std::string& advertise,
                                    const ClusterMap& map, LocalStores& stores,
                                    std::size_t max_oids_per_store) try {
  ArchiveDrainStats stats;
  if (map.targets.empty()) return stats;

  for (const auto& path : stores.paths()) {
    auto store = stores.get_shared(path);
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
} catch (const std::exception& e) {
  AIOS_LOG_ERROR("archive drain pass aborted: ", e.what());
  ArchiveDrainStats stats;
  ++stats.failed;
  return stats;
}

}  // namespace aios
