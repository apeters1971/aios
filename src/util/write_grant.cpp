#include "util/write_grant.hpp"

#include "util/auth.hpp"

#include <nlohmann/json.hpp>

#include <sstream>

namespace aios {
namespace {

std::string grant_canonical(const WriteGrant& g) {
  std::ostringstream oss;
  oss << "aios-write-grant-v1\n"
      << g.oid << '\n'
      << g.seq << '\n'
      << g.epoch << '\n'
      << g.layout << '\n'
      << g.n << '\n'
      << g.ec_k << '\n'
      << g.ec_m << '\n'
      << g.ec_codec << '\n'
      << g.storage_class << '\n'
      << g.full_size << '\n'
      << g.full_crc << '\n'
      << g.expires_ms << '\n';
  for (const auto& t : g.acting_set) {
    oss << t.node_id << '\t' << t.aios_path << '\t' << t.http_addr << '\t' << t.addr << '\n';
  }
  return oss.str();
}

nlohmann::json acting_to_json(const std::vector<StorageTarget>& acting) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& t : acting) {
    arr.push_back({{"node_id", t.node_id},
                   {"addr", t.addr},
                   {"http_addr", t.http_addr},
                   {"aios_path", t.aios_path}});
  }
  return arr;
}

bool acting_from_json(const nlohmann::json& arr, std::vector<StorageTarget>& out, std::string& err) {
  if (!arr.is_array() || arr.empty()) {
    err = "grant acting_set required";
    return false;
  }
  out.clear();
  out.reserve(arr.size());
  for (const auto& e : arr) {
    if (!e.is_object()) {
      err = "malformed grant acting_set";
      return false;
    }
    StorageTarget t;
    t.node_id = e.value("node_id", "");
    t.addr = e.value("addr", "");
    t.http_addr = e.value("http_addr", "");
    t.aios_path = e.value("aios_path", "");
    if (t.node_id.empty() || t.aios_path.empty()) {
      err = "grant target missing node_id/aios_path";
      return false;
    }
    out.push_back(std::move(t));
  }
  return true;
}

}  // namespace

std::string seal_write_grant(const WriteGrant& g, const std::string& cluster_key, std::string& err) {
  err.clear();
  if (g.oid.empty() || g.seq == 0 || g.acting_set.empty() || cluster_key.empty()) {
    err = "incomplete write grant";
    return {};
  }
  if (g.layout != "replica" && g.layout != "ec") {
    err = "grant layout must be replica or ec";
    return {};
  }
  nlohmann::json j = {{"v", 1},
                      {"oid", g.oid},
                      {"seq", g.seq},
                      {"epoch", g.epoch},
                      {"layout", g.layout},
                      {"n", g.n},
                      {"ec_k", g.ec_k},
                      {"ec_m", g.ec_m},
                      {"ec_codec", g.ec_codec},
                      {"storage_class", g.storage_class},
                      {"full_size", g.full_size},
                      {"full_crc", g.full_crc},
                      {"expires_ms", g.expires_ms},
                      {"acting_set", acting_to_json(g.acting_set)}};
  j["sig"] = hmac_sha256_hex(cluster_key, grant_canonical(g));
  return j.dump();
}

std::optional<WriteGrant> open_write_grant(const std::string& blob, const std::string& cluster_key,
                                           std::int64_t now_ms, std::string& err,
                                           bool allow_expired) {
  err.clear();
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(blob);
  } catch (...) {
    err = "malformed write grant";
    return std::nullopt;
  }
  if (!j.is_object() || j.value("v", 0) != 1) {
    err = "unsupported write grant";
    return std::nullopt;
  }
  WriteGrant g;
  try {
    g.oid = j.at("oid").get<std::string>();
    g.seq = j.at("seq").get<std::uint64_t>();
    g.epoch = j.value("epoch", static_cast<std::uint64_t>(0));
    g.layout = j.at("layout").get<std::string>();
    g.n = j.at("n").get<int>();
    g.ec_k = j.value("ec_k", 0);
    g.ec_m = j.value("ec_m", 0);
    g.ec_codec = j.value("ec_codec", "");
    g.storage_class = j.value("storage_class", "");
    g.full_size = j.value("full_size", static_cast<std::uint64_t>(0));
    g.full_crc = j.value("full_crc", 0u);
    g.expires_ms = j.at("expires_ms").get<std::int64_t>();
  } catch (...) {
    err = "malformed write grant";
    return std::nullopt;
  }
  if (!acting_from_json(j.value("acting_set", nlohmann::json::array()), g.acting_set, err)) {
    return std::nullopt;
  }
  if (static_cast<int>(g.acting_set.size()) != g.n || g.n < 1) {
    err = "grant acting_set size mismatch";
    return std::nullopt;
  }
  const auto sig = j.value("sig", "");
  const auto expect = hmac_sha256_hex(cluster_key, grant_canonical(g));
  if (sig.size() != 64 || sig != expect) {
    err = "bad write grant signature";
    return std::nullopt;
  }
  if (!allow_expired && now_ms > g.expires_ms) {
    err = "write grant expired";
    return std::nullopt;
  }
  if (g.layout != "replica" && g.layout != "ec") {
    err = "grant layout must be replica or ec";
    return std::nullopt;
  }
  return g;
}

}  // namespace aios
