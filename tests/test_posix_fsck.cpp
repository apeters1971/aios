// aios-posix-fsck: build a volume with libaios_posix, damage it through the
// object API the way crashes and bugs would, and check that fsck reports every
// kind, repairs what it should, and leaves a clean volume behind.
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "client/session.hpp"
#include "http/http_server.hpp"
#include "posix/aios_posix.h"
#include "posix/posix_fsck.hpp"
#include "posix/posix_internal.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

using aios::test::DualStoreFixture;
using aios::posix::FsckFinding;
using aios::posix::FsckOptions;
using aios::posix::FsckReport;
using Kind = FsckFinding::Kind;

struct HttpFixture {
  DualStoreFixture fx;
  std::string port;
  boost::asio::io_context ioc;
  std::unique_ptr<aios::HttpServer> http;
  std::thread th;

  explicit HttpFixture(const char* prefix, int base_port)
      : fx(prefix, 2, 2, "nvme"), port(std::to_string(base_port + static_cast<int>(::getpid() % 200))) {
    fx.cfg.http_listen = "127.0.0.1:" + port;
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership);
    http->start();
    th = std::thread([this] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  ~HttpFixture() {
    ioc.stop();
    if (th.joinable()) th.join();
  }
  std::string endpoint() const { return "127.0.0.1:" + port; }
  aios::SessionConfig session_cfg() const {
    return aios::SessionConfig{.endpoint = endpoint(), .cluster_key = fx.cfg.cluster_key};
  }
};

struct Mount {
  std::string ep, key, vol;
  aios_posix_fs* fs{nullptr};
  Mount(const HttpFixture& http, const char* volume)
      : ep(http.endpoint()), key(http.fx.cfg.cluster_key), vol(volume) {
    aios_posix_config cfg{};
    cfg.endpoint = ep.c_str();
    cfg.cluster_key = key.c_str();
    cfg.volume = vol.c_str();
    cfg.stripe_unit = 4096;
    cfg.stripe_width = 1;
    cfg.uid = 1000;
    cfg.gid = 1000;
    int err = 0;
    fs = aios_posix_mount(&cfg, &err);
    EXPECT_NE(fs, nullptr) << "mount err=" << err;
  }
  ~Mount() {
    if (fs) aios_posix_unmount(fs);
  }
};

uint64_t mk(aios_posix_fs* fs, uint64_t parent, const char* name, bool dir = false) {
  aios_posix_stat st{};
  const int rc = dir ? aios_posix_mkdir(fs, parent, name, 0755, &st) : aios_posix_create(fs, parent, name, 0644, &st);
  EXPECT_EQ(rc, 0) << name;
  return st.ino;
}

void write_bytes(aios_posix_fs* fs, uint64_t ino, size_t n) {
  std::string buf(n, 'x');
  size_t out = 0;
  ASSERT_EQ(aios_posix_write(fs, ino, 0, buf.data(), buf.size(), &out), 0);
  ASSERT_EQ(aios_posix_fsync(fs, ino), 0);
}

std::multiset<Kind> kinds(const FsckReport& r) {
  std::multiset<Kind> out;
  for (const auto& f : r.findings) out.insert(f.kind);
  return out;
}

FsckReport run(aios::Session& s, const char* vol, bool repair, std::chrono::seconds min_age = std::chrono::seconds(0)) {
  FsckOptions opt;
  opt.repair = repair;
  opt.min_age = min_age;
  opt.log = [](const std::string& l) { std::fprintf(stderr, "  [fsck] %s\n", l.c_str()); };
  return aios::posix::fsck_volume(s, vol, opt);
}

}  // namespace

TEST(PosixFsck, CleanVolumeHasNoFindings) {
  HttpFixture http("aios-fsck-clean", 24100);
  {
    Mount m(http, "v");
    const uint64_t d = mk(m.fs, 1, "d", true);
    const uint64_t f = mk(m.fs, d, "f");
    write_bytes(m.fs, f, 10000);  // 3 chunks of 4096
    mk(m.fs, 1, "top");
    aios_posix_stat st{};
    ASSERT_EQ(aios_posix_symlink(m.fs, 1, "l", "d/f", &st), 0);
    ASSERT_EQ(aios_posix_link(m.fs, d, "f", 1, "hard"), 0);
  }
  aios::Session s(http.session_cfg());
  auto r = run(s, "v", false);
  EXPECT_TRUE(r.clean()) << r.findings.size() << " findings";
  EXPECT_EQ(r.dirs, 2u);
  EXPECT_EQ(r.files, 2u);  // f (two names) + top
  EXPECT_EQ(r.symlinks, 1u);
  EXPECT_EQ(r.bytes, 10000u);
  EXPECT_EQ(r.chunk_objects, 3u);
  EXPECT_EQ(r.inode_objects, 5u);  // root, d, f, top, l
}

TEST(PosixFsck, FindsAndRepairsEveryKindOfDamage) {
  using namespace aios::posix;
  HttpFixture http("aios-fsck-damage", 24120);
  uint64_t d = 0, f = 0, victim = 0, orphan_dir = 0, gone = 0;
  {
    Mount m(http, "v");
    d = mk(m.fs, 1, "d", true);
    f = mk(m.fs, d, "f");
    write_bytes(m.fs, f, 10000);
    victim = mk(m.fs, 1, "victim");   // its inode object will be deleted -> dangling dentry
    orphan_dir = mk(m.fs, 1, "od", true);  // its dentry will be removed -> orphan inode (+ dir objects)
    gone = mk(m.fs, 1, "gone");        // inode deleted but a chunk stays -> orphan chunk
    write_bytes(m.fs, gone, 100);
  }
  aios::Session s(http.session_cfg());

  // 1. dangling dentry: "victim"'s inode object disappears.
  s.delete_object(ino_oid("v", victim));
  // 2. orphan inode + stale dir objects: "od" loses its name in the root but
  //    the inode and dir/{od}/* stay. Also drops root nlink expectation by one.
  {
    DirTable root(s, "v", 1, nullptr);
    root.load(false);
    ASSERT_EQ(root.unlink_if("od", orphan_dir), 0);
  }
  // 3. orphan chunk: "gone"'s inode and dentry go, its chunk 0 stays.
  {
    DirTable root(s, "v", 1, nullptr);
    root.load(false);
    ASSERT_EQ(root.unlink_if("gone", gone), 0);
    s.delete_object(ino_oid("v", gone));
  }
  // 4. stray chunk: a chunk of f past its size.
  s.put_bytes(chunk_oid("v", f, 7), std::string(4096, 'z'));
  // 4b. stale dir objects: dir/777/* for an inode that does not exist (an
  //     interrupted rmdir deletes the inode first).
  s.put_bytes(dir_meta_oid("v", 777), R"({"next_op":1,"log_bytes":0})");
  s.put_bytes(dir_log_oid("v", 777), "");
  // 5. nlink mismatch on f (says 3, has one name) and parent mismatch on d.
  {
    auto snap = s.get_object(ino_oid("v", f));
    auto m = inode_from_json(snap.body, cas_from_attrs(snap.attrs));
    m.nlink = 3;
    s.put_bytes(ino_oid("v", f), inode_to_json(m), {}, m.cas);
    auto dsnap = s.get_object(ino_oid("v", d));
    auto dm = inode_from_json(dsnap.body, cas_from_attrs(dsnap.attrs));
    dm.parent_ino = 42;
    s.put_bytes(ino_oid("v", d), inode_to_json(dm), {}, dm.cas);
  }
  // 6. superblock allocator behind: an inode object with a huge number, reachable.
  {
    auto snap = s.get_object(ino_oid("v", f));
    auto m = inode_from_json(snap.body, cas_from_attrs(snap.attrs));
    m.ino = 1000000;
    m.nlink = 1;
    m.size = 0;
    s.put_bytes(ino_oid("v", 1000000), inode_to_json(m));
    DirTable root(s, "v", 1, nullptr);
    root.load(false);
    ASSERT_TRUE(root.link_if_absent("big", 1000000));
  }

  // --- check only -----------------------------------------------------------
  auto r = run(s, "v", false);
  ASSERT_FALSE(r.clean());
  EXPECT_FALSE(r.fatal);
  auto k = kinds(r);
  EXPECT_EQ(k.count(Kind::DanglingDentry), 1u);
  EXPECT_EQ(k.count(Kind::OrphanInode), 1u);
  EXPECT_EQ(k.count(Kind::StaleDirObjects), 1u);
  EXPECT_EQ(k.count(Kind::OrphanChunk), 1u);
  EXPECT_EQ(k.count(Kind::StrayChunk), 1u);
  EXPECT_EQ(k.count(Kind::ParentMismatch), 1u);
  EXPECT_EQ(k.count(Kind::NextInoBehind), 1u);
  // f says 3 with one name; root lost a subdir (od) so its nlink is one too high.
  EXPECT_EQ(k.count(Kind::NlinkMismatch), 2u);
  EXPECT_EQ(r.repaired, 0u);
  for (const auto& fnd : r.findings) EXPECT_FALSE(fnd.repaired);

  // --- the age guard leaves fresh objects alone ------------------------------
  auto young = run(s, "v", true, std::chrono::hours(1));
  EXPECT_EQ(young.repaired, 1u) << "only next_ino has no timestamp to respect";
  for (const auto& fnd : young.findings) {
    if (fnd.kind != Kind::NextInoBehind) {
      EXPECT_TRUE(fnd.skipped_young) << fsck_kind_name(fnd.kind);
    }
  }

  // --- repair -----------------------------------------------------------------
  auto rep = run(s, "v", true);
  EXPECT_EQ(rep.unrepaired(), 0u);
  for (const auto& fnd : rep.findings) {
    EXPECT_TRUE(fnd.repaired) << fsck_kind_name(fnd.kind) << " " << fnd.subject << " " << fnd.detail;
  }

  auto again = run(s, "v", false);
  EXPECT_TRUE(again.clean()) << again.findings.size() << " findings remain";
  EXPECT_FALSE(s.get_object(ino_oid("v", orphan_dir)).exists);
  EXPECT_FALSE(s.get_object(dir_meta_oid("v", 777)).exists);
  EXPECT_FALSE(s.get_object(dir_log_oid("v", 777)).exists);
  EXPECT_FALSE(s.get_object(chunk_oid("v", gone, 0)).exists);
  EXPECT_FALSE(s.get_object(chunk_oid("v", f, 7)).exists);
  EXPECT_TRUE(s.get_object(chunk_oid("v", f, 2)).exists) << "live chunks untouched";
  {
    auto snap = s.get_object(ino_oid("v", f));
    EXPECT_EQ(inode_from_json(snap.body, cas_from_attrs(snap.attrs)).nlink, 1u);
    auto dsnap = s.get_object(ino_oid("v", d));
    EXPECT_EQ(inode_from_json(dsnap.body, cas_from_attrs(dsnap.attrs)).parent_ino, 1u);
    auto sup = super_from_json(s.get_object(super_oid("v")).body, 0);
    EXPECT_GT(sup.next_ino, 1000000u);
  }

  // The repaired volume mounts and reads back.
  Mount m2(http, "v");
  aios_posix_stat st{};
  EXPECT_EQ(aios_posix_lookup(m2.fs, 1, "victim", &st), -ENOENT);
  EXPECT_EQ(aios_posix_lookup(m2.fs, 1, "big", &st), 0);
  EXPECT_EQ(aios_posix_lookup(m2.fs, d, "f", &st), 0);
  EXPECT_EQ(st.size, 10000u);
  EXPECT_EQ(aios_posix_getattr(m2.fs, 1, &st), 0);
  EXPECT_EQ(st.nlink, 3u) << "root: '.', '..', d";
  // The allocator moved past the planted inode number: a new file gets a fresh one.
  const uint64_t fresh = mk(m2.fs, 1, "fresh");
  EXPECT_GT(fresh, 1000000u);
}

TEST(PosixFsck, MissingSuperblockOrRootIsFatal) {
  HttpFixture http("aios-fsck-fatal", 24140);
  aios::Session s(http.session_cfg());
  auto r = run(s, "nothing-here", false);
  EXPECT_TRUE(r.fatal);
  ASSERT_EQ(r.findings.size(), 1u);
  EXPECT_EQ(r.findings[0].kind, Kind::NoSuperblock);

  { Mount m(http, "v"); }
  s.delete_object(aios::posix::ino_oid("v", 1));
  auto r2 = run(s, "v", true);
  EXPECT_TRUE(r2.fatal);
  ASSERT_EQ(r2.findings.size(), 1u);
  EXPECT_EQ(r2.findings[0].kind, Kind::NoRoot);
  EXPECT_FALSE(r2.findings[0].repaired);
}
