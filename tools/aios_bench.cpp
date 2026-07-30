#include "store/object_store.hpp"
#include "util/log.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct BenchArgs {
  std::string root;
  std::string mode{"both"};  // inline | fs | both
  std::uint32_t shards{16};
  std::size_t count{1000};
  std::size_t small_size{256};
  std::size_t large_size{256 * 1024};
  std::size_t inline_max{64 * 1024};
  bool keep{false};
};

void usage() {
  std::cout
      << "usage: aios-bench --root DIR [options]\n"
      << "\n"
      << "Benchmark AIOS hybrid object store (SQLite inline vs filesystem bodies).\n"
      << "\n"
      << "  --root DIR          working directory (created; contains aios/)\n"
      << "  --mode inline|fs|both   which path to measure (default both)\n"
      << "  --shards N          shard count, power of two (default 16)\n"
      << "  --count N           objects per mode (default 1000)\n"
      << "  --small-size N      inline object bytes (default 256)\n"
      << "  --large-size N      filesystem object bytes (default 262144)\n"
      << "  --inline-max N      store inline_max_bytes (default 65536)\n"
      << "  --keep              do not delete the bench directory\n";
}

bool parse_args(int argc, char** argv, BenchArgs& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      usage();
      std::exit(0);
    }
    if (arg == "--root") {
      const char* v = need("--root");
      if (!v) return false;
      a.root = v;
      continue;
    }
    if (arg == "--mode") {
      const char* v = need("--mode");
      if (!v) return false;
      a.mode = v;
      continue;
    }
    if (arg == "--shards") {
      const char* v = need("--shards");
      if (!v) return false;
      a.shards = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
      continue;
    }
    if (arg == "--count") {
      const char* v = need("--count");
      if (!v) return false;
      a.count = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
      continue;
    }
    if (arg == "--small-size") {
      const char* v = need("--small-size");
      if (!v) return false;
      a.small_size = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
      continue;
    }
    if (arg == "--large-size") {
      const char* v = need("--large-size");
      if (!v) return false;
      a.large_size = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
      continue;
    }
    if (arg == "--inline-max") {
      const char* v = need("--inline-max");
      if (!v) return false;
      a.inline_max = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
      continue;
    }
    if (arg == "--keep") {
      a.keep = true;
      continue;
    }
    std::cerr << "unknown arg: " << arg << "\n";
    return false;
  }
  if (a.root.empty()) {
    std::cerr << "--root is required\n";
    return false;
  }
  if (a.mode != "inline" && a.mode != "fs" && a.mode != "both") {
    std::cerr << "--mode must be inline|fs|both\n";
    return false;
  }
  return true;
}

struct Timing {
  double put_s{0};
  double get_s{0};
  double del_s{0};
  std::size_t ops{0};
  std::size_t bytes{0};
};

void print_timing(const char* label, const Timing& t) {
  const double put_ops = t.ops / t.put_s;
  const double get_ops = t.ops / t.get_s;
  const double put_mibs = (t.bytes / (1024.0 * 1024.0)) / t.put_s;
  const double get_mibs = (t.bytes / (1024.0 * 1024.0)) / t.get_s;
  std::cout << label << ":\n"
            << "  put  " << t.put_s << " s  " << put_ops << " ops/s  " << put_mibs
            << " MiB/s\n"
            << "  get  " << t.get_s << " s  " << get_ops << " ops/s  " << get_mibs
            << " MiB/s\n"
            << "  del  " << t.del_s << " s  " << (t.ops / t.del_s) << " ops/s\n";
}

Timing run_mode(aios::ObjectStore& store, const std::string& prefix, std::size_t count,
                std::size_t obj_size) {
  Timing t;
  t.ops = count;
  t.bytes = count * obj_size;

  std::vector<std::uint8_t> buf(obj_size);
  for (std::size_t i = 0; i < obj_size; ++i) {
    buf[i] = static_cast<std::uint8_t>(i & 0xff);
  }
  std::unordered_map<std::string, std::string> attrs{{"bench", "1"}, {"prefix", prefix}};

  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < count; ++i) {
    const std::string oid = prefix + "-" + std::to_string(i);
    // Vary payload slightly so FS/content isn't identical compress-wise.
    if (!buf.empty()) buf[0] = static_cast<std::uint8_t>(i & 0xff);
    if (!store.put(oid, buf.data(), buf.size(), attrs, true, err)) {
      throw std::runtime_error("put failed: " + err);
    }
  }
  const auto t1 = std::chrono::steady_clock::now();

  for (std::size_t i = 0; i < count; ++i) {
    const std::string oid = prefix + "-" + std::to_string(i);
    auto data = store.get(oid, err);
    if (!data || data->size() != obj_size) {
      throw std::runtime_error("get failed: " + err);
    }
  }
  const auto t2 = std::chrono::steady_clock::now();

  for (std::size_t i = 0; i < count; ++i) {
    const std::string oid = prefix + "-" + std::to_string(i);
    if (!store.del(oid, err)) {
      throw std::runtime_error("del failed: " + err);
    }
  }
  const auto t3 = std::chrono::steady_clock::now();

  using sec = std::chrono::duration<double>;
  t.put_s = sec(t1 - t0).count();
  t.get_s = sec(t2 - t1).count();
  t.del_s = sec(t3 - t2).count();
  return t;
}

}  // namespace

int main(int argc, char** argv) {
  BenchArgs args;
  if (!parse_args(argc, argv, args)) {
    usage();
    return 2;
  }

  const fs::path work = fs::path(args.root) / "aios";
  std::error_code ec;
  fs::remove_all(args.root, ec);
  fs::create_directories(work, ec);

  try {
    if (args.mode == "inline" || args.mode == "both") {
      aios::ObjectStoreOptions opts;
      opts.shard_count = args.shards;
      opts.inline_max_bytes = args.inline_max;
      opts.force_mode = "inline";

      aios::ObjectStore store;
      std::string err;
      // Separate subdir so force_mode/layout don't clash with fs run.
      const fs::path root = work / "bench-inline";
      fs::create_directories(root, ec);
      if (!store.open(root.string(), opts, err)) {
        std::cerr << "open inline store: " << err << "\n";
        return 1;
      }
      std::cout << "=== inline (SQLite BLOB) size=" << args.small_size
                << " count=" << args.count << " shards=" << args.shards << " ===\n";
      print_timing("inline", run_mode(store, "inl", args.count, args.small_size));
    }

    if (args.mode == "fs" || args.mode == "both") {
      aios::ObjectStoreOptions opts;
      opts.shard_count = args.shards;
      opts.inline_max_bytes = args.inline_max;
      opts.force_mode = "fs";

      aios::ObjectStore store;
      std::string err;
      const fs::path root = work / "bench-fs";
      fs::create_directories(root, ec);
      if (!store.open(root.string(), opts, err)) {
        std::cerr << "open fs store: " << err << "\n";
        return 1;
      }
      std::cout << "=== filesystem bodies size=" << args.large_size
                << " count=" << args.count << " shards=" << args.shards << " ===\n";
      print_timing("fs", run_mode(store, "fs", args.count, args.large_size));
    }
  } catch (const std::exception& e) {
    std::cerr << "bench error: " << e.what() << "\n";
    return 1;
  }

  if (!args.keep) {
    fs::remove_all(args.root, ec);
  } else {
    std::cout << "kept " << args.root << "\n";
  }
  return 0;
}
