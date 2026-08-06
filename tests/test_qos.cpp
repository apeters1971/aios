#include "http/http_server.hpp"
#include <gtest/gtest.h>
#include "http/qos_admin.hpp"
#include "posix/aios_posix.h"
#include "posix/qos_controller.hpp"
#include "test_helpers.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace {

using aios::test::DualStoreFixture;


}  // namespace

TEST(Qos, Basic) {
  using namespace aios;
  using namespace aios::posix;
  {
    QosLimits lim;
    lim.volume_uids[7].iops = 10;
    lim.volume_uids[7].bps = 1024;
    QosProjectLimits p;
    p.iops = 100;
    lim.projects[3] = p;
    auto s = serialize_qos_limits(lim);
    auto back = parse_qos_limits(s, 1);
    EXPECT_TRUE(back.volume_uids[7].iops && *back.volume_uids[7].iops == 10) << "codec iops";
    EXPECT_TRUE(back.volume_uids[7].bps && *back.volume_uids[7].bps == 1024) << "codec bps";
    EXPECT_TRUE(back.projects[3].iops && *back.projects[3].iops == 100) << "codec project";
  }

  {
    DualStoreFixture fx("aios-qos-unit");
    fx.cfg.http_listen = "127.0.0.1:" + std::to_string(19800 + (getpid() % 200));
    fx.cfg.admin = true;
    auto qos_admin = std::make_shared<QosAdminStore>(fx.cfg, *fx.svc);
    boost::asio::io_context ioc;
    auto work = boost::asio::make_work_guard(ioc);
    std::thread th([&] { ioc.run(); });
    HttpServer http(ioc, fx.cfg, *fx.svc, fx.membership, nullptr, nullptr, qos_admin);
    http.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    aios_posix_config pcfg{};
    pcfg.endpoint = fx.cfg.http_listen.c_str();
    pcfg.cluster_key = fx.cfg.cluster_key.c_str();
    pcfg.volume = "default";
    pcfg.uid = 1001;
    pcfg.gid = 100;
    int err = 0;
    auto* fs = aios_posix_mount(&pcfg, &err);
    EXPECT_TRUE(fs != nullptr) << "mount";
    if (!fs) {
      work.reset();
      ioc.stop();
      th.join();
      }

    std::string qerr;
    EXPECT_TRUE(qos_admin->set_volume_uid(1001, 2, std::nullopt, false, qerr)) << "set iops=2";

    aios_posix_stat st{};
    EXPECT_TRUE(aios_posix_create(fs, 1, "f", 0644, &st) == 0) << "create";
    const auto fino = st.ino;
    const std::string chunk(16, 'x');
    size_t wrote = 0;
    EXPECT_TRUE(aios_posix_write(fs, fino, 0, chunk.data(), chunk.size(), &wrote) == 0) << "write1";
    EXPECT_TRUE(aios_posix_write(fs, fino, chunk.size(), chunk.data(), chunk.size(), &wrote) == 0) << "write2";
    // Burst equals rate (2); third immediate write should throttle.
    EXPECT_TRUE(aios_posix_write(fs, fino, 2 * chunk.size(), chunk.data(), chunk.size(), &wrote) ==
               -EAGAIN) << "iops EAGAIN";

    EXPECT_TRUE(qos_admin->set_volume_uid(1001, std::nullopt, std::nullopt, true, qerr)) << "clear iops";
    aios_posix_unmount(fs);
    fs = aios_posix_mount(&pcfg, &err);
    EXPECT_TRUE(aios_posix_write(fs, fino, 2 * chunk.size(), chunk.data(), chunk.size(), &wrote) == 0) << "write after clear";

    // Bandwidth limit: 20 bytes/s, burst 20 — 40-byte write denied.
    EXPECT_TRUE(qos_admin->set_volume_uid(1001, std::nullopt, 20, false, qerr)) << "set bps=20";
    aios_posix_unmount(fs);
    fs = aios_posix_mount(&pcfg, &err);
    const std::string big(40, 'y');
    EXPECT_TRUE(aios_posix_write(fs, fino, 0, big.data(), big.size(), &wrote) == -EAGAIN) << "bps EAGAIN";
    const std::string small(10, 'z');
    EXPECT_TRUE(aios_posix_write(fs, fino, 0, small.data(), small.size(), &wrote) == 0) << "bps ok";

    auto show = qos_admin->show();
    EXPECT_TRUE(show.contains("monitoring")) << "show monitoring";
    EXPECT_TRUE(show.contains("volume_uids")) << "show uids";
    EXPECT_TRUE(show["monitoring"].contains("node")) << "node rates";

    aios_posix_unmount(fs);
    work.reset();
    ioc.stop();
    th.join();
  }

  }
