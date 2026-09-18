// aios-store-bench: microbenchmark of the local ObjectStore engine alone — no
// daemon, no HTTP, no replication. Measures what a primary gets from one store
// on one disk: full-object put/get/del, random range writes and reads inside
// existing objects, and appends to per-thread logs.
#include "store/object_store.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct BenchArgs {
  std::string root;
  std::string mode{"both"};  // inline | fs | seg | both | all
  std::string op{"put"};     // put | range | append | all
  std::uint32_t shards{16};
  std::size_t count{1000};
  std::size_t small_size{256};
  std::size_t large_size{256 * 1024};
  std::size_t io_size{4096};
  std::size_t inline_max{64 * 1024};
  unsigned threads{1};
  bool fsync{true};
  bool keep{false};
  bool with_stat{true};
  std::vector<std::size_t> sizes;  // if set, run put at each size (stat+get)
};

void usage() {
  std::cout
      << "usage: aios-store-bench --root DIR [options]\n"
      << "\n"
      << "Benchmark the local AIOS object store engine (no daemon, no network, no\n"
      << "replication): SQLite inline BLOBs vs one-file-per-version vs packed segments.\n"
      << "\n"
      << "  --root DIR          working directory (created; contains aios/)\n"
      << "  --mode inline|fs|seg|both|all\n"
      << "                      body path to measure (default both = inline+fs).\n"
      << "                      all = inline+fs+seg. seg = append-only segment files.\n"
      << "  --op put|range|append|all\n"
      << "                      put:    full-object put, stat, get, del (default)\n"
      << "                      range:  random --io-size writes then reads inside\n"
      << "                              existing objects of the mode's size\n"
      << "                      append: --io-size appends to one log per thread\n"
      << "  --threads N         concurrent workers, each on its own objects (default 1)\n"
      << "  --shards N          shard count, power of two (default 16)\n"
      << "  --count N           ops per phase per mode, split across threads (default 1000)\n"
      << "  --small-size N      inline/seg object bytes (default 256)\n"
      << "  --large-size N      filesystem object bytes (default 262144)\n"
      << "  --sizes N,N,…       extra put+stat+get sizes (e.g. 256,4k,64k,1M); implies --op put\n"
      << "  --io-size N         range / append I/O bytes (default 4096)\n"
      << "  --inline-max N      store inline_max_bytes (default 65536)\n"
      << "  --no-stat           skip the STAT phase on --op put\n"
      << "  --no-fsync          skip body/dir fsync, SQLite synchronous=OFF (engine cost only)\n"
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
    auto num = [&](const char* name, std::size_t& out) {
      const char* v = need(name);
      if (!v) return false;
      out = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
      return true;
    };
    auto parse_size = [](std::string s, std::size_t& out) -> bool {
      if (s.empty()) return false;
      std::uint64_t mul = 1;
      const char last = static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
      if (last == 'k') {
        mul = 1024;
        s.pop_back();
      } else if (last == 'm') {
        mul = 1024ull * 1024ull;
        s.pop_back();
      }
      if (s.empty()) return false;
      char* end = nullptr;
      const auto n = std::strtoull(s.c_str(), &end, 10);
      if (end == s.c_str() || *end != '\0') return false;
      out = static_cast<std::size_t>(n * mul);
      return true;
    };
    if (arg == "--help" || arg == "-h") {
      usage();
      std::exit(0);
    }
    if (arg == "--root") {
      const char* v = need("--root");
      if (!v) return false;
      a.root = v;
    } else if (arg == "--mode") {
      const char* v = need("--mode");
      if (!v) return false;
      a.mode = v;
    } else if (arg == "--op") {
      const char* v = need("--op");
      if (!v) return false;
      a.op = v;
    } else if (arg == "--shards") {
      std::size_t n = 0;
      if (!num("--shards", n)) return false;
      a.shards = static_cast<std::uint32_t>(n);
    } else if (arg == "--threads") {
      std::size_t n = 0;
      if (!num("--threads", n)) return false;
      a.threads = static_cast<unsigned>(std::max<std::size_t>(1, n));
    } else if (arg == "--count") {
      if (!num("--count", a.count)) return false;
    } else if (arg == "--small-size") {
      if (!num("--small-size", a.small_size)) return false;
    } else if (arg == "--large-size") {
      if (!num("--large-size", a.large_size)) return false;
    } else if (arg == "--io-size") {
      if (!num("--io-size", a.io_size)) return false;
    } else if (arg == "--inline-max") {
      if (!num("--inline-max", a.inline_max)) return false;
    } else if (arg == "--sizes") {
      const char* v = need("--sizes");
      if (!v) return false;
      std::string rest = v;
      std::size_t pos = 0;
      while (pos < rest.size()) {
        const auto comma = rest.find(',', pos);
        const auto tok = rest.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        std::size_t n = 0;
        if (!parse_size(tok, n) || n == 0) {
          std::cerr << "bad --sizes token: " << tok << "\n";
          return false;
        }
        a.sizes.push_back(n);
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    } else if (arg == "--no-stat") {
      a.with_stat = false;
    } else if (arg == "--no-fsync") {
      a.fsync = false;
    } else if (arg == "--keep") {
      a.keep = true;
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return false;
    }
  }
  if (a.root.empty()) {
    std::cerr << "--root is required\n";
    return false;
  }
  if (a.mode != "inline" && a.mode != "fs" && a.mode != "seg" && a.mode != "both" &&
      a.mode != "all") {
    std::cerr << "--mode must be inline|fs|seg|both|all\n";
    return false;
  }
  if (a.op != "put" && a.op != "range" && a.op != "append" && a.op != "all") {
    std::cerr << "--op must be put|range|append|all\n";
    return false;
  }
  if (a.count == 0 || a.io_size == 0) {
    std::cerr << "--count and --io-size must be > 0\n";
    return false;
  }
  return true;
}

// One measured phase: N ops executed by T threads, each op timed individually.
struct Phase {
  std::string name;
  std::size_t ops{0};
  std::size_t bytes{0};
  double wall_s{0};
  std::vector<double> lat_ms;  // per-op

  double pct(double p) const {
    if (lat_ms.empty()) return 0;
    std::vector<double> v = lat_ms;
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(p / 100.0 * (v.size() - 1) + 0.5);
    return v[std::min(idx, v.size() - 1)];
  }
};

void print_phase(const Phase& p) {
  const double ops_s = p.wall_s > 0 ? p.ops / p.wall_s : 0;
  const double mib_s = p.wall_s > 0 ? (p.bytes / (1024.0 * 1024.0)) / p.wall_s : 0;
  std::printf("  %-7s %9zu ops %8.3f s %10.0f ops/s %9.2f MiB/s  p50 %7.3f  p99 %7.3f ms\n",
              p.name.c_str(), p.ops, p.wall_s, ops_s, mib_s, p.pct(50), p.pct(99));
}

// Runs fn(thread_index, op_index) for op_index in [0, count) spread over
// `threads` workers; op_index is globally unique so oids never collide.
template <typename Fn>
Phase run_phase(const std::string& name, unsigned threads, std::size_t count,
                std::size_t bytes_per_op, Fn&& fn) {
  Phase p;
  p.name = name;
  p.ops = count;
  p.bytes = count * bytes_per_op;
  std::vector<std::vector<double>> lats(threads);
  std::atomic<std::size_t> next{0};
  std::mutex err_mu;
  std::string first_err;

  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (unsigned t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      auto& my = lats[t];
      my.reserve(count / threads + 1);
      for (;;) {
        const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= count) break;
        const auto a = std::chrono::steady_clock::now();
        std::string err;
        if (!fn(t, i, err)) {
          std::lock_guard<std::mutex> lk(err_mu);
          if (first_err.empty()) first_err = name + " op " + std::to_string(i) + ": " + err;
          break;
        }
        const auto b = std::chrono::steady_clock::now();
        my.push_back(std::chrono::duration<double, std::milli>(b - a).count());
      }
    });
  }
  for (auto& w : workers) w.join();
  p.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!first_err.empty()) throw std::runtime_error(first_err);
  for (auto& v : lats) p.lat_ms.insert(p.lat_ms.end(), v.begin(), v.end());
  return p;
}

std::vector<std::uint8_t> pattern(std::size_t n, std::uint8_t seed) {
  std::vector<std::uint8_t> buf(n);
  for (std::size_t i = 0; i < n; ++i) buf[i] = static_cast<std::uint8_t>((i + seed) & 0xff);
  return buf;
}

// put: full-object put / get / del over `count` distinct objects.
void run_put(aios::ObjectStore& store, const BenchArgs& a, const std::string& prefix,
             std::size_t obj_size) {
  const std::unordered_map<std::string, std::string> attrs{{"bench", "1"}};
  auto oid = [&](std::size_t i) { return prefix + "-" + std::to_string(i); };
  // One buffer per thread so workers do not share a cache line on byte 0.
  std::vector<std::vector<std::uint8_t>> bufs;
  for (unsigned t = 0; t < a.threads; ++t) bufs.push_back(pattern(obj_size, t));

  print_phase(run_phase("put", a.threads, a.count, obj_size,
                        [&](unsigned t, std::size_t i, std::string& err) {
                          auto& b = bufs[t];
                          if (!b.empty()) b[0] = static_cast<std::uint8_t>(i & 0xff);
                          return store.put(oid(i), b.data(), b.size(), attrs, true, err);
                        }));
  if (a.with_stat) {
    print_phase(run_phase("stat", a.threads, a.count, 0,
                          [&](unsigned, std::size_t i, std::string& err) {
                            auto st = store.stat(oid(i), err);
                            if (!st || st->size != obj_size) {
                              if (err.empty()) err = "stat failed";
                              return false;
                            }
                            return true;
                          }));
  }
  print_phase(run_phase("get", a.threads, a.count, obj_size,
                        [&](unsigned, std::size_t i, std::string& err) {
                          auto d = store.get(oid(i), err);
                          if (!d || d->size() != obj_size) {
                            if (err.empty()) err = "short read";
                            return false;
                          }
                          return true;
                        }));
  print_phase(run_phase("del", a.threads, a.count, 0,
                        [&](unsigned, std::size_t i, std::string& err) {
                          return store.del(oid(i), err);
                        }));
}

// range: `threads` objects of obj_size (one per worker, created up front), then
// `count` random-offset writes of io_size, then `count` random range reads.
void run_range(aios::ObjectStore& store, const BenchArgs& a, const std::string& prefix,
               std::size_t obj_size) {
  const std::unordered_map<std::string, std::string> attrs{{"bench", "1"}};
  const std::size_t io = std::min(a.io_size, obj_size);
  auto oid = [&](unsigned t) { return prefix + "-rng-" + std::to_string(t); };
  {
    auto base = pattern(obj_size, 7);
    std::string err;
    for (unsigned t = 0; t < a.threads; ++t) {
      if (!store.put(oid(t), base.data(), base.size(), attrs, true, err)) {
        throw std::runtime_error("range setup put: " + err);
      }
    }
  }
  const std::uint64_t span = obj_size - io;  // highest valid offset
  std::vector<std::vector<std::uint8_t>> bufs;
  std::vector<std::mt19937_64> rngs;
  for (unsigned t = 0; t < a.threads; ++t) {
    bufs.push_back(pattern(io, static_cast<std::uint8_t>(t + 1)));
    rngs.emplace_back(0x5eed0000u + t);
  }
  auto off_for = [&](unsigned t) -> std::uint64_t {
    return span == 0 ? 0 : (rngs[t]() % (span + 1));
  };

  print_phase(run_phase("rwrite", a.threads, a.count, io,
                        [&](unsigned t, std::size_t i, std::string& err) {
                          auto& b = bufs[t];
                          b[0] = static_cast<std::uint8_t>(i & 0xff);
                          return store.put_range(oid(t), off_for(t), b.data(), b.size(), attrs,
                                                 false, err);
                        }));
  print_phase(run_phase("rread", a.threads, a.count, io,
                        [&](unsigned t, std::size_t, std::string& err) {
                          auto d = store.get_range(oid(t), off_for(t), io, err);
                          if (!d || d->size() != io) {
                            if (err.empty()) err = "short range read";
                            return false;
                          }
                          return true;
                        }));
  std::string err;
  for (unsigned t = 0; t < a.threads; ++t) store.del(oid(t), err);
}

// append: `count` appends of io_size to one log object per worker (a put_range
// at the current end, which is how the HTTP append lands on the store).
void run_append(aios::ObjectStore& store, const BenchArgs& a, const std::string& prefix) {
  const std::unordered_map<std::string, std::string> attrs{{"bench", "1"}};
  auto oid = [&](unsigned t) { return prefix + "-log-" + std::to_string(t); };
  std::vector<std::uint64_t> sizes(a.threads, 0);
  std::vector<std::vector<std::uint8_t>> bufs;
  for (unsigned t = 0; t < a.threads; ++t) bufs.push_back(pattern(a.io_size, t));

  print_phase(run_phase("append", a.threads, a.count, a.io_size,
                        [&](unsigned t, std::size_t i, std::string& err) {
                          auto& b = bufs[t];
                          b[0] = static_cast<std::uint8_t>(i & 0xff);
                          if (!store.put_range(oid(t), sizes[t], b.data(), b.size(), attrs,
                                               false, err)) {
                            return false;
                          }
                          sizes[t] += b.size();
                          return true;
                        }));
  // Verify the logs came out the right length before dropping them.
  std::string err;
  for (unsigned t = 0; t < a.threads; ++t) {
    auto st = store.stat(oid(t), err);
    if (!st || st->size != sizes[t]) {
      throw std::runtime_error("append verify: log " + std::to_string(t) + " size mismatch");
    }
    store.del(oid(t), err);
  }
}

void run_mode(const BenchArgs& a, const fs::path& work, const char* mode, std::size_t obj_size) {
  aios::ObjectStoreOptions opts;
  opts.shard_count = a.shards;
  opts.inline_max_bytes = a.inline_max;
  opts.force_mode = mode;
  opts.data_fsync = a.fsync;
  if (std::string(mode) == "seg") {
    opts.seg_max_record = std::max(obj_size, a.inline_max);
    opts.segment_size = std::max<std::uint64_t>(opts.seg_max_record * 4 + (1u << 20), 8ull << 20);
  }

  aios::ObjectStore store;
  std::string err;
  std::error_code ec;
  const fs::path root = work / (std::string("bench-") + mode + "-" + std::to_string(obj_size));
  fs::create_directories(root, ec);
  if (!store.open(root.string(), opts, err)) {
    throw std::runtime_error(std::string("open ") + mode + " store: " + err);
  }
  const char* label = mode;
  if (std::string(mode) == "inline") label = "inline (SQLite BLOB)";
  else if (std::string(mode) == "fs") label = "filesystem bodies";
  else if (std::string(mode) == "seg") label = "segment log";
  std::cout << "=== " << label << " size=" << obj_size << " io=" << a.io_size
            << " count=" << a.count << " threads=" << a.threads << " shards=" << a.shards
            << (a.fsync ? "" : " no-fsync") << " ===\n";
  if (a.op == "put" || a.op == "all") run_put(store, a, mode, obj_size);
  if (a.op == "range" || a.op == "all") run_range(store, a, mode, obj_size);
  if (a.op == "append" || a.op == "all") run_append(store, a, mode);
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
    auto run = [&](const char* mode, std::size_t sz) { run_mode(args, work, mode, sz); };
    const bool want_inline = args.mode == "inline" || args.mode == "both" || args.mode == "all";
    const bool want_fs = args.mode == "fs" || args.mode == "both" || args.mode == "all";
    const bool want_seg = args.mode == "seg" || args.mode == "all";
    if (!args.sizes.empty()) {
      for (std::size_t sz : args.sizes) {
        if (want_inline) run("inline", sz);
        if (want_fs) run("fs", sz);
        if (want_seg) run("seg", sz);
      }
    } else {
      if (want_inline) run("inline", args.small_size);
      if (want_fs) run("fs", args.large_size);
      if (want_seg) run("seg", args.small_size);
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
