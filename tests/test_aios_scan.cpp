#include "cluster/weight.hpp"
#include "fs/aios_scan.hpp"
#include "util/uid.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
}

}  // namespace

int test_aios_scan() {
  using namespace aios;

  std::string err;
  AiosMarker m;
  {
    expect(!parse_aios_marker("", "/mnt/data", m, err), "empty requires storage_class");
    expect(!err.empty(), "empty err set");
  }
  {
    expect(!parse_aios_marker("{}", "/mnt/data", m, err), "{} requires storage_class");
  }
  {
    expect(parse_aios_marker("storage_class: nvme\n", "/mnt/data", m, err), "class only ok");
    expect(err.empty(), "class only no err");
    expect(m.storage_class == "nvme", "class nvme");
    expect(!m.weight_specified, "weight omitted");
    expect(m.target_paths.size() == 1 && m.target_paths[0] == "/mnt/data", "default mount root");
  }
  {
    expect(bytes_to_weight(0) == 1, "zero bytes -> weight 1");
    expect(bytes_to_weight(kWeightByteUnit) == 1, "1 TiB -> 1");
    expect(bytes_to_weight(4 * kWeightByteUnit) == 4, "4 TiB -> 4");
    expect(bytes_to_weight(4 * kWeightByteUnit + kWeightByteUnit / 2) == 5, "4.5 TiB rounds up");
    expect(weight_autotune_delta_needed(10, 20, 1) == 2, "20% of 10 => 2");
    expect(weight_autotune_delta_needed(5, 20, 1) == 1, "floor min_delta");
    expect(!weight_autotune_should_update(10, 11, 20, 1), "11 within hysteresis");
    expect(weight_autotune_should_update(10, 12, 20, 1), "12 crosses 20%");
  }
  {
    expect(parse_aios_marker(
               "storage_class: hdd\nweight: 2\ntargets:\n  - data\n  - scratch\n", "/mnt", m,
               err),
           "full marker ok");
    expect(m.storage_class == "hdd", "hdd class");
    expect(m.weight == 2, "weight 2");
    expect(m.weight_specified, "weight specified");
    expect(m.state == LifecycleState::Up, "default state up");
    expect(m.target_paths.size() == 2, "two targets");
    expect(m.target_paths[0] == "/mnt/data", "data path");
    expect(m.target_paths[1] == "/mnt/scratch", "scratch path");
  }
  {
    expect(parse_aios_marker("storage_class: nvme\nstate: drain\nweight: 4\n", "/mnt", m, err),
           "state drain ok");
    expect(m.state == LifecycleState::Drain, "parsed drain");
    expect(m.weight == 4, "weight 4");
  }
  {
    expect(parse_aios_marker("storage_class: nvme\nrack: row-a\n", "/mnt", m, err), "rack ok");
    expect(m.rack_specified && m.rack == "row-a", "parsed rack");
  }
  {
    expect(!parse_aios_marker("storage_class: nvme\nstate: broken\n", "/mnt", m, err),
           "bad state rejected");
  }
  {
    expect(!parse_aios_marker("storage_class: nvme\ntargets:\n  - /elsewhere\n", "/mnt", m, err),
           "escape rejected");
  }

  const auto base = fs::temp_directory_path() / "aios-test-scan";
  fs::remove_all(base);
  fs::create_directories(base / "data");
  {
    auto t = prepare_target(base.string(), (base / "data").string(), "nvme", 1);
    expect(t.usable, "prepare usable");
    expect(t.storage_class == "nvme", "prepare class");
    expect(t.weight_explicit, "explicit weight");
    expect(t.weight == 1, "weight 1");
    expect(fs::is_directory(base / "data" / "aios"), "aios dir created");
    expect(t.bsize > 0, "statvfs bsize");
  }
  {
    auto t = prepare_target(base.string(), (base / "data").string(), "nvme", std::nullopt);
    expect(t.usable, "prepare capacity weight usable");
    expect(!t.weight_explicit, "capacity-derived");
    expect(t.weight >= 1, "capacity weight >= 1");
    const auto expect_w = bytes_to_weight(t.bsize * t.blocks);
    expect(t.weight == expect_w, "weight matches total capacity TiB");
  }
  {
    const auto marker = base / ".aios";
    {
      std::ofstream out(marker);
      out << "storage_class: nvme\nweight: 1\n";
    }
    expect(update_aios_marker_file(marker.string(), std::string("drain"), 8, err),
           "update marker");
    expect(parse_aios_marker(
               [&] {
                 std::ifstream in(marker);
                 return std::string(std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>());
               }(),
               base.string(), m, err),
           "reparse after update");
    expect(m.state == LifecycleState::Drain, "updated state");
    expect(m.weight == 8, "updated weight");
  }

  fs::remove_all(base);
  return failures;
}
