#include "kernel/aiosvd_uapi.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

void usage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0
      << " map --endpoint HOST:PORT --key KEY --pool POOL --name NAME [--size BYTES]\n"
      << "      [--obj-order N] [--create] [--excl] [--readonly] [--app-label L]\n"
      << "      [--key-id ID] [--queue-depth N] [--max-clients N]\n"
      << "  " << argv0 << " unmap DEV_ID\n"
      << "  " << argv0 << " info DEV_ID\n"
      << "  " << argv0 << " list\n"
      << "  " << argv0 << " resize DEV_ID --size BYTES\n"
      << "  " << argv0 << " clone SRC_DEV_ID --pool POOL --name NAME\n"
      << "  " << argv0 << " rename DEV_ID --pool POOL --name NAME\n"
      << "\n"
      << "Maps an AIOS volume device (aiosvd) — block device backed by object stripes\n"
      << "vd/{pool}/{name}/data.*. Creates /dev/aiosvdN.\n"
      << "clone creates a lightweight COW child header referencing the parent volume.\n";
}

uint64_t parse_size(const std::string& s) {
  char* end = nullptr;
  errno = 0;
  unsigned long long v = std::strtoull(s.c_str(), &end, 0);
  if (errno || end == s.c_str()) throw std::runtime_error("bad size");
  if (*end == '\0') return v;
  if ((end[0] == 'k' || end[0] == 'K') && end[1] == '\0') return v << 10;
  if ((end[0] == 'm' || end[0] == 'M') && end[1] == '\0') return v << 20;
  if ((end[0] == 'g' || end[0] == 'G') && end[1] == '\0') return v << 30;
  if ((end[0] == 't' || end[0] == 'T') && end[1] == '\0') return v << 40;
  throw std::runtime_error("bad size suffix");
}

int open_ctl() {
  int fd = ::open("/dev/" AIOSVD_CTL_NAME, O_RDWR);
  if (fd < 0) {
    std::cerr << "open /dev/" AIOSVD_CTL_NAME << ": " << std::strerror(errno)
              << " (load aiosvd.ko first)\n";
  }
  return fd;
}

void print_info(const aiosvd_info_arg& info) {
  std::cout << "/dev/" << AIOSVD_DISK_PREFIX << info.dev_id << "\n"
            << "  pool=" << info.pool << "\n"
            << "  name=" << info.name << "\n";
  if (info.parent_pool[0])
    std::cout << "  parent=" << info.parent_pool << "/" << info.parent_name << "\n";
  if (info.key_id[0]) std::cout << "  key_id=" << info.key_id << "\n";
  std::cout << "  size=" << info.size << "\n"
            << "  obj_order=" << info.obj_order << "\n"
            << "  flags=0x" << std::hex << info.flags << std::dec << "\n"
            << "  bytes_read=" << info.bytes_read << "\n"
            << "  bytes_written=" << info.bytes_written << "\n"
            << "  ops_read=" << info.ops_read << "\n"
            << "  ops_write=" << info.ops_write << "\n"
            << "  ops_discard=" << info.ops_discard << "\n"
            << "  errors=" << info.errors << "\n"
            << "  timeouts=" << info.timeouts << "\n"
            << "  reconnects=" << info.reconnects << "\n"
            << "  cache_hits=" << info.cache_hits << "\n";
}

int cmd_map(int argc, char** argv) {
  aiosvd_map_arg arg{};
  std::string endpoint, key, pool = "default", name, app, key_id;
  bool create = false, excl = false, readonly = false;
  uint64_t size = 0;
  uint32_t obj_order = 0;
  uint32_t queue_depth = 0;
  uint32_t max_clients = 0;

  for (int i = 0; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* opt) -> const char* {
      if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + opt);
      return argv[++i];
    };
    if (a == "--endpoint") endpoint = need("--endpoint");
    else if (a == "--key") key = need("--key");
    else if (a == "--pool") pool = need("--pool");
    else if (a == "--name") name = need("--name");
    else if (a == "--size") size = parse_size(need("--size"));
    else if (a == "--obj-order") obj_order = static_cast<uint32_t>(std::stoul(need("--obj-order")));
    else if (a == "--app-label") app = need("--app-label");
    else if (a == "--key-id") key_id = need("--key-id");
    else if (a == "--queue-depth")
      queue_depth = static_cast<uint32_t>(std::stoul(need("--queue-depth")));
    else if (a == "--max-clients")
      max_clients = static_cast<uint32_t>(std::stoul(need("--max-clients")));
    else if (a == "--create") create = true;
    else if (a == "--excl") excl = true;
    else if (a == "--readonly") readonly = true;
    else throw std::runtime_error("unknown arg: " + a);
  }
  if (endpoint.empty() || key.empty() || name.empty())
    throw std::runtime_error("endpoint, key, and name are required");
  if (create && size == 0) throw std::runtime_error("--create requires --size");
  if (excl && !create) throw std::runtime_error("--excl requires --create");

  std::snprintf(arg.endpoint, sizeof(arg.endpoint), "%s", endpoint.c_str());
  std::snprintf(arg.cluster_key, sizeof(arg.cluster_key), "%s", key.c_str());
  std::snprintf(arg.pool, sizeof(arg.pool), "%s", pool.c_str());
  std::snprintf(arg.name, sizeof(arg.name), "%s", name.c_str());
  if (app.empty()) app = "vbd";
  std::snprintf(arg.app_label, sizeof(arg.app_label), "%s", app.c_str());
  std::snprintf(arg.key_id, sizeof(arg.key_id), "%s", key_id.c_str());
  arg.size = size;
  arg.obj_order = obj_order;
  arg.queue_depth = queue_depth;
  arg.max_clients = max_clients;
  arg.flags = 0;
  if (create) arg.flags |= AIOSVD_MAP_CREATE;
  if (excl) arg.flags |= AIOSVD_MAP_EXCL;
  if (readonly) arg.flags |= AIOSVD_MAP_READONLY;

  int fd = open_ctl();
  if (fd < 0) return 1;
  if (ioctl(fd, AIOSVD_IOCTL_MAP, &arg) < 0) {
    std::cerr << "map: " << std::strerror(errno) << "\n";
    close(fd);
    return 1;
  }
  close(fd);
  std::cout << "/dev/" << AIOSVD_DISK_PREFIX << arg.dev_id << "  pool=" << arg.pool
            << " name=" << arg.name << " size=" << arg.size << " obj_order=" << arg.obj_order
            << "\n";
  return 0;
}

int cmd_unmap(int argc, char** argv) {
  if (argc != 1) throw std::runtime_error("unmap DEV_ID");
  aiosvd_unmap_arg u{};
  u.dev_id = std::stoi(argv[0]);
  int fd = open_ctl();
  if (fd < 0) return 1;
  if (ioctl(fd, AIOSVD_IOCTL_UNMAP, &u) < 0) {
    std::cerr << "unmap: " << std::strerror(errno) << "\n";
    close(fd);
    return 1;
  }
  close(fd);
  std::cout << "unmapped " << u.dev_id << "\n";
  return 0;
}

int cmd_info(int argc, char** argv) {
  if (argc != 1) throw std::runtime_error("info DEV_ID");
  aiosvd_info_arg info{};
  info.dev_id = std::stoi(argv[0]);
  int fd = open_ctl();
  if (fd < 0) return 1;
  if (ioctl(fd, AIOSVD_IOCTL_INFO, &info) < 0) {
    std::cerr << "info: " << std::strerror(errno) << "\n";
    close(fd);
    return 1;
  }
  close(fd);
  print_info(info);
  return 0;
}

int cmd_list(int argc, char** /*argv*/) {
  if (argc != 0) throw std::runtime_error("list takes no arguments");
  aiosvd_list_arg list{};
  int fd = open_ctl();
  if (fd < 0) return 1;
  if (ioctl(fd, AIOSVD_IOCTL_LIST, &list) < 0) {
    std::cerr << "list: " << std::strerror(errno) << "\n";
    close(fd);
    return 1;
  }
  close(fd);
  if (list.count == 0) {
    std::cout << "(no mapped volumes)\n";
    return 0;
  }
  for (uint32_t i = 0; i < list.count; ++i) {
    if (i) std::cout << "\n";
    print_info(list.entries[i]);
  }
  return 0;
}

int cmd_resize(int argc, char** argv) {
  if (argc < 1) throw std::runtime_error("resize DEV_ID --size BYTES");
  aiosvd_resize_arg r{};
  r.dev_id = std::stoi(argv[0]);
  uint64_t size = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--size") {
      if (i + 1 >= argc) throw std::runtime_error("missing value for --size");
      size = parse_size(argv[++i]);
    } else {
      throw std::runtime_error("unknown arg: " + a);
    }
  }
  if (!size) throw std::runtime_error("--size is required");
  r.new_size = size;

  int fd = open_ctl();
  if (fd < 0) return 1;
  if (ioctl(fd, AIOSVD_IOCTL_RESIZE, &r) < 0) {
    std::cerr << "resize: " << std::strerror(errno) << "\n";
    close(fd);
    return 1;
  }
  close(fd);
  std::cout << "/dev/" << AIOSVD_DISK_PREFIX << r.dev_id << " size=" << r.new_size << "\n";
  return 0;
}

int cmd_clone(int argc, char** argv) {
  if (argc < 1) throw std::runtime_error("clone SRC_DEV_ID --pool POOL --name NAME");
  aiosvd_clone_arg c{};
  c.src_dev_id = std::stoi(argv[0]);
  std::string pool, name;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* opt) -> const char* {
      if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + opt);
      return argv[++i];
    };
    if (a == "--pool") pool = need("--pool");
    else if (a == "--name") name = need("--name");
    else throw std::runtime_error("unknown arg: " + a);
  }
  if (pool.empty() || name.empty()) throw std::runtime_error("--pool and --name are required");
  std::snprintf(c.pool, sizeof(c.pool), "%s", pool.c_str());
  std::snprintf(c.name, sizeof(c.name), "%s", name.c_str());

  int fd = open_ctl();
  if (fd < 0) return 1;
  if (ioctl(fd, AIOSVD_IOCTL_CLONE, &c) < 0) {
    std::cerr << "clone: " << std::strerror(errno) << "\n";
    close(fd);
    return 1;
  }
  close(fd);
  std::cout << "/dev/" << AIOSVD_DISK_PREFIX << c.dest_dev_id << "  pool=" << c.pool
            << " name=" << c.name << " (clone of " << c.src_dev_id << ")\n";
  return 0;
}

int cmd_rename(int argc, char** argv) {
  if (argc < 1) throw std::runtime_error("rename DEV_ID --pool POOL --name NAME");
  aiosvd_rename_arg r{};
  r.dev_id = std::stoi(argv[0]);
  std::string pool, name;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* opt) -> const char* {
      if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + opt);
      return argv[++i];
    };
    if (a == "--pool") pool = need("--pool");
    else if (a == "--name") name = need("--name");
    else throw std::runtime_error("unknown arg: " + a);
  }
  if (pool.empty() || name.empty()) throw std::runtime_error("--pool and --name are required");
  std::snprintf(r.pool, sizeof(r.pool), "%s", pool.c_str());
  std::snprintf(r.name, sizeof(r.name), "%s", name.c_str());

  int fd = open_ctl();
  if (fd < 0) return 1;
  if (ioctl(fd, AIOSVD_IOCTL_RENAME, &r) < 0) {
    std::cerr << "rename: " << std::strerror(errno) << "\n";
    close(fd);
    return 1;
  }
  close(fd);
  std::cout << "/dev/" << AIOSVD_DISK_PREFIX << r.dev_id << " renamed to " << r.pool << "/"
            << r.name << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      usage(argv[0]);
      return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "map") return cmd_map(argc - 2, argv + 2);
    if (cmd == "unmap") return cmd_unmap(argc - 2, argv + 2);
    if (cmd == "info") return cmd_info(argc - 2, argv + 2);
    if (cmd == "list") return cmd_list(argc - 2, argv + 2);
    if (cmd == "resize") return cmd_resize(argc - 2, argv + 2);
    if (cmd == "clone") return cmd_clone(argc - 2, argv + 2);
    if (cmd == "rename") return cmd_rename(argc - 2, argv + 2);
    usage(argv[0]);
    return 2;
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
