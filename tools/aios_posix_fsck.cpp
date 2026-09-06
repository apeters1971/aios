// aios-posix-fsck: check (and with --repair, fix) one libaios_posix volume.
//
// Works on the object layout through the HTTP API; no mount is needed and an
// active mount is tolerated (repairs are CAS-guarded and skip objects younger
// than --min-age). Exit status follows fsck(8): 0 clean, 1 findings repaired,
// 4 findings left, 8 operational error.

#include "client/session.hpp"
#include "posix/posix_fsck.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void usage(const char* argv0) {
  std::fprintf(stderr,
               "Usage: %s --volume NAME [--endpoint HOST:PORT] [--repair] [--min-age SECONDS]\n"
               "          [--quiet] (--cluster-key KEY | --principal NAME --principal-key HEX)\n"
               "\n"
               "  --volume NAME        libaios_posix volume (the NAME in posix/NAME/...)\n"
               "  --endpoint HOST:PORT HTTP API of any node (default 127.0.0.1:7480)\n"
               "  --repair             fix what can be fixed; default is check only\n"
               "  --min-age SECONDS    never repair objects modified more recently (default 600)\n"
               "  --quiet              summary only\n"
               "  --cluster-key KEY    or AIOS_CLUSTER_KEY; --principal / --principal-key for ticket auth\n"
               "\n"
               "Exit status: 0 clean, 1 all findings repaired, 4 findings remain, 8 error.\n",
               argv0);
}

}  // namespace

int main(int argc, char** argv) {
  aios::SessionConfig cfg;
  aios::posix::FsckOptions opt;
  std::string volume;
  bool quiet = false;
  if (const char* env = std::getenv("AIOS_CLUSTER_KEY")) cfg.cluster_key = env;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", what);
        std::exit(8);
      }
      return argv[++i];
    };
    if (a == "--volume") volume = need("--volume");
    else if (a == "--endpoint") cfg.endpoint = need("--endpoint");
    else if (a == "--cluster-key") cfg.cluster_key = need("--cluster-key");
    else if (a == "--principal") cfg.principal = need("--principal");
    else if (a == "--principal-key") cfg.principal_key = need("--principal-key");
    else if (a == "--repair") opt.repair = true;
    else if (a == "--min-age") opt.min_age = std::chrono::seconds(std::atol(need("--min-age")));
    else if (a == "--quiet") quiet = true;
    else if (a == "-h" || a == "--help") {
      usage(argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      usage(argv[0]);
      return 8;
    }
  }
  if (volume.empty()) {
    usage(argv[0]);
    return 8;
  }
  if (cfg.cluster_key.empty() && cfg.principal.empty()) {
    std::fprintf(stderr, "credentials required: --cluster-key / AIOS_CLUSTER_KEY or --principal\n");
    return 8;
  }
  if (!quiet) opt.log = [](const std::string& line) { std::fprintf(stdout, "%s\n", line.c_str()); };

  aios::posix::FsckReport rep;
  try {
    aios::Session session(cfg);
    rep = aios::posix::fsck_volume(session, volume, opt);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "aios-posix-fsck: %s\n", e.what());
    return 8;
  }

  std::fprintf(stdout,
               "%s: %llu directories, %llu files, %llu symlinks, %llu bytes; %llu inode objects, "
               "%llu chunk objects\n",
               volume.c_str(), static_cast<unsigned long long>(rep.dirs),
               static_cast<unsigned long long>(rep.files), static_cast<unsigned long long>(rep.symlinks),
               static_cast<unsigned long long>(rep.bytes), static_cast<unsigned long long>(rep.inode_objects),
               static_cast<unsigned long long>(rep.chunk_objects));
  if (rep.clean()) {
    std::fprintf(stdout, "%s: clean\n", volume.c_str());
    return 0;
  }
  std::size_t young = 0, unfixable = 0;
  for (const auto& f : rep.findings) {
    if (f.skipped_young) ++young;
    if (!f.repairable) ++unfixable;
  }
  std::fprintf(stdout, "%s: %zu finding(s), %zu repaired, %zu skipped (younger than min-age), %zu not repairable%s\n",
               volume.c_str(), rep.findings.size(), rep.repaired, young, unfixable,
               opt.repair ? "" : "  [check only; re-run with --repair]");
  if (rep.fatal) return 8;
  return rep.unrepaired() == 0 ? 1 : 4;
}
