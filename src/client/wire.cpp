#include "client/wire.hpp"

#include "client/error.hpp"

#include <map>

namespace aios {
namespace wire {

std::string mode_name(sync_mode m) {
  return m == sync_mode::sync ? "sync" : "async";
}

nlohmann::json parse_json(const std::string& body) {
  try {
    return nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    throw client_error("bad_request", std::string("malformed stl document: ") + e.what());
  }
}

int envelope_version(const nlohmann::json& j) {
  if (!j.is_object()) return 0;
  auto it = j.find("aios_stl");
  if (it == j.end() || !it->is_number_integer()) return 0;
  return it->get<int>();
}

std::string envelope_type(const nlohmann::json& j) {
  if (!j.is_object()) return {};
  auto it = j.find("type");
  if (it == j.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

std::uint64_t u64_field(const nlohmann::json& j, const char* key, std::uint64_t def) {
  if (!j.is_object()) return def;
  auto it = j.find(key);
  if (it == j.end()) return def;
  if (!it->is_number_unsigned()) {
    throw client_error("bad_request", std::string("stl field not a u64: ") + key);
  }
  return it->get<std::uint64_t>();
}

const nlohmann::json& array_field(const nlohmann::json& j, const char* key) {
  auto it = j.is_object() ? j.find(key) : j.end();
  if (it == j.end() || !it->is_array()) {
    throw client_error("bad_request", std::string("stl field not an array: ") + key);
  }
  return *it;
}

std::string string_element(const nlohmann::json& e, const char* what) {
  if (!e.is_string()) throw client_error("bad_request", std::string("stl ") + what + " not a string");
  return e.get<std::string>();
}

void check_envelope(const nlohmann::json& j, const char* expect_type) {
  if (envelope_version(j) != kStlVersion) {
    throw client_error("bad_request", "invalid aios_stl envelope");
  }
  if (envelope_type(j) != expect_type) {
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
  const auto j = parse_json(body);
  check_envelope(j, "string");
  return j.value("data", "");
}

std::map<std::string, std::string> parse_map_doc(const std::string& body) {
  const auto j = parse_json(body);
  check_envelope(j, "map");
  std::map<std::string, std::string> out;
  for (const auto& e : array_field(j, "entries")) {
    if (!e.is_array() || e.size() != 2) {
      throw client_error("bad_request", "bad map entry");
    }
    out[string_element(e[0], "map key")] = string_element(e[1], "map value");
  }
  return out;
}

std::unordered_map<std::string, std::string> parse_unordered_map_doc(const std::string& body) {
  const auto j = parse_json(body);
  check_envelope(j, "unordered_map");
  std::unordered_map<std::string, std::string> out;
  for (const auto& e : array_field(j, "entries")) {
    if (!e.is_array() || e.size() != 2) {
      throw client_error("bad_request", "bad unordered_map entry");
    }
    out[string_element(e[0], "unordered_map key")] = string_element(e[1], "unordered_map value");
  }
  return out;
}

std::set<std::string> parse_set_doc(const std::string& body) {
  const auto j = parse_json(body);
  check_envelope(j, "set");
  std::set<std::string> out;
  for (const auto& e : array_field(j, "keys")) out.insert(string_element(e, "set key"));
  return out;
}

std::vector<std::string> parse_list_doc(const std::string& body, const char* expect_type) {
  const auto j = parse_json(body);
  check_envelope(j, expect_type);
  std::vector<std::string> out;
  for (const auto& e : array_field(j, "items")) out.push_back(string_element(e, "list item"));
  return out;
}

}  // namespace wire
}  // namespace aios
