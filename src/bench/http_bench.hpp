#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aios {

// Same knobs as the aios-bench CLI (object PUT/GET or STL client types).
struct HttpBenchConfig {
  std::string endpoint{"127.0.0.1:7480"};
  std::string cluster_key;
  unsigned threads{0};  // 0 = hardware_concurrency
  std::size_t ops{200};
  std::size_t warmup{10};
  std::string prefix{"bench"};
  std::vector<std::size_t> sizes;
  bool do_create{true};
  bool do_update{true};
  bool do_read{true};
  bool cleanup{true};
  std::string layout;
  int ec_k{0};
  int ec_m{0};
  std::string ec_codec;
  std::string mode{"object"};
  std::vector<std::string> stl_types;
  std::string stl_sync{"both"};
};

void http_bench_apply_cli_defaults(HttpBenchConfig& c);
void http_bench_apply_ui_defaults(HttpBenchConfig& c);
std::string http_bench_validate(const HttpBenchConfig& c);
std::string http_bench_validate_admin(const HttpBenchConfig& c);

HttpBenchConfig http_bench_from_json(const nlohmann::json& j);
nlohmann::json http_bench_config_json(const HttpBenchConfig& c);  // no cluster_key

using HttpBenchCancel = std::function<bool()>;
using HttpBenchProgress = std::function<void(const nlohmann::json& phase)>;

// Blocking run. Returns {mode,endpoint,threads,ops,results[],cancelled,error?}.
nlohmann::json run_http_bench(const HttpBenchConfig& cfg, HttpBenchCancel cancel = {},
                              HttpBenchProgress progress = {});

// One in-process job for the admin console (start / poll / stop).
class HttpBenchJob {
 public:
  HttpBenchJob(std::string default_endpoint, std::string cluster_key);
  ~HttpBenchJob();
  HttpBenchJob(const HttpBenchJob&) = delete;
  HttpBenchJob& operator=(const HttpBenchJob&) = delete;

  nlohmann::json start(const nlohmann::json& req);
  nlohmann::json stop();
  nlohmann::json status() const;
  nlohmann::json defaults() const;

 private:
  void join_worker();

  std::string default_endpoint_;
  std::string cluster_key_;
  mutable std::mutex mu_;
  std::string state_{"idle"};
  std::string error_;
  std::int64_t started_ms_{0};
  std::int64_t finished_ms_{0};
  HttpBenchConfig cfg_{};
  nlohmann::json results_ = nlohmann::json::array();
  nlohmann::json last_phase_;
  std::atomic<bool> cancel_{false};
  std::mutex start_mu_;
  std::thread worker_;
};

}  // namespace aios
