#pragma once

#include "config.hpp"
#include "object/object_service.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <vector>

namespace aios {

// Cluster-shared POSIX path → meta/data layout rules (CAS object posix/layout_rules).
// YAML Config::posix_layout_rules seeds the object when missing.
class PosixLayoutStore {
 public:
  PosixLayoutStore(Config cfg, ObjectService& objects);

  static std::string oid();

  std::vector<PosixLayoutRule> list();
  // Replace entire rule set. Returns false + err on validation/CAS failure.
  bool replace_all(std::vector<PosixLayoutRule> rules, std::string& err);
  // If cluster object missing and YAML has rules, write them once.
  void seed_from_config_if_empty();

  nlohmann::json list_json();
  static nlohmann::json spec_to_json(const PosixLayoutSpec& s);
  static nlohmann::json rule_to_json(const PosixLayoutRule& r);
  static bool spec_from_json(const nlohmann::json& j, PosixLayoutSpec& s, std::string& err);
  static bool rule_from_json(const nlohmann::json& j, PosixLayoutRule& r, std::string& err);
  static bool validate_rule(PosixLayoutRule& r, const Config& cfg, std::string& err);

 private:
  bool load_locked(std::vector<PosixLayoutRule>& out, std::uint64_t& cas, std::string& err);
  bool save_locked(const std::vector<PosixLayoutRule>& rules, std::uint64_t expected_cas,
                   std::string& err);

  Config cfg_;
  ObjectService& objects_;
  std::mutex mu_;
};

}  // namespace aios
