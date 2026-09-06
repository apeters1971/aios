#include "bench/http_bench.hpp"

#include <cstdlib>
#include <stdexcept>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage() {
  std::cout
      << "usage: aios-bench --cluster-key KEY [options]\n"
      << "\n"
      << "Multithreaded HTTP client benchmark.\n"
      << "\n"
      << "  --endpoint [https://]HOST:PORT  HTTP API (default 127.0.0.1:7480)\n"
      << "  --tls-ca PEM           CA bundle for https:// (default: system store)\n"
      << "  --tls-insecure         https:// without certificate verification\n"
      << "  --cluster-key KEY      required shared secret\n"
      << "  --unsigned-payload     object mode: skip the client-side body SHA-256 (sign\n"
      << "                         UNSIGNED-PAYLOAD); default signs the digest like real clients\n"
      << "  --mode object|stl      object = raw PUT/GET (default); stl = aios_client types\n"
      << "  --threads N            worker threads (default: hardware concurrency)\n"
      << "  --ops N                operations per size per phase (default 200;\n"
      << "                         object mode auto-scales: ÷4 at ≥4MiB, ÷16 at ≥16MiB)\n"
      << "  --warmup N             discarded ops per size per phase (default 10;\n"
      << "                         same size scaling as --ops in object mode)\n"
      << "  --sizes LIST           object mode: 1k,4k,… (default 1k..16M)\n"
      << "                        stl mode: string bytes or entry counts (default 16,64,256,1k,4k)\n"
      << "  --ops-mix LIST         create,update,read (default all three)\n"
      << "  --prefix STR           oid / stl name prefix (default bench)\n"
      << "  --layout replica|ec    object mode: per-PUT x-aios-layout\n"
      << "  --ec-k N / --ec-m N / --ec-codec xor|isal\n"
      << "  --stl-types LIST       stl mode: string,map,unordered_map,set,list,deque (default all)\n"
      << "  --stl-sync sync|async|both   stl mode delivery (default both)\n"
      << "  --no-cleanup          leave objects after the run\n"
      << "  --json                machine-readable summary\n"
      << "\n"
      << "Reports IOPS, bandwidth (object mode), and latency p50/p95/p99.\n";
}

std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (c != ' ' && c != '\t') {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::size_t parse_size(const std::string& s) {
  if (s.empty()) throw std::runtime_error("empty size");
  char* end = nullptr;
  const double n = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) throw std::runtime_error("bad size: " + s);
  std::string u = end;
  for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  double mul = 1.0;
  if (u.empty() || u == "b") mul = 1.0;
  else if (u == "k" || u == "kb" || u == "kib") mul = 1024.0;
  else if (u == "m" || u == "mb" || u == "mib") mul = 1024.0 * 1024.0;
  else if (u == "g" || u == "gb" || u == "gib") mul = 1024.0 * 1024.0 * 1024.0;
  else throw std::runtime_error("bad size unit: " + s);
  if (n < 0) throw std::runtime_error("negative size");
  return static_cast<std::size_t>(n * mul + 0.5);
}

bool parse_args(int argc, char** argv, aios::HttpBenchConfig& a, bool& json_out) {
  bool sizes_set = false;
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
    if (arg == "--endpoint") {
      const char* v = need("--endpoint");
      if (!v) return false;
      a.endpoint = v;
      continue;
    }
    if (arg == "--cluster-key") {
      const char* v = need("--cluster-key");
      if (!v) return false;
      a.cluster_key = v;
      continue;
    }
    if (arg == "--tls-ca") {
      const char* v = need("--tls-ca");
      if (!v) return false;
      a.tls_ca = v;
      a.tls = true;
      continue;
    }
    if (arg == "--tls-insecure") {
      a.tls_insecure = true;
      a.tls = true;
      continue;
    }
    if (arg == "--unsigned-payload") {
      a.unsigned_payload = true;
      continue;
    }
    if (arg == "--mode") {
      const char* v = need("--mode");
      if (!v) return false;
      a.mode = v;
      continue;
    }
    if (arg == "--threads") {
      const char* v = need("--threads");
      if (!v) return false;
      a.threads = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
      continue;
    }
    if (arg == "--ops") {
      const char* v = need("--ops");
      if (!v) return false;
      a.ops = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
      continue;
    }
    if (arg == "--warmup") {
      const char* v = need("--warmup");
      if (!v) return false;
      a.warmup = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
      continue;
    }
    if (arg == "--prefix") {
      const char* v = need("--prefix");
      if (!v) return false;
      a.prefix = v;
      continue;
    }
    if (arg == "--sizes") {
      const char* v = need("--sizes");
      if (!v) return false;
      a.sizes.clear();
      sizes_set = true;
      try {
        for (const auto& tok : split_csv(v)) a.sizes.push_back(parse_size(tok));
      } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return false;
      }
      continue;
    }
    if (arg == "--ops-mix") {
      const char* v = need("--ops-mix");
      if (!v) return false;
      a.do_create = a.do_update = a.do_read = false;
      for (const auto& tok : split_csv(v)) {
        if (tok == "create") a.do_create = true;
        else if (tok == "update") a.do_update = true;
        else if (tok == "read") a.do_read = true;
        else {
          std::cerr << "unknown op in --ops-mix: " << tok << "\n";
          return false;
        }
      }
      continue;
    }
    if (arg == "--layout") {
      const char* v = need("--layout");
      if (!v) return false;
      a.layout = v;
      continue;
    }
    if (arg == "--ec-k") {
      const char* v = need("--ec-k");
      if (!v) return false;
      a.ec_k = std::stoi(v);
      continue;
    }
    if (arg == "--ec-m") {
      const char* v = need("--ec-m");
      if (!v) return false;
      a.ec_m = std::stoi(v);
      continue;
    }
    if (arg == "--ec-codec") {
      const char* v = need("--ec-codec");
      if (!v) return false;
      a.ec_codec = v;
      continue;
    }
    if (arg == "--stl-types") {
      const char* v = need("--stl-types");
      if (!v) return false;
      a.stl_types = split_csv(v);
      continue;
    }
    if (arg == "--stl-sync") {
      const char* v = need("--stl-sync");
      if (!v) return false;
      a.stl_sync = v;
      continue;
    }
    if (arg == "--no-cleanup") {
      a.cleanup = false;
      continue;
    }
    if (arg == "--json") {
      json_out = true;
      continue;
    }
    std::cerr << "unknown arg: " << arg << "\n";
    return false;
  }
  (void)sizes_set;
  if (a.cluster_key.empty()) {
    std::cerr << "--cluster-key is required\n";
    return false;
  }
  aios::http_bench_apply_cli_defaults(a);
  const auto err = aios::http_bench_validate(a);
  if (!err.empty()) {
    std::cerr << err << "\n";
    return false;
  }
  return true;
}

void print_human(const nlohmann::json& doc) {
  const auto results = doc.value("results", nlohmann::json::array());
  const bool stl = doc.value("mode", "") == "stl";
  if (stl) {
    std::cout << "aios-bench mode=stl endpoint=" << doc.value("endpoint", "")
              << " threads=" << doc.value("threads", 0) << " ops=" << doc.value("ops", 0) << "\n";
    std::cout << std::left << std::setw(14) << "type" << std::setw(7) << "sync" << std::setw(8)
              << "size" << std::setw(8) << "op" << std::right << std::setw(8) << "ok"
              << std::setw(6) << "err" << std::setw(10) << "iops" << std::setw(10) << "p50_ms"
              << std::setw(10) << "p95_ms" << std::setw(10) << "p99_ms" << std::setw(10) << "avg_ms"
              << "\n";
    for (const auto& st : results) {
      std::cout << std::left << std::setw(14) << st.value("stl_type", "")
                << std::setw(7) << st.value("stl_sync", "")
                << std::setw(8) << st.value("size_label", "")
                << std::setw(8) << st.value("op", "") << std::right << std::setw(8)
                << st.value("ok", 0) << std::setw(6) << st.value("err", 0) << std::setw(10)
                << std::fixed << std::setprecision(1) << st.value("iops", 0.0) << std::setw(10)
                << std::setprecision(3) << st.value("p50_ms", 0.0) << std::setw(10)
                << st.value("p95_ms", 0.0) << std::setw(10) << st.value("p99_ms", 0.0)
                << std::setw(10) << st.value("avg_ms", 0.0) << "\n";
    }
    return;
  }
  std::cout << "aios-bench mode=object endpoint=" << doc.value("endpoint", "")
            << " threads=" << doc.value("threads", 0) << " ops=" << doc.value("ops", 0)
            << " (ops÷4 at ≥4MiB, ÷16 at ≥16MiB)\n";
  std::cout << std::left << std::setw(8) << "size" << std::setw(8) << "op" << std::right
            << std::setw(8) << "ok" << std::setw(6) << "err" << std::setw(10) << "iops"
            << std::setw(10) << "MiB/s" << std::setw(10) << "p50_ms" << std::setw(10) << "p95_ms"
            << std::setw(10) << "p99_ms" << std::setw(10) << "avg_ms" << "\n";
  for (const auto& st : results) {
    std::cout << std::left << std::setw(8) << st.value("size_label", "") << std::setw(8)
              << st.value("op", "") << std::right << std::setw(8) << st.value("ok", 0)
              << std::setw(6) << st.value("err", 0) << std::setw(10) << std::fixed
              << std::setprecision(1) << st.value("iops", 0.0) << std::setw(10)
              << std::setprecision(2) << st.value("mib_s", 0.0) << std::setw(10)
              << std::setprecision(3) << st.value("p50_ms", 0.0) << std::setw(10)
              << st.value("p95_ms", 0.0) << std::setw(10) << st.value("p99_ms", 0.0)
              << std::setw(10) << st.value("avg_ms", 0.0) << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  aios::HttpBenchConfig args;
  bool json_out = false;
  if (!parse_args(argc, argv, args, json_out)) {
    usage();
    return 2;
  }
  if (!json_out) {
    std::cerr << "aios-bench probing " << args.endpoint << " …\n" << std::flush;
  }
  // Long runs (STL sync mode at large sizes is thousands of round trips per op)
  // otherwise look hung: report each measured phase as it completes.
  aios::HttpBenchProgress progress;
  if (!json_out) {
    progress = [](const nlohmann::json& st) {
      std::cerr << "  done " << st.value("stl_type", std::string("object")) << " "
                << st.value("stl_sync", std::string("")) << " " << st.value("size_label", "")
                << " " << st.value("op", "") << ": ok=" << st.value("ok", 0)
                << " err=" << st.value("err", 0) << " iops=" << std::fixed << std::setprecision(1)
                << st.value("iops", 0.0) << " p50=" << std::setprecision(3)
                << st.value("p50_ms", 0.0) << "ms\n"
                << std::flush;
    };
  }
  const auto doc = aios::run_http_bench(args, {}, progress);
  if (doc.contains("error")) {
    std::cerr << doc["error"].get<std::string>() << "\n";
    return 1;
  }
  if (json_out) {
    std::cout << doc.dump(2) << "\n";
  } else {
    print_human(doc);
  }
  std::size_t total_err = 0;
  for (const auto& r : doc.value("results", nlohmann::json::array())) {
    total_err += r.value("err", 0);
  }
  return total_err > 0 ? 1 : 0;
}
