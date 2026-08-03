#include "fs/aios_scan.hpp"
#include "util/uid.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
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
    expect(m.target_paths.size() == 1 && m.target_paths[0] == "/mnt/data", "default mount root");
  }
  {
    expect(parse_aios_marker(
               "storage_class: hdd\nweight: 2\ntargets:\n  - data\n  - scratch\n", "/mnt", m,
               err),
           "full marker ok");
    expect(m.storage_class == "hdd", "hdd class");
    expect(m.weight == 2, "weight 2");
    expect(m.target_paths.size() == 2, "two targets");
    expect(m.target_paths[0] == "/mnt/data", "data path");
    expect(m.target_paths[1] == "/mnt/scratch", "scratch path");
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
    expect(fs::is_directory(base / "data" / "aios"), "aios dir created");
    expect(t.bsize > 0, "statvfs bsize");
  }

  fs::remove_all(base);
  return failures;
}
