#include "store/object_store.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

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

int test_store_range() {
  using namespace aios;
  const auto root = fs::temp_directory_path() / ("aios-range-" + std::to_string(::getpid()));
  fs::create_directories(root);
  ObjectStore store;
  ObjectStoreOptions opts;
  opts.shard_count = 4;
  opts.inline_max_bytes = 64;
  std::string err;
  expect(store.open(root.string(), opts, err), "open");

  // Full small put then ranged overwrite beyond inline → promote.
  expect(store.put("o1", std::string("abcd"), {}, true, err), "put small");
  const std::string chunk(100, 'Z');
  expect(store.put_range("o1", 50, reinterpret_cast<const std::uint8_t*>(chunk.data()),
                         chunk.size(), {{"k", "v"}}, false, err),
         "put_range grow");
  auto st = store.stat("o1", err);
  expect(st.has_value(), "stat");
  expect(st->size == 150, "size 150");
  expect(!st->inline_body, "fs backed");

  auto mid = store.get_range("o1", 50, 100, err);
  expect(mid.has_value() && mid->size() == 100, "get_range len");
  expect(std::string(mid->begin(), mid->end()) == chunk, "get_range data");

  auto head = store.get_range("o1", 0, 4, err);
  expect(head.has_value(), "head range");
  // First 4 bytes from original inline promote.
  expect(std::string(head->begin(), head->end()) == "abcd", "preserved prefix");

  // Preconditions
  std::vector<AttrPrecondition> ok_eq = {
      {AttrPrecondition::Kind::Eq, "k", "v"},
  };
  expect(store.check_preconditions("o1", ok_eq, err) == PrecondResult::Ok, "pred eq");
  std::vector<AttrPrecondition> bad = {
      {AttrPrecondition::Kind::Eq, "k", "nope"},
  };
  expect(store.check_preconditions("o1", bad, err) == PrecondResult::Conflict, "pred conflict");
  std::vector<AttrPrecondition> create_only = {
      {AttrPrecondition::Kind::MustNotExist, {}, {}},
  };
  expect(store.check_preconditions("o1", create_only, err) == PrecondResult::Conflict,
         "must not exist");

  // LIST
  expect(store.put("pref/a", std::string("1"), {}, true, err), "put a");
  expect(store.put("pref/b", std::string("2"), {}, true, err), "put b");
  expect(store.put("other", std::string("3"), {}, true, err), "put other");
  auto lst = store.list("pref/", "", "", 10, "", false, err);
  expect(lst.objects.size() == 2, "list prefix count");

  // Unsatisfiable range
  auto bad_r = store.get_range("o1", 1000, 10, err);
  expect(!bad_r.has_value(), "unsat range");

  fs::remove_all(root);
  return failures;
}
