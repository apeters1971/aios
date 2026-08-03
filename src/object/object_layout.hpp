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

// Optional per-request overrides (HTTP headers / TCP++ body). Omit → cluster defaults.
struct LayoutRequest {
  std::optional<std::string> layout;  // "replica" | "ec"
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

  bool is_ec() const { return kind == Kind::Ec; }
};

// Resolve request + prefix rules + Config defaults/caps for one write.
// Precedence: request fields → longest matching layout_rules prefix → cluster defaults.
bool resolve_object_layout(const Config& cfg, const std::string& oid, const LayoutRequest& req,
                           ObjectLayout& out, std::string& err);

// Persist layout keys on a version's attr map (replica or EC).
void apply_layout_attrs(std::unordered_map<std::string, std::string>& attrs,
                        const ObjectLayout& layout);

// Parse x-aios-layout / x-aios-ec-* (header names expected lowercased).
LayoutRequest layout_request_from_headers(
    const std::unordered_map<std::string, std::string>& headers);

// Parse ObjectPut / ObjectPutRange JSON fields: layout, ec_k, ec_m, ec_codec.
LayoutRequest layout_request_from_json(const nlohmann::json& body);

// Emit the same fields onto a TCP++ request body (omit unset overrides).
void apply_layout_request_to_json(nlohmann::json& body, const LayoutRequest& req);

// Force replica layout (txn control-plane objects).
LayoutRequest layout_request_replica();

// Acting-set width for an existing version: aios.n, else k+m if EC, else default_n.
int placement_n_for_attrs(const std::unordered_map<std::string, std::string>& attrs,
                          int default_n);

}  // namespace aios
