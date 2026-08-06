#include "cluster/weight.hpp"
#include <gtest/gtest.h>
#include "fs/aios_scan.hpp"
#include "util/uid.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

namespace fs = std::filesystem;


TEST(AiosScan, Basic) {
  using namespace aios;

  std::string err;
  AiosMarker m;
  {
    EXPECT_TRUE(!parse_aios_marker("", "/mnt/data", m, err)) << "empty requires storage_class";
    EXPECT_TRUE(!err.empty()) << "empty err set";
  }
  {
    EXPECT_TRUE(!parse_aios_marker("{}", "/mnt/data", m, err)) << "{} requires storage_class";
  }
  {
    EXPECT_TRUE(parse_aios_marker("storage_class: nvme\n", "/mnt/data", m, err)) << "class only ok";
    EXPECT_TRUE(err.empty()) << "class only no err";
    EXPECT_TRUE(m.storage_class == "nvme") << "class nvme";
    EXPECT_TRUE(!m.weight_specified) << "weight omitted";
    EXPECT_TRUE(m.target_paths.size() == 1 && m.target_paths[0] == "/mnt/data") << "default mount root";
  }
  {
    EXPECT_TRUE(bytes_to_weight(0) == 1) << "zero bytes -> weight 1";
    EXPECT_TRUE(bytes_to_weight(kWeightByteUnit) == 1) << "1 TiB -> 1";
    EXPECT_TRUE(bytes_to_weight(4 * kWeightByteUnit) == 4) << "4 TiB -> 4";
    EXPECT_TRUE(bytes_to_weight(4 * kWeightByteUnit + kWeightByteUnit / 2) == 5) << "4.5 TiB rounds up";
    EXPECT_TRUE(weight_autotune_delta_needed(10, 20, 1) == 2) << "20% of 10 => 2";
    EXPECT_TRUE(weight_autotune_delta_needed(5, 20, 1) == 1) << "floor min_delta";
    EXPECT_TRUE(!weight_autotune_should_update(10, 11, 20, 1)) << "11 within hysteresis";
    EXPECT_TRUE(weight_autotune_should_update(10, 12, 20, 1)) << "12 crosses 20%";
  }
  {
    EXPECT_TRUE(parse_aios_marker(
               "storage_class: hdd\nweight: 2\ntargets:\n  - data\n  - scratch\n", "/mnt", m,
               err)) << "full marker ok";
    EXPECT_TRUE(m.storage_class == "hdd") << "hdd class";
    EXPECT_TRUE(m.weight == 2) << "weight 2";
    EXPECT_TRUE(m.weight_specified) << "weight specified";
    EXPECT_TRUE(m.state == LifecycleState::Up) << "default state up";
    EXPECT_TRUE(m.target_paths.size() == 2) << "two targets";
    EXPECT_TRUE(m.target_paths[0] == "/mnt/data") << "data path";
    EXPECT_TRUE(m.target_paths[1] == "/mnt/scratch") << "scratch path";
  }
  {
    EXPECT_TRUE(parse_aios_marker("storage_class: nvme\nstate: drain\nweight: 4\n", "/mnt", m, err)) << "state drain ok";
    EXPECT_TRUE(m.state == LifecycleState::Drain) << "parsed drain";
    EXPECT_TRUE(m.weight == 4) << "weight 4";
  }
  {
    EXPECT_TRUE(parse_aios_marker("storage_class: nvme\nrack: row-a\n", "/mnt", m, err)) << "rack ok";
    EXPECT_TRUE(m.rack_specified && m.rack == "row-a") << "parsed rack";
  }
  {
    EXPECT_TRUE(!parse_aios_marker("storage_class: nvme\nstate: broken\n", "/mnt", m, err)) << "bad state rejected";
  }
  {
    EXPECT_TRUE(!parse_aios_marker("storage_class: nvme\ntargets:\n  - /elsewhere\n", "/mnt", m, err)) << "escape rejected";
  }

  const auto base = fs::temp_directory_path() / "aios-test-scan";
  fs::remove_all(base);
  fs::create_directories(base / "data");
  {
    auto t = prepare_target(base.string(), (base / "data").string(), "nvme", 1);
    EXPECT_TRUE(t.usable) << "prepare usable";
    EXPECT_TRUE(t.storage_class == "nvme") << "prepare class";
    EXPECT_TRUE(t.weight_explicit) << "explicit weight";
    EXPECT_TRUE(t.weight == 1) << "weight 1";
    EXPECT_TRUE(fs::is_directory(base / "data" / "aios")) << "aios dir created";
    EXPECT_TRUE(t.bsize > 0) << "statvfs bsize";
  }
  {
    auto t = prepare_target(base.string(), (base / "data").string(), "nvme", std::nullopt);
    EXPECT_TRUE(t.usable) << "prepare capacity weight usable";
    EXPECT_TRUE(!t.weight_explicit) << "capacity-derived";
    EXPECT_TRUE(t.weight >= 1) << "capacity weight >= 1";
    const auto expect_w = bytes_to_weight(t.bsize * t.blocks);
    EXPECT_TRUE(t.weight == expect_w) << "weight matches total capacity TiB";
  }
  {
    const auto marker = base / ".aios";
    {
      std::ofstream out(marker);
      out << "storage_class: nvme\nweight: 1\n";
    }
    EXPECT_TRUE(update_aios_marker_file(marker.string(), std::string("drain"), 8, err)) << "update marker";
    EXPECT_TRUE(parse_aios_marker(
               [&] {
                 std::ifstream in(marker);
                 return std::string(std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>());
               }(),
               base.string(), m, err)) << "reparse after update";
    EXPECT_TRUE(m.state == LifecycleState::Drain) << "updated state";
    EXPECT_TRUE(m.weight == 8) << "updated weight";
  }

  fs::remove_all(base);
  }
