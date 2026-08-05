#pragma once

#include "posix/aios_posix.h"

#include <cstdint>
#include <string>
#include <sys/stat.h>

class XrdOucEnv;
class XrdSysError;

namespace aios {
namespace xrd {

struct MountConfig {
  std::string endpoint;
  std::string cluster_key;
  std::string volume{"default"};
  uint64_t stripe_unit{0};
  uint32_t stripe_width{0};
};

/* Parse ofs.osslib parms (key=value …) and aios.* lines from config_fn. */
bool load_config(const char* config_fn, const char* parms, MountConfig* out, XrdSysError* log);

int lookup_path(aios_posix_fs* fs, const char* path, aios_posix_stat* st_out);
int resolve_parent(aios_posix_fs* fs, const char* path, uint64_t* parent_out, std::string* name_out);

void fill_stat(const aios_posix_stat& st, struct stat* out);

/* Map env.secEnv()->name via PWD and set_caller. Missing env → clear_caller (mount defaults).
 * Present env without mappable name → -EACCES. */
int apply_caller(aios_posix_fs* fs, XrdOucEnv* env);
int apply_caller(aios_posix_fs* fs, XrdOucEnv& env);

int mkdir_p(aios_posix_fs* fs, const char* path, mode_t mode);

}  // namespace xrd
}  // namespace aios
