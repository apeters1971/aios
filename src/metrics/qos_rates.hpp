#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace aios {

// Process-local observed posix QoS rates (updated on admit).
void qos_note_observed(const std::string& volume, std::uint32_t project_id, std::uint32_t uid,
                       std::uint32_t gid, std::uint64_t bytes);
nlohmann::json qos_observed_rates_json(const std::string& volume);

}  // namespace aios
