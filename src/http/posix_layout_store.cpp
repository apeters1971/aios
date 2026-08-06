#include "http/posix_layout_store.hpp"

#include "util/log.hpp"

#include <algorithm>

namespace aios {
namespace {

std::uint64_t cas_from_attrs(const std::unordered_map<std::string, std::string>& attrs) {
  auto it = attrs.find("aios.posix.cas");
  if (it == attrs.end()) return 0;
  try {
    return static_cast<std::uint64_t>(std::stoull(it->second));
  } catch (...) {
    return 0;
  }
}

bool put_cas(ObjectService& objects, const std::string& oid, const std::string& body,
             std::uint64_t expected_cas, std::string& err) {
  const std::uint64_t new_cas = expected_cas + 1;
  std::unordered_map<std::string, std::string> attrs{{"aios.posix.cas", std::to_string(new_cas)}};
  std::vector<AttrPrecondition> preds;
  if (expected_cas == 0) {
    auto head = objects.api_head(oid, {});
    if (!head.ok || !head.info) {
      preds.push_back({AttrPrecondition::Kind::MustNotExist, {}, {}});
    } else if (cas_from_attrs(head.attrs) == 0) {
      preds.push_back({AttrPrecondition::Kind::Absent, "aios.posix.cas", {}});
    } else {
      err = "cas mismatch";
      return false;
    }
  } else {
    preds.push_back(
        {AttrPrecondition::Kind::Eq, "aios.posix.cas", std::to_string(expected_cas)});
  }
  auto r = objects.api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                           attrs, true, preds);
  if (!r.ok) {
    err = r.error.empty() ? r.code : r.error;
    return false;
  }
  return true;
}

std::string lower_copy(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

bool valid_storage_class_name(const std::string& s) {
  if (s.empty()) return false;
  for (unsigned char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

bool validate_spec(PosixLayoutSpec& spec, const Config& cfg, const char* which, std::string& err) {
  (void)cfg;
  if (!spec.layout.empty()) {
    spec.layout = lower_copy(spec.layout);
    if (spec.layout != "replica" && spec.layout != "ec") {
      err = std::string("posix_layout_rules ") + which + " layout must be 'replica' or 'ec'";
      return false;
    }
  }
  if (spec.storage_class) {
    *spec.storage_class = lower_copy(*spec.storage_class);
    if (!valid_storage_class_name(*spec.storage_class)) {
      err = std::string("posix_layout_rules ") + which + " storage_class invalid";
      return false;
    }
  }
  return true;
}

}  // namespace

PosixLayoutStore::PosixLayoutStore(Config cfg, ObjectService& objects)
    : cfg_(std::move(cfg)), objects_(objects) {}

std::string PosixLayoutStore::oid() { return "posix/layout_rules"; }

nlohmann::json PosixLayoutStore::spec_to_json(const PosixLayoutSpec& s) {
  nlohmann::json j = nlohmann::json::object();
  if (!s.layout.empty()) j["layout"] = s.layout;
  if (s.storage_class) j["storage_class"] = *s.storage_class;
  if (s.ec_k) j["ec_k"] = *s.ec_k;
  if (s.ec_m) j["ec_m"] = *s.ec_m;
  if (s.ec_codec) j["ec_codec"] = *s.ec_codec;
  return j;
}

nlohmann::json PosixLayoutStore::rule_to_json(const PosixLayoutRule& r) {
  nlohmann::json j{{"path", r.path},
                   {"meta", spec_to_json(r.meta)},
                   {"data", spec_to_json(r.data)}};
  if (r.volume) j["volume"] = *r.volume;
  return j;
}

bool PosixLayoutStore::spec_from_json(const nlohmann::json& j, PosixLayoutSpec& s, std::string& err) {
  if (!j.is_object()) {
    err = "meta/data must be an object";
    return false;
  }
  s = {};
  if (j.contains("layout") && j["layout"].is_string()) s.layout = j["layout"].get<std::string>();
  if (j.contains("storage_class") && j["storage_class"].is_string()) {
    s.storage_class = j["storage_class"].get<std::string>();
  }
  if (j.contains("ec_k") && !j["ec_k"].is_null()) s.ec_k = j["ec_k"].get<int>();
  if (j.contains("ec_m") && !j["ec_m"].is_null()) s.ec_m = j["ec_m"].get<int>();
  if (j.contains("ec_codec") && j["ec_codec"].is_string()) {
    s.ec_codec = j["ec_codec"].get<std::string>();
  }
  return true;
}

bool PosixLayoutStore::rule_from_json(const nlohmann::json& j, PosixLayoutRule& r, std::string& err) {
  if (!j.is_object()) {
    err = "rule must be an object";
    return false;
  }
  r = {};
  r.path = j.value("path", "/");
  if (j.contains("volume") && j["volume"].is_string()) r.volume = j["volume"].get<std::string>();
  if (j.contains("meta")) {
    if (!spec_from_json(j["meta"], r.meta, err)) return false;
  }
  if (j.contains("data")) {
    if (!spec_from_json(j["data"], r.data, err)) return false;
  }
  return true;
}

bool PosixLayoutStore::validate_rule(PosixLayoutRule& r, const Config& cfg, std::string& err) {
  if (r.path.empty() || r.path[0] != '/') {
    err = "path must start with /";
    return false;
  }
  while (r.path.size() > 1 && r.path.back() == '/') r.path.pop_back();
  if (r.volume) *r.volume = lower_copy(*r.volume);
  if (!validate_spec(r.meta, cfg, "meta", err)) return false;
  if (!validate_spec(r.data, cfg, "data", err)) return false;
  return true;
}

bool PosixLayoutStore::load_locked(std::vector<PosixLayoutRule>& out, std::uint64_t& cas,
                                   std::string& err) {
  out.clear();
  cas = 0;
  auto r = objects_.api_get(oid(), std::nullopt, std::nullopt, {});
  if (!r.ok || !r.data) return true;  // missing → empty
  cas = cas_from_attrs(r.attrs);
  try {
    const auto j = nlohmann::json::parse(
        std::string(reinterpret_cast<const char*>(r.data->data()), r.data->size()));
    if (!j.contains("rules") || !j["rules"].is_array()) {
      err = "bad posix/layout_rules document";
      return false;
    }
    for (const auto& jr : j["rules"]) {
      PosixLayoutRule rule;
      if (!rule_from_json(jr, rule, err)) return false;
      if (!validate_rule(rule, cfg_, err)) return false;
      out.push_back(std::move(rule));
    }
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
  return true;
}

bool PosixLayoutStore::save_locked(const std::vector<PosixLayoutRule>& rules,
                                   std::uint64_t expected_cas, std::string& err) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& r : rules) arr.push_back(rule_to_json(r));
  nlohmann::json doc{{"aios_posix_layout", 1}, {"rules", arr}};
  return put_cas(objects_, oid(), doc.dump(), expected_cas, err);
}

std::vector<PosixLayoutRule> PosixLayoutStore::list() {
  std::lock_guard lock(mu_);
  std::vector<PosixLayoutRule> out;
  std::uint64_t cas = 0;
  std::string err;
  if (!load_locked(out, cas, err)) {
    AIOS_LOG_WARN("posix layout load: ", err);
    return {};
  }
  return out;
}

nlohmann::json PosixLayoutStore::list_json() {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& r : list()) arr.push_back(rule_to_json(r));
  return {{"posix_layout_rules", arr}};
}

bool PosixLayoutStore::replace_all(std::vector<PosixLayoutRule> rules, std::string& err) {
  for (auto& r : rules) {
    if (!validate_rule(r, cfg_, err)) return false;
  }
  std::sort(rules.begin(), rules.end(),
            [](const PosixLayoutRule& a, const PosixLayoutRule& b) {
              return a.path.size() > b.path.size();
            });
  std::lock_guard lock(mu_);
  std::vector<PosixLayoutRule> cur;
  std::uint64_t cas = 0;
  if (!load_locked(cur, cas, err)) return false;
  return save_locked(rules, cas, err);
}

void PosixLayoutStore::seed_from_config_if_empty() {
  std::lock_guard lock(mu_);
  std::vector<PosixLayoutRule> cur;
  std::uint64_t cas = 0;
  std::string err;
  if (!load_locked(cur, cas, err)) {
    AIOS_LOG_WARN("posix layout seed load: ", err);
    return;
  }
  if (!cur.empty() || cfg_.posix_layout_rules.empty()) return;
  cur = cfg_.posix_layout_rules;
  for (auto& r : cur) {
    if (!validate_rule(r, cfg_, err)) {
      AIOS_LOG_WARN("posix layout seed validate: ", err);
      return;
    }
  }
  if (!save_locked(cur, cas, err)) {
    AIOS_LOG_WARN("posix layout seed save: ", err);
  } else {
    AIOS_LOG_INFO("seeded posix/layout_rules from YAML (", cur.size(), " rules)");
  }
}

}  // namespace aios
