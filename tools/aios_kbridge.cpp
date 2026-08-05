/* Userspace half of the aiosfs kernel prototype.
 * Serves /dev/aios_bridge requests by calling libaios_posix.
 *
 *   aios-kbridge [/dev/aios_bridge]
 *   # then: mount -t aios none /mnt -o endpoint=HOST:PORT,cluster_key=KEY,volume=default
 */

#include "posix/aios_posix.h"
#include "aios_kabi.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

void fill_kstat(const aios_posix_stat& in, aios_kabi_stat* out) {
  std::memset(out, 0, sizeof(*out));
  out->ino = in.ino;
  out->mode = in.mode;
  out->nlink = in.nlink;
  out->uid = in.uid;
  out->gid = in.gid;
  out->size = in.size;
  out->atime_ns = in.atime_ns;
  out->mtime_ns = in.mtime_ns;
  out->ctime_ns = in.ctime_ns;
}

struct Mount {
  aios_posix_fs* fs{nullptr};
};

std::mutex g_mu;
std::unordered_map<int, std::unique_ptr<Mount>> g_mounts;
int g_next_mount_id{1};

bool write_reply(int fd, uint64_t unique, int32_t result, const void* payload,
                 uint32_t payload_len) {
  aios_kabi_rep_hdr hdr{};
  hdr.magic = AIOS_KABI_MAGIC;
  hdr.version = AIOS_KABI_VERSION;
  hdr.unique = unique;
  hdr.result = result;
  hdr.payload_len = payload_len;
  if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) return false;
  if (payload_len && write(fd, payload, payload_len) != (ssize_t)payload_len) return false;
  return true;
}

int handle_mount(const aios_kabi_mount_in* in, aios_kabi_mount_out* out) {
  aios_posix_config cfg{};
  cfg.endpoint = in->endpoint;
  cfg.cluster_key = in->cluster_key;
  cfg.volume = in->volume[0] ? in->volume : "default";
  cfg.app_label = in->app_label[0] ? in->app_label : nullptr;
  cfg.stripe_unit = in->stripe_unit;
  cfg.stripe_width = in->stripe_width;
  cfg.uid = in->uid;
  cfg.gid = in->gid;
  cfg.rstat_interval_ms = 60000;

  int err = 0;
  aios_posix_fs* fs = aios_posix_mount(&cfg, &err);
  if (!fs) return err ? -err : -EIO;

  aios_posix_stat st{};
  int rc = aios_posix_getattr(fs, 1, &st);
  if (rc) {
    aios_posix_unmount(fs);
    return rc;
  }

  auto m = std::make_unique<Mount>();
  m->fs = fs;
  int id = 0;
  {
    std::lock_guard lock(g_mu);
    id = g_next_mount_id++;
    g_mounts[id] = std::move(m);
  }
  out->mount_id = id;
  fill_kstat(st, &out->root);
  std::fprintf(stderr, "aios-kbridge: mount_id=%d endpoint=%s volume=%s\n", id, in->endpoint,
               cfg.volume);
  return 0;
}

aios_posix_fs* fs_for(int mount_id) {
  std::lock_guard lock(g_mu);
  auto it = g_mounts.find(mount_id);
  if (it == g_mounts.end()) return nullptr;
  return it->second->fs;
}

void handle_umount(int mount_id) {
  std::unique_ptr<Mount> m;
  {
    std::lock_guard lock(g_mu);
    auto it = g_mounts.find(mount_id);
    if (it == g_mounts.end()) return;
    m = std::move(it->second);
    g_mounts.erase(it);
  }
  if (m && m->fs) aios_posix_unmount(m->fs);
  std::fprintf(stderr, "aios-kbridge: umount mount_id=%d\n", mount_id);
}

bool serve_one(int fd, const aios_kabi_req_hdr& hdr, const std::vector<uint8_t>& payload) {
  switch (hdr.opcode) {
    case AIOS_OP_MOUNT: {
      if (payload.size() < sizeof(aios_kabi_mount_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      aios_kabi_mount_out out{};
      int rc = handle_mount(reinterpret_cast<const aios_kabi_mount_in*>(payload.data()), &out);
      if (rc) return write_reply(fd, hdr.unique, rc, nullptr, 0);
      return write_reply(fd, hdr.unique, 0, &out, sizeof(out));
    }
    case AIOS_OP_UMOUNT: {
      handle_umount(hdr.mount_id);
      return write_reply(fd, hdr.unique, 0, nullptr, 0);
    }
    default:
      break;
  }

  aios_posix_fs* fs = fs_for(hdr.mount_id);
  if (!fs) return write_reply(fd, hdr.unique, -ENOTCONN, nullptr, 0);

  switch (hdr.opcode) {
    case AIOS_OP_LOOKUP: {
      if (payload.size() < sizeof(aios_kabi_lookup_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_lookup_in*>(payload.data());
      aios_posix_stat st{};
      int rc = aios_posix_lookup(fs, in->parent, in->name, &st);
      if (rc) return write_reply(fd, hdr.unique, rc, nullptr, 0);
      aios_kabi_stat out{};
      fill_kstat(st, &out);
      return write_reply(fd, hdr.unique, 0, &out, sizeof(out));
    }
    case AIOS_OP_GETATTR: {
      if (payload.size() < sizeof(aios_kabi_ino_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_ino_in*>(payload.data());
      aios_posix_stat st{};
      int rc = aios_posix_getattr(fs, in->ino, &st);
      if (rc) return write_reply(fd, hdr.unique, rc, nullptr, 0);
      aios_kabi_stat out{};
      fill_kstat(st, &out);
      return write_reply(fd, hdr.unique, 0, &out, sizeof(out));
    }
    case AIOS_OP_READDIR: {
      if (payload.size() < sizeof(aios_kabi_readdir_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_readdir_in*>(payload.data());
      uint32_t max = in->max_entries ? in->max_entries : 64;
      if (max > 256) max = 256;
      std::vector<aios_posix_dirent> ents(max);
      uint64_t off = in->offset;
      int n = aios_posix_readdir(fs, in->ino, &off, ents.data(), max);
      if (n < 0) return write_reply(fd, hdr.unique, n, nullptr, 0);
      std::vector<uint8_t> out(sizeof(aios_kabi_readdir_out) +
                               static_cast<size_t>(n) * sizeof(aios_kabi_dirent));
      auto* hdr_out = reinterpret_cast<aios_kabi_readdir_out*>(out.data());
      hdr_out->next_offset = off;
      hdr_out->count = static_cast<uint32_t>(n);
      auto* dout = reinterpret_cast<aios_kabi_dirent*>(hdr_out + 1);
      for (int i = 0; i < n; ++i) {
        std::memset(&dout[i], 0, sizeof(dout[i]));
        dout[i].ino = ents[i].ino;
        dout[i].mode = ents[i].mode;
        std::snprintf(dout[i].name, sizeof(dout[i].name), "%s", ents[i].name);
      }
      return write_reply(fd, hdr.unique, 0, out.data(), static_cast<uint32_t>(out.size()));
    }
    case AIOS_OP_CREATE:
    case AIOS_OP_MKDIR: {
      if (payload.size() < sizeof(aios_kabi_create_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_create_in*>(payload.data());
      aios_posix_stat st{};
      int rc = (hdr.opcode == AIOS_OP_MKDIR)
                   ? aios_posix_mkdir(fs, in->parent, in->name, in->mode, &st)
                   : aios_posix_create(fs, in->parent, in->name, in->mode, &st);
      if (rc) return write_reply(fd, hdr.unique, rc, nullptr, 0);
      aios_kabi_stat out{};
      fill_kstat(st, &out);
      return write_reply(fd, hdr.unique, 0, &out, sizeof(out));
    }
    case AIOS_OP_UNLINK:
    case AIOS_OP_RMDIR: {
      if (payload.size() < sizeof(aios_kabi_unlink_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_unlink_in*>(payload.data());
      int rc = (hdr.opcode == AIOS_OP_RMDIR) ? aios_posix_rmdir(fs, in->parent, in->name)
                                             : aios_posix_unlink(fs, in->parent, in->name);
      return write_reply(fd, hdr.unique, rc, nullptr, 0);
    }
    case AIOS_OP_RENAME: {
      if (payload.size() < sizeof(aios_kabi_rename_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_rename_in*>(payload.data());
      int rc = aios_posix_rename(fs, in->old_parent, in->old_name, in->new_parent, in->new_name);
      return write_reply(fd, hdr.unique, rc, nullptr, 0);
    }
    case AIOS_OP_LINK: {
      if (payload.size() < sizeof(aios_kabi_link_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_link_in*>(payload.data());
      int rc = aios_posix_link(fs, in->old_parent, in->old_name, in->new_parent, in->new_name);
      return write_reply(fd, hdr.unique, rc, nullptr, 0);
    }
    case AIOS_OP_READ: {
      if (payload.size() < sizeof(aios_kabi_rw_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_rw_in*>(payload.data());
      uint32_t n = in->size;
      if (n > AIOS_KABI_MAX_PAYLOAD) n = AIOS_KABI_MAX_PAYLOAD;
      std::vector<uint8_t> out(sizeof(aios_kabi_rw_out) + n);
      auto* hdr_out = reinterpret_cast<aios_kabi_rw_out*>(out.data());
      size_t got = 0;
      int rc = aios_posix_read(fs, in->ino, in->offset, hdr_out + 1, n, &got);
      if (rc) return write_reply(fd, hdr.unique, rc, nullptr, 0);
      hdr_out->size = static_cast<uint32_t>(got);
      return write_reply(fd, hdr.unique, 0, out.data(),
                         static_cast<uint32_t>(sizeof(*hdr_out) + got));
    }
    case AIOS_OP_WRITE: {
      if (payload.size() < sizeof(aios_kabi_rw_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_rw_in*>(payload.data());
      if (payload.size() < sizeof(*in) + in->size)
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      size_t wrote = 0;
      int rc = aios_posix_write(fs, in->ino, in->offset, in + 1, in->size, &wrote);
      if (rc) return write_reply(fd, hdr.unique, rc, nullptr, 0);
      aios_kabi_rw_out out{};
      out.size = static_cast<uint32_t>(wrote);
      return write_reply(fd, hdr.unique, 0, &out, sizeof(out));
    }
    case AIOS_OP_TRUNCATE: {
      if (payload.size() < sizeof(aios_kabi_truncate_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_truncate_in*>(payload.data());
      return write_reply(fd, hdr.unique, aios_posix_truncate(fs, in->ino, in->size), nullptr, 0);
    }
    case AIOS_OP_SETATTR: {
      if (payload.size() < sizeof(aios_kabi_setattr_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_setattr_in*>(payload.data());
      aios_posix_stat st{};
      st.mode = in->st.mode;
      st.uid = in->st.uid;
      st.gid = in->st.gid;
      st.size = in->st.size;
      st.mtime_ns = in->st.mtime_ns;
      st.atime_ns = in->st.atime_ns;
      return write_reply(fd, hdr.unique,
                         aios_posix_setattr(fs, in->ino, &st, in->to_set), nullptr, 0);
    }
    case AIOS_OP_FSYNC: {
      if (payload.size() < sizeof(aios_kabi_ino_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_ino_in*>(payload.data());
      return write_reply(fd, hdr.unique, aios_posix_fsync(fs, in->ino), nullptr, 0);
    }
    case AIOS_OP_STATFS: {
      aios_posix_statvfs st{};
      int rc = aios_posix_statfs(fs, &st);
      if (rc) return write_reply(fd, hdr.unique, rc, nullptr, 0);
      aios_kabi_statvfs out{};
      out.blocks = st.blocks;
      out.bfree = st.bfree;
      out.bavail = st.bavail;
      out.files = st.files;
      out.ffree = st.ffree;
      out.bsize = st.bsize;
      out.namemax = st.namemax;
      return write_reply(fd, hdr.unique, 0, &out, sizeof(out));
    }
    case AIOS_OP_SETXATTR: {
      if (payload.size() < sizeof(aios_kabi_setxattr_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_setxattr_in*>(payload.data());
      if (payload.size() < sizeof(*in) + in->value_len)
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      const void* val = in->value_len ? static_cast<const void*>(in + 1) : nullptr;
      int rc = aios_posix_setxattr(fs, in->ino, in->name, val, in->value_len, in->flags);
      return write_reply(fd, hdr.unique, rc, nullptr, 0);
    }
    case AIOS_OP_GETXATTR: {
      if (payload.size() < sizeof(aios_kabi_getxattr_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_getxattr_in*>(payload.data());
      if (in->size == 0) {
        int rc = aios_posix_getxattr(fs, in->ino, in->name, nullptr, 0);
        if (rc < 0) return write_reply(fd, hdr.unique, rc, nullptr, 0);
        aios_kabi_xattr_out out{};
        out.size = static_cast<uint32_t>(rc);
        return write_reply(fd, hdr.unique, 0, &out, sizeof(out));
      }
      uint32_t n = in->size;
      if (n > AIOS_KABI_XATTR_VALUE_MAX) n = AIOS_KABI_XATTR_VALUE_MAX;
      std::vector<uint8_t> buf(sizeof(aios_kabi_xattr_out) + n);
      auto* hdr_out = reinterpret_cast<aios_kabi_xattr_out*>(buf.data());
      int rc = aios_posix_getxattr(fs, in->ino, in->name, hdr_out + 1, n);
      if (rc < 0) return write_reply(fd, hdr.unique, rc, nullptr, 0);
      hdr_out->size = static_cast<uint32_t>(rc);
      return write_reply(fd, hdr.unique, 0, buf.data(),
                         static_cast<uint32_t>(sizeof(*hdr_out) + rc));
    }
    case AIOS_OP_LISTXATTR: {
      if (payload.size() < sizeof(aios_kabi_listxattr_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_listxattr_in*>(payload.data());
      if (in->size == 0) {
        int rc = aios_posix_listxattr(fs, in->ino, nullptr, 0);
        if (rc < 0) return write_reply(fd, hdr.unique, rc, nullptr, 0);
        aios_kabi_xattr_out out{};
        out.size = static_cast<uint32_t>(rc);
        return write_reply(fd, hdr.unique, 0, &out, sizeof(out));
      }
      uint32_t n = in->size;
      if (n > AIOS_KABI_MAX_PAYLOAD - sizeof(aios_kabi_xattr_out))
        n = AIOS_KABI_MAX_PAYLOAD - sizeof(aios_kabi_xattr_out);
      std::vector<uint8_t> buf(sizeof(aios_kabi_xattr_out) + n);
      auto* hdr_out = reinterpret_cast<aios_kabi_xattr_out*>(buf.data());
      int rc = aios_posix_listxattr(fs, in->ino, reinterpret_cast<char*>(hdr_out + 1), n);
      if (rc < 0) return write_reply(fd, hdr.unique, rc, nullptr, 0);
      hdr_out->size = static_cast<uint32_t>(rc);
      return write_reply(fd, hdr.unique, 0, buf.data(),
                         static_cast<uint32_t>(sizeof(*hdr_out) + rc));
    }
    case AIOS_OP_REMOVEXATTR: {
      if (payload.size() < sizeof(aios_kabi_removexattr_in))
        return write_reply(fd, hdr.unique, -EINVAL, nullptr, 0);
      auto* in = reinterpret_cast<const aios_kabi_removexattr_in*>(payload.data());
      int rc = aios_posix_removexattr(fs, in->ino, in->name);
      return write_reply(fd, hdr.unique, rc, nullptr, 0);
    }
    default:
      return write_reply(fd, hdr.unique, -ENOSYS, nullptr, 0);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const char* dev = "/dev/aios_bridge";
  if (argc > 1) dev = argv[1];

  int fd = open(dev, O_RDWR);
  if (fd < 0) {
    std::perror(dev);
    std::fprintf(stderr, "Load aiosfs.ko first (AlmaLinux 9).\n");
    return 1;
  }
  std::fprintf(stderr, "aios-kbridge: listening on %s\n", dev);

  std::vector<uint8_t> buf(sizeof(aios_kabi_req_hdr) + AIOS_KABI_MAX_PAYLOAD);
  while (true) {
    ssize_t n = read(fd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EMSGSIZE) {
        std::fprintf(stderr, "aios-kbridge: buffer too small\n");
        break;
      }
      std::perror("read");
      break;
    }
    if (n == 0) break;
    if ((size_t)n < sizeof(aios_kabi_req_hdr)) continue;
    aios_kabi_req_hdr hdr{};
    std::memcpy(&hdr, buf.data(), sizeof(hdr));
    if (hdr.magic != AIOS_KABI_MAGIC || hdr.version != AIOS_KABI_VERSION) {
      std::fprintf(stderr, "aios-kbridge: bad request header\n");
      continue;
    }
    if ((size_t)n < sizeof(hdr) + hdr.payload_len) continue;
    std::vector<uint8_t> payload(buf.begin() + sizeof(hdr),
                                 buf.begin() + sizeof(hdr) + hdr.payload_len);
    if (!serve_one(fd, hdr, payload)) {
      std::perror("write reply");
      break;
    }
  }

  {
    std::lock_guard lock(g_mu);
    for (auto& [id, m] : g_mounts) {
      if (m && m->fs) aios_posix_unmount(m->fs);
    }
    g_mounts.clear();
  }
  close(fd);
  return 0;
}
