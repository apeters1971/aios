#include "posix/posix_layout.hpp"

#include <nlohmann/json.hpp>

namespace aios {
namespace posix {
namespace {

constexpr auto kLayoutRefresh = std::chrono::seconds(30);

}  // namespace

PutLayout put_layout_from_spec(const PosixLayoutSpec& spec) {
  PutLayout out;
  if (!spec.layout.empty()) out.layout = spec.layout;
  if (spec.storage_class) out.storage_class = *spec.storage_class;
  if (spec.ec_k) out.ec_k = *spec.ec_k;
  if (spec.ec_m) out.ec_m = *spec.ec_m;
  if (spec.ec_codec) out.ec_codec = *spec.ec_codec;
  return out;
}

std::string normalize_fs_path(std::string path) {
  if (path.empty() || path[0] != '/') path = "/" + path;
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  // Collapse duplicate slashes lightly.
  std::string out;
  out.reserve(path.size());
  for (char c : path) {
    if (c == '/' && !out.empty() && out.back() == '/') continue;
    out.push_back(c);
  }
  if (out.empty()) out = "/";
  return out;
}

std::optional<PosixLayoutRule> match_posix_layout_rule(
    const std::vector<PosixLayoutRule>& rules, const std::string& volume,
    const std::string& path) {
  const std::string p = normalize_fs_path(path);
  const PosixLayoutRule* best = nullptr;
  std::size_t best_len = 0;
  for (const auto& r : rules) {
    if (r.volume && *r.volume != volume) continue;
    const std::string rp = normalize_fs_path(r.path);
    if (p == rp || (rp != "/" && p.rfind(rp + "/", 0) == 0) || (rp == "/" && p[0] == '/')) {
      if (rp.size() >= best_len) {
        best = &r;
        best_len = rp.size();
      }
    }
  }
  if (!best) return std::nullopt;
  return *best;
}

std::string layout_domain_key(const std::optional<PosixLayoutRule>& rule) {
  if (!rule) return {};
  auto enc = [](const PosixLayoutSpec& s) {
    return (s.layout.empty() ? "-" : s.layout) + "|" +
           (s.storage_class ? *s.storage_class : std::string("-")) + "|" +
           (s.ec_k ? std::to_string(*s.ec_k) : "-") + "|" +
           (s.ec_m ? std::to_string(*s.ec_m) : "-") + "|" +
           (s.ec_codec ? *s.ec_codec : std::string("-"));
  };
  return enc(rule->meta) + "#" + enc(rule->data);
}

std::string path_of_ino(FsState& st, uint64_t ino) {
  if (ino == 0 || ino == kRootIno) return "/";
  std::vector<std::string> parts;
  uint64_t cur = ino;
  for (int guard = 0; guard < 1024 && cur != 0 && cur != kRootIno; ++guard) {
    auto m = load_inode(st, cur);
    if (!m.exists) break;
    uint64_t parent = m.parent_ino;
    if (parent == 0) parent = kRootIno;
    DirTable dt(st.session, st.volume, parent);
    dt.load();
    std::string name;
    for (const auto& [n, child] : dt.entries()) {
      if (child == cur) {
        name = n;
        break;
      }
    }
    if (name.empty()) break;
    parts.push_back(std::move(name));
    if (parent == kRootIno) break;
    cur = parent;
  }
  std::string path = "/";
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    if (path.size() > 1) path += "/";
    path += *it;
  }
  return path;
}

std::shared_ptr<const std::vector<PosixLayoutRule>> refresh_layout_rules(FsState& st) {
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard lock(st.layout_mu);
    if (st.layout_rules_loaded.time_since_epoch().count() != 0 &&
        now - st.layout_rules_loaded < kLayoutRefresh) {
      return st.layout_rules ? st.layout_rules
                             : std::make_shared<const std::vector<PosixLayoutRule>>();
    }
    // Claim the refresh so concurrent callers keep serving the current version.
    st.layout_rules_loaded = now;
  }
  auto fresh = std::make_shared<std::vector<PosixLayoutRule>>();
  try {
    auto snap = st.session.get_object("posix/layout_rules");
    if (snap.exists && !snap.body.empty()) {
      auto j = nlohmann::json::parse(snap.body);
      if (j.contains("rules") && j["rules"].is_array()) {
        for (const auto& jr : j["rules"]) {
          PosixLayoutRule r;
          r.path = jr.value("path", "/");
          if (jr.contains("volume") && jr["volume"].is_string()) {
            r.volume = jr["volume"].get<std::string>();
          }
          auto parse_spec = [](const nlohmann::json& s, PosixLayoutSpec& out) {
            if (!s.is_object()) return;
            if (s.contains("layout") && s["layout"].is_string()) {
              out.layout = s["layout"].get<std::string>();
            }
            if (s.contains("storage_class") && s["storage_class"].is_string()) {
              out.storage_class = s["storage_class"].get<std::string>();
            }
            if (s.contains("ec_k") && !s["ec_k"].is_null()) out.ec_k = s["ec_k"].get<int>();
            if (s.contains("ec_m") && !s["ec_m"].is_null()) out.ec_m = s["ec_m"].get<int>();
            if (s.contains("ec_codec") && s["ec_codec"].is_string()) {
              out.ec_codec = s["ec_codec"].get<std::string>();
            }
          };
          if (jr.contains("meta")) parse_spec(jr["meta"], r.meta);
          if (jr.contains("data")) parse_spec(jr["data"], r.data);
          fresh->push_back(std::move(r));
        }
      }
    }
  } catch (...) {
    // Keep serving the previous version rather than dropping every rule.
    std::lock_guard lock(st.layout_mu);
    if (st.layout_rules) return st.layout_rules;
  }
  std::shared_ptr<const std::vector<PosixLayoutRule>> published = std::move(fresh);
  std::lock_guard lock(st.layout_mu);
  st.layout_rules = published;
  return published;
}

PutLayout meta_layout_for_path(FsState& st, const std::string& path) {
  auto rules = refresh_layout_rules(st);
  const auto r = match_posix_layout_rule(*rules, st.volume, path);
  return r ? put_layout_from_spec(r->meta) : PutLayout{};
}

PutLayout data_layout_for_path(FsState& st, const std::string& path) {
  auto rules = refresh_layout_rules(st);
  const auto r = match_posix_layout_rule(*rules, st.volume, path);
  return r ? put_layout_from_spec(r->data) : PutLayout{};
}

PutLayout meta_layout_for_ino(FsState& st, uint64_t ino) {
  return meta_layout_for_path(st, path_of_ino(st, ino));
}

PutLayout data_layout_for_ino(FsState& st, uint64_t ino) {
  return data_layout_for_path(st, path_of_ino(st, ino));
}

bool layout_domains_differ(FsState& st, const std::string& path_a, const std::string& path_b) {
  auto rules = refresh_layout_rules(st);
  const auto ra = match_posix_layout_rule(*rules, st.volume, path_a);
  const auto rb = match_posix_layout_rule(*rules, st.volume, path_b);
  return layout_domain_key(ra) != layout_domain_key(rb);
}

}  // namespace posix
}  // namespace aios
