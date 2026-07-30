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
  {
    auto t = parse_aios_targets("", "/mnt/data", err);
    expect(err.empty(), "empty yaml no err");
    expect(t.size() == 1 && t[0] == "/mnt/data", "empty => mount root");
  }
  {
    auto t = parse_aios_targets("{}", "/mnt/data", err);
    expect(err.empty(), "{} no err");
    expect(t.size() == 1 && t[0] == "/mnt/data", "{} => mount root");
  }
  {
    auto t = parse_aios_targets("targets:\n  - data\n  - scratch\n", "/mnt", err);
    expect(err.empty(), "targets no err");
    expect(t.size() == 2, "two targets");
    expect(t[0] == "/mnt/data", "data path");
    expect(t[1] == "/mnt/scratch", "scratch path");
  }
  {
    auto t = parse_aios_targets("targets:\n  - /elsewhere\n", "/mnt", err);
    expect(!err.empty() || t.empty(), "escape rejected");
  }

  // Fixture dir with prepare_target
  const auto base = fs::temp_directory_path() / "aios-test-scan";
  fs::remove_all(base);
  fs::create_directories(base / "data");
  {
    auto t = prepare_target(base.string(), (base / "data").string());
    expect(t.usable, "prepare usable");
    expect(fs::is_directory(base / "data" / "aios"), "aios dir created");
    expect(t.bsize > 0, "statvfs bsize");
  }

  // Ownership mismatch simulation is hard without root; skip.

  fs::remove_all(base);
  return failures;
}
