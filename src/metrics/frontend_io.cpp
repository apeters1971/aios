#include "metrics/frontend_io.hpp"

#include "kernel/aiosvd_uapi.h"

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <sys/ioctl.h>
#include <unistd.h>
#include <unordered_map>

namespace aios {
namespace {

struct FrontendCounters {
  std::atomic<std::uint64_t> read_ops{0};
  std::atomic<std::uint64_t> write_ops{0};
  std::atomic<std::uint64_t> read_bytes{0};
  std::atomic<std::uint64_t> write_bytes{0};
};

std::mutex g_mu;
std::unordered_map<std::string, std::unique_ptr<FrontendCounters>> g_frontends;

FrontendCounters& bucket(const std::string& name) {
  std::lock_guard lock(g_mu);
  auto& p = g_frontends[name];
  if (!p) p = std::make_unique<FrontendCounters>();
  return *p;
}

nlohmann::json counters_json(const FrontendCounters& c) {
  return {{"read_ops", c.read_ops.load()},
          {"write_ops", c.write_ops.load()},
          {"read_bytes", c.read_bytes.load()},
          {"write_bytes", c.write_bytes.load()}};
}

std::string normalize_frontend(const std::string& frontend) {
  if (frontend == kFrontendS3 || frontend == kFrontendFs || frontend == kFrontendVbd)
    return frontend;
  if (frontend.empty()) return kFrontendFs;
  return frontend;
}

nlohmann::json object_slice(const nlohmann::json& ops_by_label, const char* key) {
  if (ops_by_label.is_object() && ops_by_label.contains(key)) return ops_by_label[key];
  return nlohmann::json::object();
}

}  // namespace

void note_frontend_io(const std::string& frontend, bool is_write, std::uint64_t bytes) {
  auto& c = bucket(normalize_frontend(frontend));
  if (is_write) {
    c.write_ops.fetch_add(1, std::memory_order_relaxed);
    c.write_bytes.fetch_add(bytes, std::memory_order_relaxed);
  } else {
    c.read_ops.fetch_add(1, std::memory_order_relaxed);
    c.read_bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

nlohmann::json frontend_io_json() {
  std::lock_guard lock(g_mu);
  nlohmann::json out = nlohmann::json::object();
  // Always publish the three reserved frontends (zeros if idle).
  for (const char* name : {kFrontendS3, kFrontendFs, kFrontendVbd}) {
    auto it = g_frontends.find(name);
    if (it != g_frontends.end() && it->second) out[name] = counters_json(*it->second);
    else out[name] = counters_json(FrontendCounters{});
  }
  for (const auto& [k, v] : g_frontends) {
    if (k == kFrontendS3 || k == kFrontendFs || k == kFrontendVbd) continue;
    if (v) out[k] = counters_json(*v);
  }
  return out;
}

nlohmann::json vbd_devices_json() {
  nlohmann::json devices = nlohmann::json::array();
  const int fd = ::open("/dev/" AIOSVD_CTL_NAME, O_RDONLY);
  if (fd < 0) return devices;
  aiosvd_list_arg list{};
  if (::ioctl(fd, AIOSVD_IOCTL_LIST, &list) == 0) {
    for (uint32_t i = 0; i < list.count && i < AIOSVD_MAX_DEVS; ++i) {
      const auto& e = list.entries[i];
      devices.push_back({{"dev_id", e.dev_id},
                         {"pool", e.pool},
                         {"name", e.name},
                         {"size", e.size},
                         {"bytes_read", e.bytes_read},
                         {"bytes_written", e.bytes_written},
                         {"ops_read", e.ops_read},
                         {"ops_write", e.ops_write},
                         {"ops_discard", e.ops_discard},
                         {"errors", e.errors},
                         {"timeouts", e.timeouts},
                         {"reconnects", e.reconnects},
                         {"cache_hits", e.cache_hits}});
    }
  }
  ::close(fd);
  return devices;
}

nlohmann::json io_frontends_admin_json(const nlohmann::json& ops_by_label) {
  auto logical = frontend_io_json();
  auto devices = vbd_devices_json();
  // Aggregate VBD device counters into logical.vbd when kernel stats exist (authoritative).
  if (!devices.empty()) {
    std::uint64_t ro = 0, wo = 0, rb = 0, wb = 0;
    for (const auto& d : devices) {
      ro += d.value("ops_read", 0ull);
      wo += d.value("ops_write", 0ull);
      rb += d.value("bytes_read", 0ull);
      wb += d.value("bytes_written", 0ull);
    }
    logical[kFrontendVbd] = {{"read_ops", ro},
                             {"write_ops", wo},
                             {"read_bytes", rb},
                             {"write_bytes", wb},
                             {"source", "aiosvd"}};
  } else if (logical.contains(kFrontendVbd)) {
    logical[kFrontendVbd]["source"] = "none";
  }
  if (logical.contains(kFrontendS3)) logical[kFrontendS3]["source"] = "posix";
  if (logical.contains(kFrontendFs)) logical[kFrontendFs]["source"] = "posix";

  return {{"logical", logical},
          {"object_ops",
           {{kFrontendS3, object_slice(ops_by_label, kFrontendS3)},
            {kFrontendFs, object_slice(ops_by_label, kFrontendFs)},
            {kFrontendVbd, object_slice(ops_by_label, kFrontendVbd)}}},
          {"vbd_devices", devices}};
}

}  // namespace aios
