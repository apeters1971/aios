#include "object/object_layout.hpp"

#include "ec/codec_factory.hpp"

#include <cctype>

namespace aios {
namespace {

std::string lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool resolve_codec(int m, std::string codec, std::string& out, std::string& err) {
  if (codec.empty()) codec = (m == 1) ? "xor" : "isal";
  if (codec == "xor") {
    if (m != 1) {
      err = "ec_codec=xor requires ec_m=1";
      return false;
    }
  } else if (codec == "isal" || codec == "rs") {
    if (!isal_ec_available()) {
      err = "ec_codec=isal requires a build with ISA-L (AIOS_WITH_ISAL + libisal)";
      return false;
    }
    codec = "isal";
  } else {
    err = "ec_codec must be empty, 'xor', or 'isal'";
    return false;
  }
  out = std::move(codec);
  return true;
}

}  // namespace

bool resolve_object_layout(const Config& cfg, const LayoutRequest& req, ObjectLayout& out,
                           std::string& err) {
  std::string layout = req.layout.value_or(cfg.durability);
  if (layout.empty()) layout = "replica";
  layout = lower_copy(std::move(layout));

  if (layout == "replica") {
    const int n = cfg.replica_count;
    if (n < 1) {
      err = "replica_count must be >= 1";
      return false;
    }
    if (cfg.max_replica_count > 0 && n > cfg.max_replica_count) {
      err = "replica_count exceeds max_replica_count";
      return false;
    }
    out = ObjectLayout{};
    out.kind = ObjectLayout::Kind::Replica;
    out.n = n;
    return true;
  }

  if (layout != "ec") {
    err = "layout must be 'replica' or 'ec'";
    return false;
  }

  const int k = req.ec_k.value_or(cfg.ec_k);
  const int m = req.ec_m.value_or(cfg.ec_m);
  if (k < 1 || m < 1) {
    err = "ec_k and ec_m must be >= 1";
    return false;
  }
  if (cfg.max_ec_k > 0 && k > cfg.max_ec_k) {
    err = "ec_k exceeds max_ec_k";
    return false;
  }
  if (cfg.max_ec_m > 0 && m > cfg.max_ec_m) {
    err = "ec_m exceeds max_ec_m";
    return false;
  }

  std::string codec;
  if (!resolve_codec(m, req.ec_codec.value_or(cfg.ec_codec), codec, err)) return false;

  out = ObjectLayout{};
  out.kind = ObjectLayout::Kind::Ec;
  out.ec_k = k;
  out.ec_m = m;
  out.ec_codec = std::move(codec);
  out.n = k + m;
  if (cfg.max_replica_count > 0 && out.n > cfg.max_replica_count) {
    err = "k+m exceeds max_replica_count";
    return false;
  }
  return true;
}

void apply_layout_attrs(std::unordered_map<std::string, std::string>& attrs,
                        const ObjectLayout& layout) {
  attrs[kLayoutAttr] = layout.is_ec() ? "ec" : "replica";
  attrs[kLayoutNAttr] = std::to_string(layout.n);
}

LayoutRequest layout_request_from_headers(
    const std::unordered_map<std::string, std::string>& headers) {
  LayoutRequest req;
  auto get = [&](const char* name) -> std::string {
    auto it = headers.find(name);
    return it == headers.end() ? std::string{} : it->second;
  };
  const auto layout = get("x-aios-layout");
  if (!layout.empty()) req.layout = lower_copy(layout);
  const auto k = get("x-aios-ec-k");
  if (!k.empty()) {
    try {
      req.ec_k = std::stoi(k);
    } catch (...) {
      req.ec_k = -1;  // resolve will reject
    }
  }
  const auto m = get("x-aios-ec-m");
  if (!m.empty()) {
    try {
      req.ec_m = std::stoi(m);
    } catch (...) {
      req.ec_m = -1;
    }
  }
  const auto codec = get("x-aios-ec-codec");
  if (!codec.empty()) req.ec_codec = lower_copy(codec);
  return req;
}

LayoutRequest layout_request_replica() {
  LayoutRequest req;
  req.layout = "replica";
  return req;
}

int placement_n_for_attrs(const std::unordered_map<std::string, std::string>& attrs,
                          int default_n) {
  if (auto it = attrs.find(kLayoutNAttr); it != attrs.end()) {
    try {
      const int n = std::stoi(it->second);
      if (n > 0) return n;
    } catch (...) {
    }
  }
  if (auto meta = parse_ec_attrs(attrs)) return meta->k + meta->m;
  return default_n;
}

}  // namespace aios
