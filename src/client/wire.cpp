#include "client/wire.hpp"

#include "client/error.hpp"

#include <map>

namespace aios {
namespace wire {

std::string mode_name(sync_mode m) {
  return m == sync_mode::sync ? "sync" : "async";
}

void check_envelope(const nlohmann::json& j, const char* expect_type) {
  if (!j.is_object() || j.value("aios_stl", 0) != kStlVersion) {
    throw client_error("bad_request", "invalid aios_stl envelope");
  }
  if (j.value("type", "") != expect_type) {
    throw client_error("bad_request", std::string("stl type mismatch, expected ") + expect_type);
  }
}

nlohmann::json make_string_doc(const std::string& data, sync_mode mode) {
  return nlohmann::json{{"aios_stl", kStlVersion},
                        {"type", "string"},
                        {"mode_hint", mode_name(mode)},
                        {"data", data}};
}

nlohmann::json make_map_doc(const std::map<std::string, std::string>& entries, sync_mode mode) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& [k, v] : entries) arr.push_back(nlohmann::json::array({k, v}));
  return nlohmann::json{{"aios_stl", kStlVersion},
                        {"type", "map"},
                        {"mode_hint", mode_name(mode)},
                        {"entries", arr}};
}

nlohmann::json make_unordered_map_doc(const std::unordered_map<std::string, std::string>& entries,
                                      sync_mode mode) {
  // Stable order for deterministic CAS bodies.
  std::map<std::string, std::string> sorted(entries.begin(), entries.end());
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& [k, v] : sorted) arr.push_back(nlohmann::json::array({k, v}));
  return nlohmann::json{{"aios_stl", kStlVersion},
                        {"type", "unordered_map"},
                        {"mode_hint", mode_name(mode)},
                        {"entries", arr}};
}

nlohmann::json make_set_doc(const std::set<std::string>& keys, sync_mode mode) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& k : keys) arr.push_back(k);
  return nlohmann::json{{"aios_stl", kStlVersion},
                        {"type", "set"},
                        {"mode_hint", mode_name(mode)},
                        {"keys", arr}};
}

nlohmann::json make_list_doc(const std::vector<std::string>& items, sync_mode mode,
                             const char* type) {
  return nlohmann::json{{"aios_stl", kStlVersion},
                        {"type", type},
                        {"mode_hint", mode_name(mode)},
                        {"items", items}};
}

std::string parse_string_doc(const std::string& body) {
  auto j = nlohmann::json::parse(body);
  check_envelope(j, "string");
  return j.value("data", "");
}

std::map<std::string, std::string> parse_map_doc(const std::string& body) {
  auto j = nlohmann::json::parse(body);
  check_envelope(j, "map");
  std::map<std::string, std::string> out;
  for (const auto& e : j.at("entries")) {
    if (!e.is_array() || e.size() != 2) {
      throw client_error("bad_request", "bad map entry");
    }
    out[e[0].get<std::string>()] = e[1].get<std::string>();
  }
  return out;
}

std::unordered_map<std::string, std::string> parse_unordered_map_doc(const std::string& body) {
  auto j = nlohmann::json::parse(body);
  check_envelope(j, "unordered_map");
  std::unordered_map<std::string, std::string> out;
  for (const auto& e : j.at("entries")) {
    if (!e.is_array() || e.size() != 2) {
      throw client_error("bad_request", "bad unordered_map entry");
    }
    out[e[0].get<std::string>()] = e[1].get<std::string>();
  }
  return out;
}

std::set<std::string> parse_set_doc(const std::string& body) {
  auto j = nlohmann::json::parse(body);
  check_envelope(j, "set");
  std::set<std::string> out;
  for (const auto& e : j.at("keys")) out.insert(e.get<std::string>());
  return out;
}

std::vector<std::string> parse_list_doc(const std::string& body, const char* expect_type) {
  auto j = nlohmann::json::parse(body);
  check_envelope(j, expect_type);
  std::vector<std::string> out;
  for (const auto& e : j.at("items")) out.push_back(e.get<std::string>());
  return out;
}

}  // namespace wire
}  // namespace aios
