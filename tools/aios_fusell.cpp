#include "posix/aios_posix.h"
#include "posix/fuse3_ll_ops.hpp"

#include <fuse3/fuse_opt.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct Options {
  std::string endpoint{"127.0.0.1:7480"};
  std::string cluster_key;
  std::string volume{"default"};
  std::string app_label;
  uint64_t stripe_unit{0};
  uint32_t stripe_width{0};
  bool nolease{false};
  std::string tls_ca;        // https:// endpoint: PEM bundle to trust
  bool tls_insecure{false};  // https:// endpoint: skip verification
};

void usage(const char* argv0) {
  std::fprintf(stderr,
               "Usage: %s [FUSE options] -o endpoint=[https://]HOST:PORT,cluster_key=KEY[,volume=NAME] "
               "MOUNTPOINT\n\n"
               "AIOS options: endpoint, cluster_key, volume, app_label, stripe_unit, "
               "stripe_width, nolease, tls_ca, tls_insecure\n",
               argv0);
  fuse_cmdline_help();
  fuse_lowlevel_help();
}

bool is_aios_opt(const std::string& k) {
  return k == "endpoint" || k == "cluster_key" || k == "volume" || k == "app_label" ||
         k == "stripe_unit" || k == "stripe_width" || k == "nolease" || k == "tls_ca" ||
         k == "tls_insecure";
}

void apply_aios_opt(const std::string& k, const std::string& v, Options& opt) {
  if (k == "endpoint") opt.endpoint = v;
  else if (k == "cluster_key") opt.cluster_key = v;
  else if (k == "volume") opt.volume = v;
  else if (k == "app_label") opt.app_label = v;
  else if (k == "stripe_unit") opt.stripe_unit = std::stoull(v);
  else if (k == "stripe_width") opt.stripe_width = static_cast<uint32_t>(std::stoul(v));
  else if (k == "nolease") opt.nolease = true;
  else if (k == "tls_ca") opt.tls_ca = v;
  else if (k == "tls_insecure") opt.tls_insecure = true;
}

/* Consume AIOS keys from a comma-separated -o list. Return leftover FUSE keys. */
std::string take_aios_opts(const std::string& s, Options& opt) {
  std::string rest;
  size_t i = 0;
  while (i < s.size()) {
    const size_t comma = s.find(',', i);
    const std::string part =
        s.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
    const auto eq = part.find('=');
    const std::string k = eq == std::string::npos ? part : part.substr(0, eq);
    const std::string v = eq == std::string::npos ? std::string() : part.substr(eq + 1);
    if (is_aios_opt(k)) {
      apply_aios_opt(k, v, opt);
    } else if (!part.empty()) {
      if (!rest.empty()) rest.push_back(',');
      rest += part;
    }
    if (comma == std::string::npos) break;
    i = comma + 1;
  }
  return rest;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (const char* env = std::getenv("AIOS_CLUSTER_KEY")) opt.cluster_key = env;
  if (const char* env = std::getenv("AIOS_ENDPOINT")) opt.endpoint = env;

  std::vector<std::string> leftover;
  leftover.emplace_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      const std::string rest = take_aios_opts(argv[++i], opt);
      if (!rest.empty()) {
        leftover.emplace_back("-o");
        leftover.push_back(rest);
      }
      continue;
    }
    if (std::strncmp(argv[i], "-o", 2) == 0 && argv[i][2] != '\0') {
      const std::string rest = take_aios_opts(argv[i] + 2, opt);
      if (!rest.empty()) leftover.push_back(std::string("-o") + rest);
      continue;
    }
    leftover.emplace_back(argv[i]);
  }

  struct fuse_args args = FUSE_ARGS_INIT(0, nullptr);
  for (const auto& s : leftover) {
    if (fuse_opt_add_arg(&args, s.c_str()) != 0) {
      std::fprintf(stderr, "fuse_opt_add_arg failed\n");
      fuse_opt_free_args(&args);
      return 1;
    }
  }

  struct fuse_cmdline_opts opts {};
  if (fuse_parse_cmdline(&args, &opts) != 0) {
    usage(argv[0]);
    fuse_opt_free_args(&args);
    return 2;
  }
  if (opts.show_help) {
    usage(argv[0]);
    std::free(opts.mountpoint);
    fuse_opt_free_args(&args);
    return 0;
  }
  if (opts.show_version) {
    fuse_lowlevel_version();
    std::free(opts.mountpoint);
    fuse_opt_free_args(&args);
    return 0;
  }
  if (!opts.mountpoint) {
    usage(argv[0]);
    std::fprintf(stderr, "mountpoint required\n");
    fuse_opt_free_args(&args);
    return 2;
  }

  if (opt.cluster_key.empty()) {
    usage(argv[0]);
    std::fprintf(stderr, "cluster_key required (-o cluster_key=... or AIOS_CLUSTER_KEY)\n");
    std::free(opts.mountpoint);
    fuse_opt_free_args(&args);
    return 2;
  }

  aios_posix_config cfg{};
  cfg.endpoint = opt.endpoint.c_str();
  cfg.cluster_key = opt.cluster_key.c_str();
  cfg.volume = opt.volume.c_str();
  if (opt.app_label.empty()) opt.app_label = "fs";
  cfg.app_label = opt.app_label.c_str();
  cfg.stripe_unit = opt.stripe_unit;
  cfg.stripe_width = opt.stripe_width;
  cfg.uid = static_cast<uint32_t>(::geteuid());
  cfg.gid = static_cast<uint32_t>(::getegid());
  cfg.rstat_interval_ms = 60000;
  if (!opt.tls_ca.empty()) cfg.tls_ca = opt.tls_ca.c_str();
  if (opt.tls_insecure) cfg.flags |= AIOS_POSIX_F_TLS_INSECURE;
  if (opt.nolease) cfg.flags |= AIOS_POSIX_F_NOLEASE;

  int err = 0;
  aios_posix_fs* fs = aios_posix_mount(&cfg, &err);
  if (!fs) {
    std::fprintf(stderr, "aios_posix_mount failed errno=%d\n", err);
    std::free(opts.mountpoint);
    fuse_opt_free_args(&args);
    return 1;
  }

  /* libfuse 3.10: session_new and init() must request the same max_read. */
  bool have_max_read = false;
  for (int i = 0; i < args.argc; ++i) {
    if (args.argv[i] && std::strstr(args.argv[i], "max_read=")) have_max_read = true;
  }
  if (!have_max_read) {
    const std::string max_read = "max_read=" + std::to_string(aios_fuse_ll_max_io(fs));
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, max_read.c_str());
  }

  int rc = 1;
  auto ops = aios_fuse_ll_operations();
  struct fuse_session* se = fuse_session_new(&args, &ops, sizeof(ops), fs);
  if (!se) goto out_unmount;
  if (fuse_set_signal_handlers(se) != 0) goto out_destroy;
  if (fuse_session_mount(se, opts.mountpoint) != 0) goto out_signals;
  fuse_daemonize(opts.foreground);
  if (opts.singlethread) {
    rc = fuse_session_loop(se);
  } else {
    rc = fuse_session_loop_mt(se, opts.clone_fd);
  }
  fuse_session_unmount(se);
out_signals:
  fuse_remove_signal_handlers(se);
out_destroy:
  fuse_session_destroy(se);
out_unmount:
  aios_posix_unmount(fs);
  std::free(opts.mountpoint);
  fuse_opt_free_args(&args);
  return rc == 0 ? 0 : 1;
}
