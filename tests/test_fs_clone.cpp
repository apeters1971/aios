#include "test_helpers.hpp"

#include "store/fs_clone.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

int test_fs_clone() {
  using namespace aios;
  using aios::test::expect;
  using aios::test::failures;
  using aios::test::temp_root;

  failures() = 0;

  expect(clone_file_supported(), "clone_file_supported");

  const auto root = temp_root("aios-fs-clone");
  const auto src = (root / "src.bin").string();
  const auto dst = (root / "dst.bin").string();
  const auto dst2 = (root / "dst2.bin").string();
  const auto copy_dst = (root / "copy.bin").string();

  {
    std::ofstream out(src, std::ios::binary);
    out << "clone-payload-bytes";
  }
  {
    std::ofstream out(dst2, std::ios::binary);
    out << "existing";
  }

  std::string err;
  expect(clone_or_copy_file(src, dst, /*allow_copy=*/true, err), "clone_or_copy allow_copy");
  expect(fs::exists(dst), "dst exists");
  {
    std::ifstream in(dst, std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), {});
    expect(got == "clone-payload-bytes", "cloned/copied content");
  }

  err.clear();
  expect(!clone_file(src, dst2, err), "clone to existing dest fails");
  expect(!err.empty(), "clone existing err set");

  err.clear();
  expect(copy_file_full(src, copy_dst, err), "copy_file_full works");
  expect(fs::exists(copy_dst), "copy dest exists");
  {
    std::ifstream in(copy_dst, std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), {});
    expect(got == "clone-payload-bytes", "copied content");
  }

  std::error_code ec;
  fs::remove_all(root, ec);
  return failures();
}
