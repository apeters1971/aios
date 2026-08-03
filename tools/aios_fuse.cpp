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

bool parse_kv(const std::string& s, Options& opt) {
  // comma-separated key=value
  size_t i = 0;
  while (i < s.size()) {
    size_t comma = s.find(',', i);
    std::string part = s.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
    auto eq = part.find('=');
    if (eq == std::string::npos) return false;
    std::string k = part.substr(0, eq);
    std::string v = part.substr(eq + 1);
    if (k == "endpoint") opt.endpoint = v;
    else if (k == "cluster_key") opt.cluster_key = v;
    else if (k == "volume") opt.volume = v;
    else if (k == "app_label") opt.app_label = v;
    else if (k == "stripe_unit") opt.stripe_unit = std::stoull(v);
    else if (k == "stripe_width") opt.stripe_width = static_cast<uint32_t>(std::stoul(v));
    else {
      // unknown keys left for FUSE
    }
    if (comma == std::string::npos) break;
    i = comma + 1;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (const char* env = std::getenv("AIOS_CLUSTER_KEY")) opt.cluster_key = env;
  if (const char* env = std::getenv("AIOS_ENDPOINT")) opt.endpoint = env;

  // Extract our -o options before fuse_main; pass the rest through.
  std::vector<char*> fuse_argv;
  fuse_argv.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      parse_kv(argv[i + 1], opt);
      // Still forward to FUSE (it ignores unknown keys if we strip ours... keep simple: forward all)
      fuse_argv.push_back(argv[i]);
      fuse_argv.push_back(argv[++i]);
      continue;
    }
    if (std::strncmp(argv[i], "-o", 2) == 0 && argv[i][2] != '\0') {
      parse_kv(argv[i] + 2, opt);
      fuse_argv.push_back(argv[i]);
      continue;
    }
    fuse_argv.push_back(argv[i]);
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
  cfg.app_label = opt.app_label.empty() ? nullptr : opt.app_label.c_str();
  cfg.stripe_unit = opt.stripe_unit;
  cfg.stripe_width = opt.stripe_width;
  cfg.uid = static_cast<uint32_t>(::geteuid());
  cfg.gid = static_cast<uint32_t>(::getegid());

  int err = 0;
  aios_posix_fs* fs = aios_posix_mount(&cfg, &err);
  if (!fs) {
    std::fprintf(stderr, "aios_posix_mount failed errno=%d\n", err);
    return 1;
  }

  auto ops = aios_fuse_operations();
  fuse_argv.push_back(nullptr);
  const int rc = fuse_main(static_cast<int>(fuse_argv.size() - 1), fuse_argv.data(), &ops, fs);
  aios_posix_unmount(fs);
  return rc;
}
