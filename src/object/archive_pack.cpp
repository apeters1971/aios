#include "object/archive_pack.hpp"

#include "cluster/place.hpp"
#include "object/archive_bag.hpp"
#include "object/archive_tape.hpp"
#include "object/object_io.hpp"
#include "object/object_layout.hpp"
#include "util/log.hpp"

#include <chrono>
#include <random>
#include <sstream>

namespace aios {
namespace {

const ArchiveRule* find_archive_rule_in(const std::vector<ArchiveRule>& rules,
                                        const std::string& oid) {
  const ArchiveRule* best = nullptr;
  for (const auto& rule : rules) {
    if (oid.size() < rule.prefix.size()) continue;
    if (oid.compare(0, rule.prefix.size(), rule.prefix) != 0) continue;
    if (!best || rule.prefix.size() > best->prefix.size()) best = &rule;
  }
  return best;
}

std::string make_bag_oid() {
  static thread_local std::mt19937_64 rng{
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
  std::ostringstream oss;
  oss << "archive/bag/" << std::hex << rng() << rng();
  return oss.str();
}

bool tip_age_ok(const ObjectInfo& info, int min_age_days) {
  if (min_age_days <= 0) return true;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const std::int64_t min_age_ms =
      static_cast<std::int64_t>(min_age_days) * 24ll * 3600ll * 1000ll;
  return (now - info.mtime_ms) >= min_age_ms;
}

bool seal_bag(const Config& cfg, const std::string& advertise, const ClusterMap& map,
              LocalStores& stores, const ArchiveRule& rule, std::vector<ArchiveMember>& members,
              ArchiveStats& stats) {
  if (members.empty()) return true;
  std::string err;
  std::vector<std::uint8_t> plain_bytes;
  if (!encode_archive_bag(members, plain_bytes, err)) {
    AIOS_LOG_WARN("archive encode: ", err);
    ++stats.failed;
    members.clear();
    return false;
  }
  // Decode plaintext to get member offsets (relative to AIAB body).
  ArchiveBag decoded;
  if (!decode_archive_bag(plain_bytes.data(), plain_bytes.size(), decoded, false, err)) {
    ++stats.failed;
    members.clear();
    return false;
  }
  BagTransformOpts xopts;
  xopts.compression = rule.bag_compression.empty() ? "none" : rule.bag_compression;
  xopts.compression_level =
      rule.bag_compression_level > 0 ? rule.bag_compression_level : cfg.compression_level;
  xopts.encryption = rule.bag_encryption.empty() ? "none" : rule.bag_encryption;
  std::vector<std::uint8_t> bag_bytes;
  std::unordered_map<std::string, std::string> xattrs;
  if (!transform_bag_for_storage(plain_bytes, xopts, cfg.bag_encryption_key, bag_bytes, xattrs,
                                 err)) {
    AIOS_LOG_WARN("archive bag transform: ", err);
    ++stats.failed;
    members.clear();
    return false;
  }
  const std::string bag_id = make_bag_oid();
  LayoutRequest req;
  req.layout = "replica";
  req.storage_class = rule.staging_class;
  ObjectLayout layout;
  if (!resolve_object_layout(cfg, bag_id, req, layout, err)) {
    AIOS_LOG_WARN("archive bag layout: ", err);
    ++stats.failed;
    members.clear();
    return false;
  }
  auto dest = place(bag_id, map, layout.n, rule.staging_class);
  if (dest.acting_set.empty() || dest.acting_set[0].node_id != cfg.node_id) {
    // Another node owns this bag oid; retry next tick with a new id.
    ++stats.failed;
    members.clear();
    return false;
  }
  std::unordered_map<std::string, std::string> bag_attrs;
  apply_layout_attrs(bag_attrs, layout);
  bag_attrs[kArchiveStateAttr] = kArchiveStateBagged;
  bag_attrs["aios.bag_members"] = std::to_string(members.size());
  for (const auto& [k, v] : xattrs) bag_attrs[k] = v;
  if (!install_replica_version(cfg, advertise, map, stores, dest, bag_id, bag_bytes, bag_attrs)) {
    ++stats.failed;
    members.clear();
    return false;
  }

  for (const auto& m : decoded.members) {
    const int sn = placement_n_for_attrs(m.attrs, map.replica_count);
    const std::string sc = storage_class_for_attrs(m.attrs, rule.from);
    auto mdest = place(m.oid, map, sn, sc);
    if (mdest.acting_set.empty() || mdest.acting_set[0].node_id != cfg.node_id) {
      ++stats.failed;
      continue;
    }
    auto stub_attrs = m.attrs;
    for (auto it = stub_attrs.begin(); it != stub_attrs.end();) {
      if (it->first.rfind("aios.ec.", 0) == 0) it = stub_attrs.erase(it);
      else ++it;
    }
    apply_frozen_stub_attrs(stub_attrs, bag_id, m.offset, m.length, m.sha256_hex);
    std::vector<std::uint8_t> empty;
    if (!install_replica_version(cfg, advertise, map, stores, mdest, m.oid, empty, stub_attrs)) {
      ++stats.failed;
      continue;
    }
    ++stats.packed;
  }

  if (rule.tape_sink == "external" || rule.tape_sink == "s3" || rule.tape_sink == "xrdcp") {
    // Mark on_tape; body stays staged until run_archive_drain copies out.
    bag_attrs[kArchiveStateAttr] = kArchiveStateOnTape;
    bag_attrs[kTapeSinkAttr] = rule.tape_sink;
    if (!rule.tape_root.empty()) bag_attrs[kTapeRootAttr] = rule.tape_root;
    if (!rule.tape_uri_prefix.empty()) bag_attrs[kTapeUriPrefixAttr] = rule.tape_uri_prefix;
    if (!rule.tape_bin.empty()) bag_attrs[kTapeBinAttr] = rule.tape_bin;
    if (!rule.tape_s3_endpoint.empty()) bag_attrs[kTapeS3EndpointAttr] = rule.tape_s3_endpoint;
    bag_attrs[kContentSha256Attr] = sha256_hex_bytes(bag_bytes.data(), bag_bytes.size());
    install_replica_version(cfg, advertise, map, stores, dest, bag_id, bag_bytes, bag_attrs);
    for (const auto& m : decoded.members) {
      std::string e2;
      for (const auto& path : stores.paths()) {
        auto* s = stores.get(path);
        if (!s) continue;
        auto info = s->stat(m.oid, e2);
        if (!info || info->is_delete) continue;
        auto attrs = s->list_attrs(m.oid, e2);
        if (!attrs_are_frozen(attrs)) continue;
        attrs[kArchiveStateAttr] = kArchiveStateOnTape;
        const int sn = placement_n_for_attrs(attrs, map.replica_count);
        const std::string sc = storage_class_for_attrs(attrs, rule.from);
        auto mdest = place(m.oid, map, sn, sc);
        if (mdest.acting_set.empty() || mdest.acting_set[0].node_id != cfg.node_id) break;
        std::vector<std::uint8_t> empty;
        install_replica_version(cfg, advertise, map, stores, mdest, m.oid, empty, attrs);
        break;
      }
    }
  }

  ++stats.bags_sealed;
  members.clear();
  return true;
}

}  // namespace

bool read_frozen_member(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                        LocalStores& stores,
                        const std::unordered_map<std::string, std::string>& stub_attrs,
                        std::vector<std::uint8_t>& out, std::string& err) {
  auto bid = stub_attrs.find(kBagIdAttr);
  auto boff = stub_attrs.find(kBagOffsetAttr);
  auto blen = stub_attrs.find(kBagLengthAttr);
  if (bid == stub_attrs.end() || boff == stub_attrs.end() || blen == stub_attrs.end()) {
    err = "incomplete freeze attrs";
    return false;
  }
  const std::string state = archive_state_for_attrs(stub_attrs);
  if (state == kArchiveStateOnTape || state == kArchiveStateRestoring) {
    err = "restoring";
    return false;
  }
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  try {
    offset = std::stoull(boff->second);
    length = std::stoull(blen->second);
  } catch (...) {
    err = "bad bag extents";
    return false;
  }

  // Load bag tip attrs to find placement class.
  std::unordered_map<std::string, std::string> bag_attrs;
  std::string e2;
  for (const auto& path : stores.paths()) {
    auto* s = stores.get(path);
    if (!s) continue;
    auto info = s->stat(bid->second, e2);
    if (!info || info->is_delete) continue;
    bag_attrs = s->list_attrs(bid->second, e2);
    break;
  }
  const std::string sc = storage_class_for_attrs(bag_attrs, cfg.default_storage_class);
  const int n = placement_n_for_attrs(bag_attrs, map.replica_count);
  auto placement = place(bid->second, map, n, sc);
  std::vector<std::uint8_t> stored;
  if (!load_object_bytes(cfg, advertise, map, stores, placement, bid->second, stored, bag_attrs)) {
    err = "bag not available";
    return false;
  }
  std::vector<std::uint8_t> bag;
  if (!untransform_bag_from_storage(stored.data(), stored.size(), cfg.bag_encryption_key, bag,
                                    err)) {
    return false;
  }
  if (offset + length > bag.size()) {
    err = "bag slice out of range";
    return false;
  }
  out.assign(bag.begin() + static_cast<std::ptrdiff_t>(offset),
             bag.begin() + static_cast<std::ptrdiff_t>(offset + length));
  auto expect = stub_attrs.find(kContentSha256Attr);
  if (expect != stub_attrs.end() && !expect->second.empty()) {
    if (sha256_hex_bytes(out.data(), out.size()) != expect->second) {
      err = "bag member checksum mismatch";
      return false;
    }
  }
  return true;
}

bool recall_archived_oid(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                         LocalStores& stores, const std::string& oid, std::string& err) {
  std::unordered_map<std::string, std::string> attrs;
  for (const auto& path : stores.paths()) {
    auto* s = stores.get(path);
    if (!s) continue;
    auto info = s->stat(oid, err);
    if (!info || info->is_delete) continue;
    attrs = s->list_attrs(oid, err);
    break;
  }
  if (!attrs_are_frozen(attrs)) {
    err = "oid is not frozen";
    return false;
  }

  auto bid = attrs.find(kBagIdAttr);
  if (bid == attrs.end() || bid->second.empty()) {
    err = "missing bag id";
    return false;
  }

  const std::string prior_state = archive_state_for_attrs(attrs);
  if (prior_state == kArchiveStateOnTape || prior_state == kArchiveStateRestoring) {
    // Persist restoring on the stub while we fetch the bag.
    attrs[kArchiveStateAttr] = kArchiveStateRestoring;
    const int sn = placement_n_for_attrs(attrs, map.replica_count);
    const std::string sc = storage_class_for_attrs(attrs, cfg.default_storage_class);
    auto mdest = place(oid, map, sn, sc);
    if (!mdest.acting_set.empty() && mdest.acting_set[0].node_id == cfg.node_id) {
      std::vector<std::uint8_t> empty;
      install_replica_version(cfg, advertise, map, stores, mdest, oid, empty, attrs);
    }
  }

  std::unordered_map<std::string, std::string> bag_attrs;
  if (!ensure_bag_on_staging(cfg, advertise, map, stores, bid->second, bag_attrs, err)) {
    if (prior_state == kArchiveStateOnTape || prior_state == kArchiveStateRestoring) {
      err = "restoring";
    }
    return false;
  }

  std::vector<std::uint8_t> data;
  // Temporarily treat as bagged so read_frozen_member allows the slice.
  attrs[kArchiveStateAttr] = kArchiveStateBagged;
  if (!read_frozen_member(cfg, advertise, map, stores, attrs, data, err)) {
    if (prior_state == kArchiveStateOnTape || prior_state == kArchiveStateRestoring) {
      err = "restoring";
    }
    return false;
  }
  clear_frozen_stub_attrs(attrs);
  const int n = placement_n_for_attrs(attrs, map.replica_count);
  const std::string sc = storage_class_for_attrs(attrs, cfg.default_storage_class);
  auto dest = place(oid, map, n, sc);
  if (dest.acting_set.empty() || dest.acting_set[0].node_id != cfg.node_id) {
    err = "not primary for oid";
    return false;
  }
  if (!install_replica_version(cfg, advertise, map, stores, dest, oid, data, attrs)) {
    err = "rehydrate failed";
    return false;
  }
  return true;
}

ArchiveStats run_archive_with_rules(const Config& cfg, const std::string& advertise,
                                    const ClusterMap& map, LocalStores& stores,
                                    std::size_t max_oids_per_store,
                                    const std::vector<ArchiveRule>& rules) {
  ArchiveStats stats;
  if (rules.empty() || map.targets.empty()) return stats;

  std::vector<std::vector<ArchiveMember>> open(rules.size());
  std::vector<std::uint64_t> open_bytes(rules.size(), 0);

  for (const auto& path : stores.paths()) {
    auto* store = stores.get(path);
    if (!store) continue;
    std::string err;
    auto oids = store->list_oids(max_oids_per_store, err);
    if (!err.empty()) continue;
    for (const auto& oid : oids) {
      ++stats.oids_scanned;
      if (is_archive_bag_oid(oid)) continue;
      const ArchiveRule* rule = find_archive_rule_in(rules, oid);
      if (!rule) continue;
      ++stats.matched;

      auto info = store->stat(oid, err);
      if (!info || info->is_delete) continue;
      auto attrs = store->list_attrs(oid, err);
      if (attrs_are_frozen(attrs)) continue;
      if (storage_class_for_attrs(attrs, rule->from) != rule->from) continue;
      if (!tip_age_ok(*info, rule->min_age_days)) continue;

      const int sn = placement_n_for_attrs(attrs, map.replica_count);
      auto src = place(oid, map, sn, rule->from);
      if (src.acting_set.empty() || src.acting_set[0].node_id != cfg.node_id) continue;

      std::vector<std::uint8_t> data;
      std::unordered_map<std::string, std::string> loaded;
      if (!load_object_bytes(cfg, advertise, map, stores, src, oid, data, loaded)) {
        ++stats.failed;
        continue;
      }
      if (!loaded.empty()) attrs = loaded;

      std::size_t ri = 0;
      for (; ri < rules.size(); ++ri) {
        if (&rules[ri] == rule) break;
      }
      if (ri >= rules.size()) continue;

      ArchiveMember m;
      m.oid = oid;
      m.attrs = attrs;
      m.data = std::move(data);
      m.sha256_hex = sha256_hex_bytes(m.data.data(), m.data.size());
      m.length = m.data.size();
      open_bytes[ri] += m.length;
      open[ri].push_back(std::move(m));

      const bool size_ready = open_bytes[ri] >= rule->min_bag_bytes;
      const bool count_ready =
          rule->max_members > 0 && open[ri].size() >= static_cast<std::size_t>(rule->max_members);
      const bool over_max =
          rule->max_bag_bytes > 0 && open_bytes[ri] >= rule->max_bag_bytes;
      if (size_ready || count_ready || over_max) {
        seal_bag(cfg, advertise, map, stores, *rule, open[ri], stats);
        open_bytes[ri] = 0;
      }
    }
  }

  for (std::size_t ri = 0; ri < rules.size(); ++ri) {
    const auto& rule = rules[ri];
    if (open[ri].empty()) continue;
    if (open_bytes[ri] >= rule.min_bag_bytes || rule.max_open_ms == 0) {
      seal_bag(cfg, advertise, map, stores, rule, open[ri], stats);
    }
  }
  return stats;
}

ArchiveStats run_archive(const Config& cfg, const std::string& advertise, const ClusterMap& map,
                         LocalStores& stores, std::size_t max_oids_per_store) {
  return run_archive_with_rules(cfg, advertise, map, stores, max_oids_per_store,
                                cfg.archive_rules);
}

}  // namespace aios
