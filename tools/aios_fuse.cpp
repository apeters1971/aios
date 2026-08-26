#include "posix/aios_posix.h"
#include "posix/fuse3_ops.hpp"

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
};

void usage(const char* argv0) {
  std::fprintf(stderr,
               "Usage: %s [FUSE options] -o endpoint=HOST:PORT,cluster_key=KEY[,volume=NAME] "
               "MOUNTPOINT\n",
               argv0);
}

bool is_aios_opt(const std::string& k) {
  return k == "endpoint" || k == "cluster_key" || k == "volume" || k == "app_label" ||
         k == "stripe_unit" || k == "stripe_width";
}

void apply_aios_opt(const std::string& k, const std::string& v, Options& opt) {
  if (k == "endpoint") opt.endpoint = v;
  else if (k == "cluster_key") opt.cluster_key = v;
  else if (k == "volume") opt.volume = v;
  else if (k == "app_label") opt.app_label = v;
  else if (k == "stripe_unit") opt.stripe_unit = std::stoull(v);
  else if (k == "stripe_width") opt.stripe_width = static_cast<uint32_t>(std::stoul(v));
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

  /* libfuse rejects unknown -o keys. Peel ours off; forward only FUSE leftovers. */
  std::vector<std::string> fuse_args;
  fuse_args.emplace_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      const std::string rest = take_aios_opts(argv[++i], opt);
      if (!rest.empty()) {
        fuse_args.emplace_back("-o");
        fuse_args.push_back(rest);
      }
      continue;
    }
    if (std::strncmp(argv[i], "-o", 2) == 0 && argv[i][2] != '\0') {
      const std::string rest = take_aios_opts(argv[i] + 2, opt);
      if (!rest.empty()) fuse_args.push_back(std::string("-o") + rest);
      continue;
    }
    fuse_args.emplace_back(argv[i]);
  }

  if (opt.cluster_key.empty()) {
    usage(argv[0]);
    std::fprintf(stderr, "cluster_key required (-o cluster_key=... or AIOS_CLUSTER_KEY)\n");
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

  int err = 0;
  aios_posix_fs* fs = aios_posix_mount(&cfg, &err);
  if (!fs) {
    std::fprintf(stderr, "aios_posix_mount failed errno=%d\n", err);
    return 1;
  }

  /* libfuse 3.10: session_new and init() must request the same max_read. */
  bool have_max_read = false;
  for (const auto& s : fuse_args) {
    if (s.find("max_read=") != std::string::npos) have_max_read = true;
  }
  if (!have_max_read) {
    fuse_args.emplace_back("-o");
    fuse_args.push_back("max_read=" + std::to_string(aios_fuse_max_io(fs)));
  }

  std::vector<char*> fuse_argv;
  fuse_argv.reserve(fuse_args.size() + 1);
  for (auto& s : fuse_args) fuse_argv.push_back(s.data());
  fuse_argv.push_back(nullptr);

  auto ops = aios_fuse_operations();
  const int rc = fuse_main(static_cast<int>(fuse_argv.size() - 1), fuse_argv.data(), &ops, fs);
  aios_posix_unmount(fs);
  return rc;
}
