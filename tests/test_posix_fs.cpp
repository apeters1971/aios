#include "test_helpers.hpp"

#include "client/session.hpp"
#include "http/http_server.hpp"
#include "posix/aios_posix.h"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <errno.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using aios::test::DualStoreFixture;
using aios::test::expect;
using aios::test::failures;

struct HttpFixture {
  DualStoreFixture fx;
  int port_num;
  std::string host{"127.0.0.1"};
  std::string port;
  boost::asio::io_context ioc;
  std::unique_ptr<aios::HttpServer> http;
  std::thread th;

  explicit HttpFixture(const char* prefix)
      : fx(prefix, 2, 2, "nvme"), port_num(19450 + static_cast<int>(::getpid() % 200)) {
    port = std::to_string(port_num);
    fx.cfg.http_listen = host + ":" + port;
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership);
    http->start();
    th = std::thread([this] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }

  ~HttpFixture() {
    ioc.stop();
    if (th.joinable()) th.join();
  }

  std::string endpoint() const { return host + ":" + port; }
};

}  // namespace

int test_posix_fs() {
  failures() = 0;
  HttpFixture http("aios-posix-fs");

  aios_posix_config cfg{};
  const std::string ep = http.endpoint();
  cfg.endpoint = ep.c_str();
  cfg.cluster_key = http.fx.cfg.cluster_key.c_str();
  cfg.volume = "vtest";
  cfg.stripe_unit = 4096;
  cfg.stripe_width = 2;
  cfg.uid = 1000;
  cfg.gid = 1000;

  // Seed path layout rules before mount so the first refresh sees them.
  {
    aios::SessionConfig sc;
    sc.endpoint = ep;
    sc.cluster_key = http.fx.cfg.cluster_key;
    aios::Session sess(std::move(sc));
    // Domains differ via layout field only (both use the fixture's nvme targets).
    nlohmann::json doc{
        {"aios_posix_layout", 1},
        {"rules",
         nlohmann::json::array(
             {{{"path", "/"},
               {"meta", {{"storage_class", "nvme"}}},
               {"data", {{"storage_class", "nvme"}}}},
              {{"path", "/cold"},
               {"meta", {{"layout", "replica"}, {"storage_class", "nvme"}}},
               {"data", {{"layout", "replica"}, {"storage_class", "nvme"}}}}})}};
    sess.put_bytes("posix/layout_rules", doc.dump());
  }

  int err = 0;
  aios_posix_fs* fs = aios_posix_mount(&cfg, &err);
  expect(fs != nullptr, "mount ok");
  if (!fs) return failures();

  aios_posix_stat st{};
  expect(aios_posix_getattr(fs, 1, &st) == 0, "root getattr");
  expect(st.ino == 1 && S_ISDIR(st.mode), "root is dir ino 1");

  expect(aios_posix_mkdir(fs, 1, "dir", 0755, &st) == 0, "mkdir");
  expect(S_ISDIR(st.mode), "mkdir mode");
  const uint64_t dir_ino = st.ino;

  expect(aios_posix_create(fs, dir_ino, "file.txt", 0644, &st) == 0, "create");
  expect(S_ISREG(st.mode), "file mode");
  const uint64_t file_ino = st.ino;

  aios_posix_stat looked{};
  expect(aios_posix_lookup(fs, dir_ino, "file.txt", &looked) == 0, "lookup");
  expect(looked.ino == file_ino, "lookup ino");

  const char* payload = "hello-posix-stripe";
  size_t wrote = 0;
  expect(aios_posix_write(fs, file_ino, 0, payload, std::strlen(payload), &wrote) == 0, "write");
  expect(wrote == std::strlen(payload), "write len");

  char buf[64]{};
  size_t got = 0;
  expect(aios_posix_read(fs, file_ino, 0, buf, sizeof(buf), &got) == 0, "read");
  expect(got == std::strlen(payload), "read len");
  expect(std::string(buf, got) == payload, "read data");

  // Multi-chunk write (stripe_unit=4096).
  std::string big(5000, 'Z');
  expect(aios_posix_write(fs, file_ino, 0, big.data(), big.size(), &wrote) == 0, "big write");
  expect(wrote == big.size(), "big write len");
  std::vector<char> rbuf(big.size());
  expect(aios_posix_read(fs, file_ino, 0, rbuf.data(), rbuf.size(), &got) == 0, "big read");
  expect(got == big.size() && std::string(rbuf.data(), got) == big, "big data");

  expect(aios_posix_truncate(fs, file_ino, 3) == 0, "truncate");
  expect(aios_posix_getattr(fs, file_ino, &st) == 0 && st.size == 3, "trunc size");

  uint64_t off = 0;
  aios_posix_dirent ents[16];
  int n = aios_posix_readdir(fs, dir_ino, &off, ents, 16);
  expect(n >= 3, "readdir has ., .., file");
  bool saw_file = false;
  for (int i = 0; i < n; ++i) {
    if (std::strcmp(ents[i].name, "file.txt") == 0) saw_file = true;
  }
  expect(saw_file, "readdir saw file");

  expect(aios_posix_rename(fs, dir_ino, "file.txt", dir_ino, "renamed.txt") == 0, "rename");
  expect(aios_posix_lookup(fs, dir_ino, "file.txt", &looked) == -ENOENT, "old name gone");
  expect(aios_posix_lookup(fs, dir_ino, "renamed.txt", &looked) == 0, "new name");

  // xattrs
  const char* xa_name = "user.aios";
  const char xa_val[] = "meta\0bin";
  expect(aios_posix_setxattr(fs, file_ino, xa_name, xa_val, sizeof(xa_val), 0) == 0, "setxattr");
  expect(aios_posix_setxattr(fs, file_ino, xa_name, "x", 1, AIOS_POSIX_XATTR_CREATE) < 0,
         "xattr create fail");
  char xbuf[32]{};
  int xsz = aios_posix_getxattr(fs, file_ino, xa_name, nullptr, 0);
  expect(xsz == static_cast<int>(sizeof(xa_val)), "getxattr size");
  expect(aios_posix_getxattr(fs, file_ino, xa_name, xbuf, sizeof(xbuf)) == xsz, "getxattr");
  expect(std::memcmp(xbuf, xa_val, sizeof(xa_val)) == 0, "xattr data");
  char list[64]{};
  int lsz = aios_posix_listxattr(fs, file_ino, list, sizeof(list));
  expect(lsz > 0 && std::strstr(list, xa_name) != nullptr, "listxattr");
  expect(aios_posix_removexattr(fs, file_ino, xa_name) == 0, "removexattr");
  expect(aios_posix_getxattr(fs, file_ino, xa_name, nullptr, 0) < 0, "xattr gone");

  // hard link (same dir) + second name across dirs
  expect(aios_posix_mkdir(fs, 1, "dir2", 0755, &st) == 0, "mkdir dir2");
  const uint64_t dir2 = st.ino;
  expect(aios_posix_link(fs, dir_ino, "renamed.txt", dir_ino, "alias.txt") == 0, "hardlink same");
  expect(aios_posix_getattr(fs, file_ino, &st) == 0 && st.nlink == 2, "nlink 2");
  expect(aios_posix_link(fs, dir_ino, "renamed.txt", dir2, "cross.txt") == 0, "hardlink cross");
  expect(aios_posix_getattr(fs, file_ino, &st) == 0 && st.nlink == 3, "nlink 3");
  char r2[8]{};
  size_t got2 = 0;
  expect(aios_posix_lookup(fs, dir2, "cross.txt", &looked) == 0 && looked.ino == file_ino,
         "cross lookup");
  expect(aios_posix_read(fs, looked.ino, 0, r2, sizeof(r2), &got2) == 0 && got2 == 3, "via link");

  // flock exclusive
  expect(aios_posix_flock(fs, file_ino, LOCK_EX | LOCK_NB) == 0, "flock ex");
  expect(aios_posix_flock(fs, file_ino, LOCK_EX | LOCK_NB) == 0, "flock reentrant renew");
  expect(aios_posix_flock(fs, file_ino, LOCK_UN) == 0, "flock un");

  // Cross-directory rename (multi-object txn).
  expect(aios_posix_mkdir(fs, 1, "dir3", 0755, &st) == 0, "mkdir dir3");
  const uint64_t dir3 = st.ino;
  expect(aios_posix_rename(fs, dir_ino, "alias.txt", dir3, "moved.txt") == 0, "cross rename");
  expect(aios_posix_lookup(fs, dir_ino, "alias.txt", &looked) == -ENOENT, "src gone");
  expect(aios_posix_lookup(fs, dir3, "moved.txt", &looked) == 0 && looked.ino == file_ino,
         "dst present");
  expect(aios_posix_getattr(fs, file_ino, &st) == 0 && st.nlink == 3, "nlink unchanged by rename");
  // Replace at destination via cross-dir rename.
  expect(aios_posix_create(fs, dir3, "victim.txt", 0644, &st) == 0, "victim");
  const uint64_t victim = st.ino;
  expect(aios_posix_rename(fs, dir_ino, "renamed.txt", dir3, "victim.txt") == 0,
         "cross rename replace");
  expect(aios_posix_getattr(fs, victim, &st) == -ENOENT, "victim unlinked");
  expect(aios_posix_lookup(fs, dir3, "victim.txt", &looked) == 0 && looked.ino == file_ino,
         "replaced name");
  expect(aios_posix_getattr(fs, file_ino, &st) == 0 && st.nlink == 3, "nlink after replace move");

  expect(aios_posix_unlink(fs, dir2, "cross.txt") == 0, "unlink cross");
  expect(aios_posix_unlink(fs, dir3, "moved.txt") == 0, "unlink moved");
  expect(aios_posix_unlink(fs, dir3, "victim.txt") == 0, "unlink last");
  expect(aios_posix_getattr(fs, file_ino, &st) == -ENOENT, "inode gone");
  expect(aios_posix_rmdir(fs, 1, "dir3") == 0, "rmdir dir3");
  expect(aios_posix_rmdir(fs, 1, "dir2") == 0, "rmdir dir2");
  expect(aios_posix_rmdir(fs, 1, "dir") == 0, "rmdir");

  // Caller credentials: ownership + mode checks.
  // Parent must be world-writable so non-mount uids can create under root (0755).
  aios_posix_set_caller(fs, 0, 0);
  expect(aios_posix_mkdir(fs, 1, "priv", 0777, &st) == 0, "mkdir staging");
  const uint64_t priv = st.ino;
  aios_posix_set_caller(fs, 2000, 2000);
  auto cred = aios_posix_get_caller(fs);
  expect(cred.uid == 2000 && cred.gid == 2000, "get_caller");
  expect(aios_posix_create(fs, priv, "secret", 0600, &st) == 0, "create as A");
  const uint64_t secret = st.ino;
  expect(st.uid == 2000 && st.gid == 2000, "create owned by caller");
  expect(aios_posix_write(fs, secret, 0, "ok", 2, &wrote) == 0 && wrote == 2, "A write");
  expect(aios_posix_read(fs, secret, 0, buf, sizeof(buf), &got) == 0 && got == 2, "A read");

  aios_posix_set_caller(fs, 3000, 3000);
  expect(aios_posix_read(fs, secret, 0, buf, sizeof(buf), &got) == -EACCES, "B read EACCES");
  expect(aios_posix_write(fs, secret, 0, "no", 2, &wrote) == -EACCES, "B write EACCES");
  expect(aios_posix_access(fs, secret, R_OK) == -EACCES, "B access R");
  expect(aios_posix_access(fs, secret, W_OK) == -EACCES, "B access W");

  aios_posix_set_caller(fs, 0, 0);
  expect(aios_posix_read(fs, secret, 0, buf, sizeof(buf), &got) == 0 && got == 2, "root read");
  expect(aios_posix_write(fs, secret, 0, "rz", 2, &wrote) == 0, "root write");
  expect(aios_posix_access(fs, secret, R_OK | W_OK) == 0, "root access");

  aios_posix_set_caller(fs, 2000, 2000);
  expect(aios_posix_unlink(fs, priv, "secret") == 0, "A unlink");
  aios_posix_set_caller(fs, 0, 0);
  expect(aios_posix_rmdir(fs, 1, "priv") == 0, "root rmdir");
  aios_posix_clear_caller(fs);
  cred = aios_posix_get_caller(fs);
  expect(cred.uid == 1000 && cred.gid == 1000, "clear → mount defaults");

  // parent_ino + lazy recursive directory stats (aios.r*)
  expect(aios_posix_mkdir(fs, 1, "roll", 0755, &st) == 0, "mkdir roll");
  const uint64_t roll = st.ino;
  expect(st.parent_ino == 1, "roll parent_ino root");
  expect(aios_posix_mkdir(fs, roll, "nested", 0755, &st) == 0, "mkdir nested");
  const uint64_t nested = st.ino;
  expect(st.parent_ino == roll, "nested parent_ino");
  expect(aios_posix_create(fs, nested, "blob", 0644, &st) == 0, "create blob");
  const uint64_t blob = st.ino;
  expect(st.parent_ino == nested, "blob parent_ino");
  const char* roll_payload = "rollup-bytes";
  expect(aios_posix_write(fs, blob, 0, roll_payload, std::strlen(roll_payload), &wrote) == 0,
         "write blob");
  aios_posix_flush_rstats(fs);
  auto read_u64_xattr = [&](uint64_t ino, const char* name) -> uint64_t {
    char xb[32]{};
    int n = aios_posix_getxattr(fs, ino, name, xb, sizeof(xb));
    if (n <= 0) return UINT64_MAX;
    return std::stoull(std::string(xb, static_cast<size_t>(n)));
  };
  expect(read_u64_xattr(nested, "aios.rbytes") == std::strlen(roll_payload), "nested rbytes");
  expect(read_u64_xattr(nested, "aios.rfiles") == 1, "nested rfiles");
  expect(read_u64_xattr(nested, "aios.rdirs") == 0, "nested rdirs");
  expect(read_u64_xattr(roll, "aios.rbytes") == std::strlen(roll_payload), "roll rbytes");
  expect(read_u64_xattr(roll, "aios.rfiles") == 1, "roll rfiles");
  expect(read_u64_xattr(roll, "aios.rdirs") == 1, "roll rdirs");
  expect(read_u64_xattr(nested, "aios.rtime") != UINT64_MAX, "nested rtime");
  char xlist[128]{};
  int xlsz = aios_posix_listxattr(fs, nested, xlist, sizeof(xlist));
  expect(xlsz > 0 && std::strstr(xlist, "aios.rbytes") != nullptr, "listxattr rbytes");
  expect(aios_posix_setxattr(fs, nested, "aios.rbytes", "0", 1, 0) == -EPERM, "rbytes EPERM");
  expect(aios_posix_mkdir(fs, 1, "roll2", 0755, &st) == 0, "mkdir roll2");
  const uint64_t roll2 = st.ino;
  expect(aios_posix_rename(fs, nested, "blob", roll2, "blob") == 0, "cross rename blob");
  expect(aios_posix_getattr(fs, blob, &st) == 0 && st.parent_ino == roll2, "parent after rename");
  aios_posix_flush_rstats(fs);
  expect(read_u64_xattr(nested, "aios.rbytes") == 0, "nested empty after move");
  expect(read_u64_xattr(roll2, "aios.rbytes") == std::strlen(roll_payload), "roll2 rbytes");
  expect(aios_posix_unlink(fs, roll2, "blob") == 0, "unlink blob");
  aios_posix_flush_rstats(fs);
  expect(read_u64_xattr(roll2, "aios.rbytes") == 0, "roll2 empty after unlink");
  expect(read_u64_xattr(roll2, "aios.rfiles") == 0, "roll2 rfiles 0");
  expect(aios_posix_rmdir(fs, roll, "nested") == 0, "rmdir nested");
  expect(aios_posix_rmdir(fs, 1, "roll") == 0, "rmdir roll");
  expect(aios_posix_rmdir(fs, 1, "roll2") == 0, "rmdir roll2");

  // Cross layout-domain rename must return EXDEV (copy, don't move tips).
  expect(aios_posix_mkdir(fs, 1, "cold", 0755, &st) == 0, "mkdir cold");
  const uint64_t cold_ino = st.ino;
  expect(aios_posix_create(fs, 1, "warm.txt", 0644, &st) == 0, "create warm");
  expect(aios_posix_rename(fs, 1, "warm.txt", cold_ino, "warm.txt") == -EXDEV,
         "rename across layout domains EXDEV");
  expect(aios_posix_rename(fs, 1, "warm.txt", 1, "warm2.txt") == 0,
         "rename within same layout domain");
  expect(aios_posix_unlink(fs, 1, "warm2.txt") == 0, "unlink warm2");
  expect(aios_posix_rmdir(fs, 1, "cold") == 0, "rmdir cold");

  aios_posix_unmount(fs);
  return failures();
}
