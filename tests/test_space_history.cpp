#include "test_helpers.hpp"

#include "http/space_history.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using namespace aios;

namespace {

FsEntry disk(const std::string& node, const std::string& mount, const std::string& cls,
             std::uint64_t blocks, std::uint64_t bavail, bool usable = true) {
  FsEntry e;
  e.node_id = node;
  e.mount = mount;
  e.aios_path = mount + "/aios";
  e.storage_class = cls;
  e.bsize = 4096;
  e.blocks = blocks;
  e.bavail = bavail;
  e.usable = usable;
  e.state = usable ? LifecycleState::Up : LifecycleState::Off;
  return e;
}

}  // namespace

TEST(SpaceHistory, SnapshotAndPersist) {
  const auto root = aios::test::temp_root("aios-space-hist");
  const auto path = (root / "aios-space.json").string();
  SpaceHistory hist(path);

  const auto e1 = disk("n1", "/d0", "nvme", 1000, 400);
  const auto e2 = disk("n2", "/d1", "hdd", 2000, 500);
  const auto off = disk("n2", "/d2", "hdd", 3000, 100, false);
  hist.maybe_record({e1, e2, off}, "n1");

  const auto j = hist.to_json();
  ASSERT_TRUE(j.contains("totals"));
  EXPECT_EQ(j["totals"]["total_bytes"].get<std::uint64_t>(), 4096ull * 3000);
  EXPECT_EQ(j["totals"]["avail_bytes"].get<std::uint64_t>(), 4096ull * 900);
  EXPECT_EQ(j["totals"]["used_bytes"].get<std::uint64_t>(), 4096ull * 2100);
  EXPECT_EQ(j["disks"].size(), 3u);
  EXPECT_EQ(j["by_host"].size(), 2u);
  EXPECT_EQ(j["by_class"].size(), 2u);
  ASSERT_FALSE(j["history"]["recent"].empty());
  ASSERT_FALSE(j["history"]["hourly"].empty());
  ASSERT_FALSE(j["history"]["daily"].empty());

  const auto n_recent = j["history"]["recent"].size();
  hist.maybe_record({e1, e2, off}, "n1");
  EXPECT_EQ(hist.to_json()["history"]["recent"].size(), n_recent);

  SpaceHistory loaded(path);
  const auto j2 = loaded.to_json();
  EXPECT_EQ(j2["history"]["recent"].size(), n_recent);
  EXPECT_EQ(j2["history"]["daily"].size(), 1u);

  std::filesystem::remove_all(root);
}
