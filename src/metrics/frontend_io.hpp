#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace aios {

// Reserved app_label / frontend names for IO monitoring breakdown.
inline constexpr const char* kFrontendS3 = "s3";
inline constexpr const char* kFrontendFs = "fs";
inline constexpr const char* kFrontendVbd = "vbd";

// Logical frontend IO (posix read/write for S3/FS). Separate from object chunk OPS.
void note_frontend_io(const std::string& frontend, bool is_write, std::uint64_t bytes);
nlohmann::json frontend_io_json();

// Best-effort scrape of mapped aiosvd devices via /dev/aiosvd_ctl (empty if unavailable).
nlohmann::json vbd_devices_json();

// Combined admin payload: logical frontends + object ops_by_label slice + vbd devices.
nlohmann::json io_frontends_admin_json(const nlohmann::json& ops_by_label);

}  // namespace aios
