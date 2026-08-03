#pragma once

#include "config.hpp"
#include "ec/ec_attrs.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <unordered_map>

namespace aios {

inline constexpr const char* kLayoutAttr = "aios.layout";
inline constexpr const char* kLayoutNAttr = "aios.n";
inline constexpr const char* kStorageClassAttr = "aios.storage_class";
inline constexpr const char* kStorageClassPrevAttr = "aios.storage_class_prev";
inline constexpr const char* kTransitionAttr = "aios.transition";

// Optional per-request overrides (HTTP headers / TCP++ body). Omit → cluster defaults.
struct LayoutRequest {
  std::optional<std::string> layout;  // "replica" | "ec"
  std::optional<std::string> storage_class;
  std::optional<int> ec_k;
  std::optional<int> ec_m;
  std::optional<std::string> ec_codec;
};

struct ObjectLayout {
  enum class Kind { Replica, Ec } kind{Kind::Replica};
  int n{0};
  int ec_k{0};
  int ec_m{0};
  std::string ec_codec;
  std::string storage_class;

  bool is_ec() const { return kind == Kind::Ec; }
};

// Resolve request + prefix rules + Config defaults/caps for one write.
// Precedence: request fields → longest matching layout_rules prefix → cluster defaults.
bool resolve_object_layout(const Config& cfg, const std::string& oid, const LayoutRequest& req,
                           ObjectLayout& out, std::string& err);

// Persist layout + storage_class keys on a version's attr map.
void apply_layout_attrs(std::unordered_map<std::string, std::string>& attrs,
                        const ObjectLayout& layout);

// Parse x-aios-layout / x-aios-storage-class / x-aios-ec-* (header names lowercased).
LayoutRequest layout_request_from_headers(
    const std::unordered_map<std::string, std::string>& headers);

// Parse ObjectPut / ObjectPutRange JSON fields.
LayoutRequest layout_request_from_json(const nlohmann::json& body);

// Emit the same fields onto a TCP++ request body (omit unset overrides).
void apply_layout_request_to_json(nlohmann::json& body, const LayoutRequest& req);

// Force replica layout (txn control-plane objects).
LayoutRequest layout_request_replica();

// Acting-set width for an existing version: aios.n, else k+m if EC, else default_n.
int placement_n_for_attrs(const std::unordered_map<std::string, std::string>& attrs,
                          int default_n);

// Storage class for an existing version; falls back to default_class.
std::string storage_class_for_attrs(const std::unordered_map<std::string, std::string>& attrs,
                                    const std::string& default_class);

// Previous class during a transition (empty if none).
std::string storage_class_prev_for_attrs(
    const std::unordered_map<std::string, std::string>& attrs);

}  // namespace aios
