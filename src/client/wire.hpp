#pragma once

#include "client/mode.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {
namespace wire {

constexpr int kStlVersion = 1;

nlohmann::json make_string_doc(const std::string& data, sync_mode mode);
nlohmann::json make_map_doc(const std::map<std::string, std::string>& entries, sync_mode mode);
nlohmann::json make_unordered_map_doc(const std::unordered_map<std::string, std::string>& entries,
                                      sync_mode mode);
nlohmann::json make_set_doc(const std::set<std::string>& keys, sync_mode mode);
nlohmann::json make_list_doc(const std::vector<std::string>& items, sync_mode mode,
                             const char* type);

std::string parse_string_doc(const std::string& body);
std::map<std::string, std::string> parse_map_doc(const std::string& body);
std::unordered_map<std::string, std::string> parse_unordered_map_doc(const std::string& body);
std::set<std::string> parse_set_doc(const std::string& body);
std::vector<std::string> parse_list_doc(const std::string& body, const char* expect_type);

std::string mode_name(sync_mode m);
void check_envelope(const nlohmann::json& j, const char* expect_type);

}  // namespace wire
}  // namespace aios
