#include "http/http_server.hpp"
#include "http/quota_admin.hpp"
#include "posix/aios_posix.h"
#include "posix/quota_ledger.hpp"
#include "test_helpers.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace {

using aios::test::DualStoreFixture;

int& failures() {
  static int n = 0;
  return n;
}
void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL quota: " << msg << "\n";
    ++failures();
  }
}

std::int64_t project_used(const nlohmann::json& show, std::uint32_t pid) {
  for (const auto& p : show.at("projects")) {
    if (p.value("id", 0u) == pid) return p.value("used_bytes", 0ll);
  }
  return -1;
}

}  // namespace

int test_quota() {
  using namespace aios;
  using namespace aios::posix;
  failures() = 0;

  {
    QuotaLimits lim;
    lim.volume_uids[1].bytes = 99;
    QuotaProjectLimits p;
    p.name = "x";
    p.root_ino = 7;
    p.bytes = 1000;
    lim.projects[2] = p;
    auto s = serialize_quota_limits(lim);
    auto back = parse_quota_limits(s, 3);
    expect(back.volume_uids[1].bytes && *back.volume_uids[1].bytes == 99, "codec uid");
    expect(back.projects[2].name == "x" && back.projects[2].root_ino == 7, "codec project");
  }

  {
    DualStoreFixture fx("aios-quota-unit");
    fx.cfg.http_listen = "127.0.0.1:" + std::to_string(19700 + (getpid() % 200));
    fx.cfg.admin = true;
    auto quota_admin = std::make_shared<QuotaAdminStore>(fx.cfg, *fx.svc);
    boost::asio::io_context ioc;
    auto work = boost::asio::make_work_guard(ioc);
    std::thread th([&] { ioc.run(); });
    HttpServer http(ioc, fx.cfg, *fx.svc, fx.membership, nullptr, quota_admin);
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    const std::string endpoint = fx.cfg.http_listen;
    aios_posix_config pcfg{};
    pcfg.endpoint = endpoint.c_str();
    pcfg.cluster_key = fx.cfg.cluster_key.c_str();
    pcfg.volume = "default";
    pcfg.uid = 1001;
    pcfg.gid = 100;
    int err = 0;
    auto* fs = aios_posix_mount(&pcfg, &err);
    expect(fs != nullptr, "mount");
    if (!fs) {
      work.reset();
      ioc.stop();
      th.join();
      return failures();
    }

    std::string qerr;
    size_t wrote = 0;

    // Volume uid soft limit
    expect(quota_admin->set_volume_uid_limit(1001, 100, qerr), "set uid limit 100 bytes");
    aios_posix_stat st{};
    expect(aios_posix_mkdir(fs, 1, "photos", 0755, &st) == 0, "mkdir photos");
    const auto photos_ino = st.ino;
    expect(aios_posix_mkdir(fs, 1, "other", 0755, &st) == 0, "mkdir other");
    const auto other_ino = st.ino;
    expect(aios_posix_create(fs, other_ino, "volf", 0644, &st) == 0, "create volf");
    const auto volf = st.ino;
    const std::string big(80, 'x');
    expect(aios_posix_write(fs, volf, 0, big.data(), big.size(), &wrote) == 0, "write under limit");
    const std::string more(50, 'y');
    expect(aios_posix_write(fs, volf, big.size(), more.data(), more.size(), &wrote) == -EDQUOT,
           "over limit EDQUOT");
    expect(quota_admin->set_volume_uid_limit(1001, 100000, qerr), "raise limit");
    aios_posix_unmount(fs);
    fs = aios_posix_mount(&pcfg, &err);
    expect(fs != nullptr, "remount after raise");
    expect(aios_posix_write(fs, volf, big.size(), more.data(), more.size(), &wrote) == 0,
           "write after raise");

    // Project on photos; outside writes do not charge project total
    std::uint32_t pid = 0;
    expect(quota_admin->create_project("photos", photos_ino, 50, pid, qerr), "create project");
    expect(pid > 0, "project id");
    aios_posix_unmount(fs);
    fs = aios_posix_mount(&pcfg, &err);
    expect(fs != nullptr, "remount after project");

    aios_posix_stat outside{};
    expect(aios_posix_create(fs, other_ino, "out", 0644, &outside) == 0, "create outside");
    const std::string out_chunk(40, 'o');
    expect(aios_posix_write(fs, outside.ino, 0, out_chunk.data(), out_chunk.size(), &wrote) == 0,
           "write outside project");

    aios_posix_stat pfile{};
    expect(aios_posix_create(fs, photos_ino, "pfile", 0644, &pfile) == 0, "create pfile");
    const std::string chunk(40, 'z');
    expect(aios_posix_write(fs, pfile.ino, 0, chunk.data(), chunk.size(), &wrote) == 0,
           "project write ok");
    expect(aios_posix_write(fs, pfile.ino, chunk.size(), chunk.data(), chunk.size(), &wrote) ==
               -EDQUOT,
           "project total EDQUOT");

    // Optional project-uid limit on a fresh project with headroom on total
    expect(quota_admin->delete_project(pid, qerr), "delete project before uid-cap test");
    expect(aios_posix_unlink(fs, photos_ino, "pfile") == 0, "unlink pfile");
    expect(quota_admin->create_project("photos", photos_ino, 1000, pid, qerr), "recreate project");
    expect(quota_admin->set_project_uid_limit(pid, 1001, 20, qerr), "project uid limit 20");
    aios_posix_unmount(fs);
    fs = aios_posix_mount(&pcfg, &err);
    aios_posix_stat puidf{};
    expect(aios_posix_create(fs, photos_ino, "puid", 0644, &puidf) == 0, "create puid");
    expect(aios_posix_write(fs, puidf.ino, 0, std::string(30, 'u').data(), 30, &wrote) == -EDQUOT,
           "project uid EDQUOT");
    expect(quota_admin->set_project_uid_limit(pid, 1001, std::nullopt, qerr), "clear project uid");
    aios_posix_unmount(fs);
    fs = aios_posix_mount(&pcfg, &err);
    expect(aios_posix_write(fs, puidf.ino, 0, chunk.data(), chunk.size(), &wrote) == 0,
           "write after clear uid limit");

    // Rename out of project → usage moves (verify via reconcile)
    expect(aios_posix_rename(fs, photos_ino, "puid", other_ino, "moved") == 0, "rename out");
    aios_posix_unmount(fs);
    expect(quota_admin->reconcile(qerr), "reconcile after rename");
    auto show = quota_admin->show();
    expect(show.contains("volume_uids"), "show has uids");
    expect(project_used(show, pid) == 0, "project usage zero after rename out");

    // Write under project again; reconcile matches size
    fs = aios_posix_mount(&pcfg, &err);
    aios_posix_stat p2{};
    expect(aios_posix_create(fs, photos_ino, "p2", 0644, &p2) == 0, "create p2");
    expect(aios_posix_write(fs, p2.ino, 0, chunk.data(), chunk.size(), &wrote) == 0, "p2 write");
    aios_posix_unmount(fs);
    expect(quota_admin->reconcile(qerr), "reconcile p2");
    show = quota_admin->show();
    expect(project_used(show, pid) == 40, "reconcile project usage matches");

    expect(quota_admin->delete_project(pid, qerr), "delete project");
    show = quota_admin->show();
    expect(show["projects"].empty(), "projects empty after delete");

    work.reset();
    ioc.stop();
    th.join();
  }

  if (failures() == 0) std::cout << "test_quota OK\n";
  return failures();
}
