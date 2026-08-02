#include "object/object_service.hpp"

#include "cluster/place.hpp"
#include "ec/codec_factory.hpp"
#include "ec/ec_attrs.hpp"
#include "net/object_client.hpp"
#include "object/object_layout.hpp"
#include "util/base64.hpp"
#include "util/crc32c.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <random>
#include <span>
#include <sstream>
#include <unordered_set>

namespace aios {

ObjectService::ObjectService(Config cfg, ClusterMap& map, LocalStores& stores)
    : cfg_(std::move(cfg)), map_(map), stores_(stores) {}

void ObjectService::set_advertise(std::string advertise) {
  advertise_ = std::move(advertise);
}

Frame ObjectService::reply_ok(std::uint64_t epoch) const {
  Frame f;
  f.type = MsgType::ObjectReply;
  f.body = {{"ok", true}, {"epoch", epoch}};
  return f;
}

Frame ObjectService::reply_err(std::uint64_t epoch, const std::string& code,
                               const std::string& error) const {
  Frame f;
  f.type = MsgType::ObjectReply;
  f.body = {{"ok", false}, {"epoch", epoch}, {"code", code}, {"error", error}};
  return f;
}

ApiResult ObjectService::fail(const std::string& code, const std::string& error) const {
  ApiResult r;
  r.ok = false;
  r.code = code;
  r.error = error;
  r.epoch = map_.epoch;
  return r;
}

int ObjectService::quorum_need(const Placement& placement) const {
  const int configured = cfg_.write_quorum > 0 ? cfg_.write_quorum : map_.replica_count;
  return std::min(configured, static_cast<int>(placement.acting_set.size()));
}

bool ObjectService::epoch_ok(std::uint64_t req_epoch, Frame& err_out) const {
  if (req_epoch != 0 && req_epoch != map_.epoch) {
    err_out = reply_err(map_.epoch, "epoch_mismatch", "cluster map epoch mismatch");
    err_out.body["cluster_map"] = map_.to_json();
    return false;
  }
  return true;
}

ObjectStore* ObjectService::primary_store(const Placement& p, std::string& err) {
  if (p.acting_set.empty()) {
    err = "no targets";
    return nullptr;
  }
  const auto& t = p.acting_set[0];
  if (t.node_id != cfg_.node_id) {
    err = "not primary node";
    return nullptr;
  }
  auto* store = stores_.get(t.aios_path);
  if (!store) {
    err = "no local store for primary path";
    return nullptr;
  }
  return store;
}

PrecondResult ObjectService::check_preds_on(ObjectStore* store, const std::string& oid,
                                           const std::vector<AttrPrecondition>& preds,
                                           std::string& err) {
  if (!store) {
    err = "no store";
    return PrecondResult::NotFound;
  }
  return store->check_preconditions(oid, preds, err);
}

bool ObjectService::local_install(const std::string& aios_path, const PreparedVersion& v,
                                 const std::uint8_t* data, std::size_t len,
                                 const std::unordered_map<std::string, std::string>& attrs,
                                 std::string& err) {
  auto* store = stores_.get(aios_path);
  if (!store) {
    err = "no local store for " + aios_path;
    return false;
  }
  return store->install_version(v, data, len, attrs, err);
}

bool ObjectService::local_install_file(
    const std::string& aios_path, const PreparedVersion& v, const std::string& abs_body_path,
    const std::unordered_map<std::string, std::string>& attrs, std::string& err) {
  auto* store = stores_.get(aios_path);
  if (!store) {
    err = "no local store for " + aios_path;
    return false;
  }
  PreparedVersion pv = v;
  std::string rel;
  if (!v.is_delete && v.redirect_oid.empty() && v.size > 0) {
    // Always copy — source path is shared across replica installs / primary body.
    std::string staging;
    if (!store->create_staging_file(v.oid, staging, err)) return false;
    {
      std::ifstream in(abs_body_path, std::ios::binary);
      std::ofstream out(staging, std::ios::binary | std::ios::trunc);
      if (!in || !out) {
        err = "copy staging failed";
        return false;
      }
      out << in.rdbuf();
    }
    if (!store->place_staging_as_version(v.oid, v.seq, staging, rel, err)) return false;
    pv.fs_path = rel;
    pv.inline_body = false;
  }
  return store->install_version(pv, nullptr, 0, attrs, err);
}

bool ObjectService::local_publish(const std::string& aios_path, const std::string& oid,
                                 std::uint64_t seq, std::string& err) {
  auto* store = stores_.get(aios_path);
  if (!store) {
    err = "no local store for " + aios_path;
    return false;
  }
  return store->publish_tip(oid, seq, err);
}

bool ObjectService::local_abort(const std::string& aios_path, const std::string& oid,
                               std::uint64_t seq, std::string& err) {
  auto* store = stores_.get(aios_path);
  if (!store) {
    err = "no local store for " + aios_path;
    return false;
  }
  return store->abort_version(oid, seq, err);
}

int ObjectService::replicate_install(
    const Placement& placement, const PreparedVersion& v, const std::uint8_t* data,
    std::size_t len, const std::unordered_map<std::string, std::string>& attrs,
    const std::string& abs_body_path) {
  const bool use_file =
      !v.inline_body && !v.is_delete && v.redirect_oid.empty() && v.size > 0 &&
      (!abs_body_path.empty() || (data == nullptr));
  // Prefer file streaming when body is large or only a path is available.
  const bool file_stream =
      use_file && (abs_body_path.empty() == false) &&
      (data == nullptr || len > 4u * 1024u * 1024u || v.size > 4u * 1024u * 1024u);

  int ok = 0;
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    const auto& t = placement.acting_set[i];
    if (t.node_id == cfg_.node_id) {
      std::string err;
      bool done = false;
      if (file_stream) {
        done = local_install_file(t.aios_path, v, abs_body_path, attrs, err);
      } else {
        done = local_install(t.aios_path, v, data, len, attrs, err);
      }
      if (done) ++ok;
      else
        AIOS_LOG_WARN("local replica install failed ", t.aios_path, " oid=", v.oid, ": ",
                      err);
      continue;
    }
    ObjectRpcResult r;
    if (file_stream) {
      r = object_install_file_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                     cfg_.auth_skew_ms, placement.epoch, t.aios_path, v,
                                     attrs, abs_body_path);
    } else {
      r = object_install_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                cfg_.auth_skew_ms, placement.epoch, t.aios_path, v, data,
                                len, attrs);
    }
    if (r.ok) ++ok;
    else
      AIOS_LOG_WARN("remote replica install failed ", t.addr, ": ", r.error);
  }
  return ok;
}

int ObjectService::replicate_publish(const Placement& placement, const std::string& oid,
                                     std::uint64_t seq) {
  int ok = 0;
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    const auto& t = placement.acting_set[i];
    if (t.node_id == cfg_.node_id) {
      std::string err;
      if (local_publish(t.aios_path, oid, seq, err)) ++ok;
      else
        AIOS_LOG_WARN("local replica publish failed ", t.aios_path, ": ", err);
      continue;
    }
    auto r = object_publish_tip_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                       cfg_.auth_skew_ms, placement.epoch, t.aios_path, oid,
                                       seq);
    if (r.ok) ++ok;
    else
      AIOS_LOG_WARN("remote replica publish failed ", t.addr, ": ", r.error);
  }
  return ok;
}

void ObjectService::replicate_abort(const Placement& placement, const std::string& oid,
                                    std::uint64_t seq) {
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    const auto& t = placement.acting_set[i];
    if (t.node_id == cfg_.node_id) {
      std::string err;
      local_abort(t.aios_path, oid, seq, err);
      continue;
    }
    object_abort_version_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                cfg_.auth_skew_ms, placement.epoch, t.aios_path, oid, seq);
  }
}

ApiResult ObjectService::install_prepared(
    ObjectStore* store, const Placement& placement, PreparedVersion& pv,
    const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs,
    const std::string& abs_body_path) {
  std::string body_path = abs_body_path;
  if (body_path.empty() && !pv.inline_body && !pv.is_delete && pv.redirect_oid.empty() &&
      pv.size > 0) {
    std::string err;
    if (auto p = store->fs_body_path(pv.oid, pv.seq, err)) body_path = *p;
  }
  const int total_ok = 1 + replicate_install(placement, pv, data, len, attrs, body_path);
  if (total_ok < quorum_need(placement)) {
    std::string aerr;
    store->abort_version(pv.oid, pv.seq, aerr);
    replicate_abort(placement, pv.oid, pv.seq);
    return fail("quorum_failed", "quorum failed");
  }
  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  r.replicas = total_ok;
  r.placement = placement;
  r.attrs = attrs;
  r.info = ObjectInfo{};
  r.info->oid = pv.oid;
  r.info->seq = pv.seq;
  r.info->size = pv.size;
  r.info->crc32c = pv.crc32c;
  r.info->crc32c_known = true;
  r.info->is_delete = pv.is_delete;
  r.info->redirect_oid = pv.redirect_oid;
  r.info->inline_body = pv.inline_body;
  r.info->fs_path = pv.fs_path;
  if (!pv.redirect_oid.empty()) {
    r.redirect_oid = pv.redirect_oid;
    r.code = "redirect";
  }
  return r;
}

ApiResult ObjectService::commit_prepared(
    ObjectStore* store, const Placement& placement, PreparedVersion& pv,
    const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs,
    const std::string& abs_body_path) {
  auto r = install_prepared(store, placement, pv, data, len, attrs, abs_body_path);
  if (!r.ok) return r;
  std::string err;
  if (!store->publish_tip(pv.oid, pv.seq, err)) {
    store->abort_version(pv.oid, pv.seq, err);
    replicate_abort(placement, pv.oid, pv.seq);
    return fail("store_error", err);
  }
  replicate_publish(placement, pv.oid, pv.seq);

  if (auto st = store->stat(pv.oid, err)) {
    r.info = st;
    if (!st->redirect_oid.empty()) {
      r.redirect_oid = st->redirect_oid;
      r.code = "redirect";
    }
  } else if (!pv.redirect_oid.empty()) {
    r.redirect_oid = pv.redirect_oid;
    r.code = "redirect";
  }
  return r;
}

Frame ObjectService::handle(const Frame& req) {
  std::lock_guard lock(mu_);
  switch (req.type) {
    case MsgType::ObjectPut:
      return handle_put(req.body);
    case MsgType::ObjectPutRange:
      return handle_put_range(req);
    case MsgType::ObjectGet:
      return handle_get(req.body);
    case MsgType::ObjectDel:
      return handle_del(req.body);
    case MsgType::ObjectStat:
      return handle_stat(req.body);
    case MsgType::ObjectPublishTip:
      return handle_publish_tip(req.body);
    case MsgType::ObjectAbortVersion:
      return handle_abort_version(req.body);
    case MsgType::ObjectListVersions:
      return handle_list_versions(req.body);
    case MsgType::ObjectPurgeVersions:
      return handle_purge_versions(req.body);
    case MsgType::ObjectStageBegin:
      return handle_stage_begin(req.body);
    case MsgType::ObjectStageData:
      return handle_stage_data(req);
    case MsgType::ObjectStageCommit:
      return handle_stage_commit(req.body);
    case MsgType::ObjectList:
      return handle_list(req.body);
    default:
      return reply_err(map_.epoch, "bad_type", "unsupported object message");
  }
}

static std::unordered_map<std::string, std::string> parse_attrs_json(
    const nlohmann::json& body) {
  std::unordered_map<std::string, std::string> attrs;
  if (body.contains("attrs") && body["attrs"].is_object()) {
    for (auto it = body["attrs"].begin(); it != body["attrs"].end(); ++it) {
      if (it.value().is_string()) attrs[it.key()] = it.value().get<std::string>();
    }
  }
  return attrs;
}

Frame ObjectService::handle_put(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;

  const std::string oid = body.value("oid", "");
  const std::string aios_path = body.value("aios_path", "");
  const std::string role = body.value("role", "primary");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(map_.epoch, "bad_request", "oid and aios_path required");
  }

  std::vector<std::uint8_t> data;
  const bool is_delete = body.value("is_delete", false);
  const std::string redirect_oid = body.value("redirect", "");
  const bool is_redirect = !redirect_oid.empty();
  if (!is_delete && !is_redirect) {
    std::string derr;
    if (!body.contains("data_b64") || !body["data_b64"].is_string() ||
        !base64_decode(body["data_b64"].get<std::string>(), data, derr)) {
      return reply_err(map_.epoch, "bad_request", "invalid data_b64: " + derr);
    }
  }

  auto attrs = parse_attrs_json(body);

  std::optional<std::uint32_t> expected_crc;
  if (body.contains("crc32c") && !body["crc32c"].is_null()) {
    expected_crc = body.value("crc32c", 0u);
    if (!is_delete && !is_redirect && crc32c(data.data(), data.size()) != *expected_crc) {
      return reply_err(map_.epoch, "crc_mismatch", "crc32c mismatch");
    }
  }

  const auto placement = place(oid, map_);
  if (placement.acting_set.empty()) {
    return reply_err(map_.epoch, "no_targets", "no storage targets in cluster map");
  }

  // Replica install of a prepared version (seq present).
  if (role == "replica" && body.contains("seq")) {
    if (!in_acting_set(oid, map_, cfg_.node_id, aios_path)) {
      return reply_err(map_.epoch, "not_replica", "not in acting set for oid");
    }
    PreparedVersion v;
    v.oid = oid;
    v.seq = body.value("seq", static_cast<std::uint64_t>(0));
    v.prev_tip = body.value("base_seq", static_cast<std::uint64_t>(0));
    v.size = (is_delete || is_redirect)
                 ? 0
                 : body.value("size", static_cast<std::uint64_t>(data.size()));
    if (v.size == 0 && !is_delete && !is_redirect) v.size = data.size();
    v.crc32c = expected_crc.value_or(
        (is_delete || is_redirect) ? crc32c(nullptr, 0) : crc32c(data.data(), data.size()));
    v.inline_body = body.value("inline_body", false);
    v.fs_path = body.value("fs_path", "");
    v.is_delete = is_delete;
    v.redirect_oid = redirect_oid;
    std::string err;
    if (!local_install(aios_path, v, data.data(), data.size(), attrs, err)) {
      return reply_err(map_.epoch, "store_error", err);
    }
    auto f = reply_ok(map_.epoch);
    f.body["seq"] = v.seq;
    return f;
  }

  if (role == "replica") {
    // Legacy full put (publish immediately) — kept for repair tooling.
    if (!in_acting_set(oid, map_, cfg_.node_id, aios_path)) {
      return reply_err(map_.epoch, "not_replica", "not in acting set for oid");
    }
    auto* store = stores_.get(aios_path);
    if (!store) return reply_err(map_.epoch, "store_error", "no local store");
    std::string err;
    if (is_redirect) {
      if (!store->put_redirect(oid, redirect_oid, attrs, true, nullptr, err)) {
        return reply_err(map_.epoch, "store_error", err);
      }
    } else if (!store->put(oid, data.data(), data.size(), attrs, true, expected_crc, err)) {
      return reply_err(map_.epoch, "store_error", err);
    }
    return reply_ok(map_.epoch);
  }

  // Full primary PUT with layout (replica or EC). Redirects keep the legacy path.
  if (!is_delete && !is_redirect) {
    const LayoutRequest layout_req = layout_request_from_json(body);
    const bool do_publish = body.value("publish", true);
    if (do_publish) {
      auto r = api_put(oid, data.data(), data.size(), attrs, true, {}, expected_crc, layout_req);
      if (!r.ok) {
        auto f = reply_err(map_.epoch, r.code, r.error);
        if (r.code == "not_primary") {
          f.body["acting_set"] = nlohmann::json::array();
          for (const auto& t : r.placement.acting_set) {
            f.body["acting_set"].push_back(
                {{"node_id", t.node_id}, {"addr", t.addr}, {"aios_path", t.aios_path}});
          }
        }
        return f;
      }
      auto f = reply_ok(map_.epoch);
      f.body["replicas"] = r.replicas;
      if (r.info) f.body["seq"] = r.info->seq;
      f.body["published"] = true;
      return f;
    }
    // prepare-only (publish=false): replica layout only for now.
    ObjectLayout layout;
    std::string lerr;
    if (!resolve_object_layout(cfg_, layout_req, layout, lerr)) {
      return reply_err(map_.epoch, "bad_request", lerr);
    }
    if (layout.is_ec()) {
      return reply_err(map_.epoch, "bad_request",
                       "ec layout not supported for unpublished ObjectPut");
    }
    apply_layout_attrs(attrs, layout);
  }

  if (!is_primary_for(oid, map_, cfg_.node_id, aios_path)) {
    auto f = reply_err(map_.epoch, "not_primary", "this node/target is not primary");
    f.body["acting_set"] = nlohmann::json::array();
    for (const auto& t : placement.acting_set) {
      f.body["acting_set"].push_back(
          {{"node_id", t.node_id}, {"addr", t.addr}, {"aios_path", t.aios_path}});
    }
    return f;
  }

  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(map_.epoch, "store_error", "no local store");
  std::string err;
  PreparedVersion pv;
  if (is_redirect) {
    if (!store->prepare_redirect(oid, redirect_oid, attrs, true, pv, err)) {
      return reply_err(map_.epoch, "store_error", err);
    }
  } else if (!store->prepare_put(oid, data.data(), data.size(), attrs, true, expected_crc, pv,
                                 err)) {
    if (err == "crc32c mismatch") return reply_err(map_.epoch, "crc_mismatch", err);
    return reply_err(map_.epoch, "store_error", err);
  }
  const bool do_publish = body.value("publish", true);
  ApiResult r = do_publish ? commit_prepared(store, placement, pv, data.data(), data.size(), attrs)
                           : install_prepared(store, placement, pv, data.data(), data.size(),
                                              attrs);
  if (!r.ok) return reply_err(map_.epoch, r.code, r.error);
  auto f = reply_ok(map_.epoch);
  f.body["replicas"] = r.replicas;
  f.body["seq"] = pv.seq;
  f.body["prev_tip"] = pv.prev_tip;
  f.body["published"] = do_publish;
  if (!pv.redirect_oid.empty()) f.body["redirect"] = pv.redirect_oid;
  return f;
}

Frame ObjectService::handle_put_range(const Frame& req) {
  Frame errf;
  const auto& body = req.body;
  if (!epoch_ok(body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;

  const std::string oid = body.value("oid", "");
  const std::string aios_path = body.value("aios_path", "");
  const std::string role = body.value("role", "primary");
  const auto offset = body.value("offset", static_cast<std::uint64_t>(0));
  const bool replace_attrs = body.value("replace_attrs", false);
  if (oid.empty() || aios_path.empty()) {
    return reply_err(map_.epoch, "bad_request", "oid and aios_path required");
  }

  const auto* data = req.raw.data();
  const auto len = req.raw.size();
  auto attrs = parse_attrs_json(body);

  if (body.contains("range_crc32c") && !body["range_crc32c"].is_null()) {
    const auto expect = body.value("range_crc32c", 0u);
    if (crc32c(data, len) != expect) {
      return reply_err(map_.epoch, "crc_mismatch", "range crc32c mismatch");
    }
  }

  if (role == "replica") {
    // Range replicas are installed as full versions via ObjectPut+seq.
    return reply_err(map_.epoch, "bad_request",
                     "use ObjectPut with seq to install prepared range versions");
  }

  const LayoutRequest layout_req = layout_request_from_json(body);
  auto r = api_put_range(oid, offset, data, len, attrs, replace_attrs, {}, layout_req);
  if (!r.ok) return reply_err(map_.epoch, r.code, r.error);
  auto f = reply_ok(map_.epoch);
  f.body["replicas"] = r.replicas;
  if (r.info) f.body["seq"] = r.info->seq;
  return f;
}

Frame ObjectService::handle_get(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;
  const std::string oid = body.value("oid", "");
  const std::string aios_path = body.value("aios_path", "");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(map_.epoch, "bad_request", "oid and aios_path required");
  }
  if (!map_.targets.empty() && !in_acting_set(oid, map_, cfg_.node_id, aios_path)) {
    return reply_err(map_.epoch, "not_replica", "not in acting set for oid");
  }
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(map_.epoch, "store_error", "no local store");
  std::optional<std::uint64_t> seq;
  if (body.contains("seq") && !body["seq"].is_null()) {
    seq = body.value("seq", static_cast<std::uint64_t>(0));
  }
  std::string err;
  auto st = store->stat(oid, seq, err);
  if (!st) return reply_err(map_.epoch, "not_found", err);
  if (!st->redirect_oid.empty()) {
    auto f = reply_ok(map_.epoch);
    f.body["redirect"] = st->redirect_oid;
    f.body["seq"] = st->seq;
    f.body["code"] = "redirect";
    return f;
  }
  auto f = reply_ok(map_.epoch);
  f.body["seq"] = st->seq;
  f.body["size"] = st->size;
  if (st->crc32c_known) f.body["crc32c"] = st->crc32c;

  // Optional ranged raw get (avoids base64 / full-object buffer on wire).
  if (body.contains("offset")) {
    const auto offset = body.value("offset", static_cast<std::uint64_t>(0));
    const auto len = body.value("length", static_cast<std::uint64_t>(0));
    if (len == 0 || len > kMaxBodySize) {
      return reply_err(map_.epoch, "bad_request", "invalid get length");
    }
    auto data = store->get_range(oid, seq, offset, static_cast<std::size_t>(len), err);
    if (!data) {
      if (err == "range unsatisfiable") {
        return reply_err(map_.epoch, "range_unsatisfiable", err);
      }
      return reply_err(map_.epoch, "not_found", err);
    }
    f.body["offset"] = offset;
    f.body["length"] = data->size();
    f.flags |= kFlagRawBody;
    f.raw = std::move(*data);
    return f;
  }

  auto data = store->get(oid, seq, err);
  if (!data) return reply_err(map_.epoch, "not_found", err);
  f.body["data_b64"] = base64_encode(*data);
  f.body["size"] = data->size();
  return f;
}

Frame ObjectService::handle_del(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;
  const std::string oid = body.value("oid", "");
  const std::string aios_path = body.value("aios_path", "");
  const std::string role = body.value("role", "primary");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(map_.epoch, "bad_request", "oid and aios_path required");
  }
  const auto placement = place(oid, map_);
  if (placement.acting_set.empty()) {
    return reply_err(map_.epoch, "no_targets", "no storage targets");
  }

  // Replica install of delete-marker version.
  if (role == "replica" && body.contains("seq")) {
    if (!in_acting_set(oid, map_, cfg_.node_id, aios_path)) {
      return reply_err(map_.epoch, "not_replica", "not in acting set");
    }
    PreparedVersion v;
    v.oid = oid;
    v.seq = body.value("seq", static_cast<std::uint64_t>(0));
    v.prev_tip = body.value("base_seq", static_cast<std::uint64_t>(0));
    v.is_delete = true;
    v.crc32c = crc32c(nullptr, 0);
    std::string err;
    if (!local_install(aios_path, v, nullptr, 0, {}, err)) {
      return reply_err(map_.epoch, "store_error", err);
    }
    return reply_ok(map_.epoch);
  }

  if (role == "replica") {
    if (!in_acting_set(oid, map_, cfg_.node_id, aios_path)) {
      return reply_err(map_.epoch, "not_replica", "not in acting set");
    }
    auto* store = stores_.get(aios_path);
    if (!store) return reply_err(map_.epoch, "store_error", "no local store");
    std::string err;
    if (!store->del(oid, err) && err != "object not found") {
      return reply_err(map_.epoch, "store_error", err);
    }
    return reply_ok(map_.epoch);
  }

  if (!is_primary_for(oid, map_, cfg_.node_id, aios_path)) {
    return reply_err(map_.epoch, "not_primary", "not primary");
  }
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(map_.epoch, "store_error", "no local store");
  std::string err;
  PreparedVersion pv;
  if (!store->prepare_delete(oid, pv, err)) {
    if (err == "object not found") return reply_err(map_.epoch, "not_found", err);
    return reply_err(map_.epoch, "store_error", err);
  }
  const bool do_publish = body.value("publish", true);
  ApiResult r = do_publish ? commit_prepared(store, placement, pv, nullptr, 0, {})
                           : install_prepared(store, placement, pv, nullptr, 0, {});
  if (!r.ok) return reply_err(map_.epoch, r.code, r.error);
  auto f = reply_ok(map_.epoch);
  f.body["replicas"] = r.replicas;
  f.body["seq"] = pv.seq;
  f.body["prev_tip"] = pv.prev_tip;
  f.body["published"] = do_publish;
  return f;
}

Frame ObjectService::handle_stat(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;
  const std::string oid = body.value("oid", "");
  const std::string aios_path = body.value("aios_path", "");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(map_.epoch, "bad_request", "oid and aios_path required");
  }
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(map_.epoch, "store_error", "no local store");
  std::optional<std::uint64_t> seq;
  if (body.contains("seq") && !body["seq"].is_null()) {
    seq = body.value("seq", static_cast<std::uint64_t>(0));
  }
  std::string err;
  auto info = store->stat(oid, seq, err);
  if (!info) return reply_err(map_.epoch, "not_found", err);
  auto f = reply_ok(map_.epoch);
  f.body["size"] = info->size;
  f.body["mtime_ms"] = info->mtime_ms;
  f.body["ctime_ms"] = info->ctime_ms;
  f.body["shard"] = info->shard;
  f.body["inline_body"] = info->inline_body;
  f.body["seq"] = info->seq;
  f.body["is_delete"] = info->is_delete;
  if (!info->redirect_oid.empty()) f.body["redirect"] = info->redirect_oid;
  if (info->crc32c_known) f.body["crc32c"] = info->crc32c;
  nlohmann::json attrs_j = nlohmann::json::object();
  for (const auto& [k, v] : store->list_attrs(oid, err)) attrs_j[k] = v;
  f.body["attrs"] = std::move(attrs_j);
  return f;
}

Frame ObjectService::handle_publish_tip(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;
  const std::string oid = body.value("oid", "");
  const std::string aios_path = body.value("aios_path", "");
  const auto seq = body.value("seq", static_cast<std::uint64_t>(0));
  const std::string role = body.value("role", "replica");
  if (oid.empty() || aios_path.empty() || seq == 0) {
    return reply_err(map_.epoch, "bad_request", "oid, aios_path, seq required");
  }
  if (role == "primary") {
    auto r = api_publish_version(oid, seq);
    if (!r.ok) return reply_err(map_.epoch, r.code, r.error);
    return reply_ok(map_.epoch);
  }
  if (!in_acting_set(oid, map_, cfg_.node_id, aios_path)) {
    return reply_err(map_.epoch, "not_replica", "not in acting set");
  }
  std::string err;
  if (!local_publish(aios_path, oid, seq, err)) {
    return reply_err(map_.epoch, "store_error", err);
  }
  return reply_ok(map_.epoch);
}

Frame ObjectService::handle_abort_version(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;
  const std::string oid = body.value("oid", "");
  const std::string aios_path = body.value("aios_path", "");
  const auto seq = body.value("seq", static_cast<std::uint64_t>(0));
  const std::string role = body.value("role", "replica");
  if (oid.empty() || aios_path.empty() || seq == 0) {
    return reply_err(map_.epoch, "bad_request", "oid, aios_path, seq required");
  }
  if (role == "primary") {
    auto r = api_abort_prepared(oid, seq);
    if (!r.ok) return reply_err(map_.epoch, r.code, r.error);
    return reply_ok(map_.epoch);
  }
  if (!in_acting_set(oid, map_, cfg_.node_id, aios_path)) {
    return reply_err(map_.epoch, "not_replica", "not in acting set");
  }
  std::string err;
  if (!local_abort(aios_path, oid, seq, err)) {
    return reply_err(map_.epoch, "store_error", err);
  }
  return reply_ok(map_.epoch);
}

Frame ObjectService::handle_list_versions(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;
  const std::string oid = body.value("oid", "");
  const std::string aios_path = body.value("aios_path", "");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(map_.epoch, "bad_request", "oid and aios_path required");
  }
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(map_.epoch, "store_error", "no local store");
  std::string err;
  auto vers = store->list_versions(oid, err);
  auto f = reply_ok(map_.epoch);
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& v : vers) {
    nlohmann::json j = {{"seq", v.seq},
                        {"size", v.size},
                        {"ctime_ms", v.ctime_ms},
                        {"is_delete", v.is_delete},
                        {"inline_body", v.inline_body}};
    if (v.crc32c_known) j["crc32c"] = v.crc32c;
    if (!v.redirect_oid.empty()) j["redirect"] = v.redirect_oid;
    arr.push_back(std::move(j));
  }
  f.body["versions"] = std::move(arr);
  return f;
}

Frame ObjectService::handle_purge_versions(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;
  const std::string oid = body.value("oid", "");
  const std::string aios_path = body.value("aios_path", "");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(map_.epoch, "bad_request", "oid and aios_path required");
  }
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(map_.epoch, "store_error", "no local store");
  std::string err;
  if (body.contains("seq") && !body["seq"].is_null()) {
    const auto seq = body.value("seq", static_cast<std::uint64_t>(0));
    const bool allow_tip = body.value("allow_tip", false);
    if (!store->purge_version(oid, seq, allow_tip, err)) {
      return reply_err(map_.epoch, "store_error", err);
    }
  } else {
    int keep = body.value("keep", store->options().max_versions);
    if (!store->trim_versions(oid, keep, err)) {
      return reply_err(map_.epoch, "store_error", err);
    }
  }
  return reply_ok(map_.epoch);
}

ApiResult ObjectService::commit_ec_put(
    ObjectStore* store, const Placement& placement, const std::string& oid,
    const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    std::optional<std::uint32_t> expected_crc32c, const ObjectLayout& layout) {
  std::string err;
  auto codec = make_erasure_codec(layout.ec_k, layout.ec_m, layout.ec_codec, err);
  if (!codec) return fail("bad_request", err);
  if (static_cast<int>(placement.acting_set.size()) < codec->shard_count()) {
    return fail("no_targets", "acting set smaller than k+m");
  }
  const std::uint32_t full_crc = crc32c(data, len);
  if (expected_crc32c && *expected_crc32c != full_crc) {
    return fail("crc_mismatch", "crc32c mismatch");
  }

  std::vector<std::vector<std::uint8_t>> shards;
  if (!codec->encode(std::span<const std::uint8_t>(data, len), shards, err)) {
    return fail("store_error", err);
  }

  std::unordered_map<std::string, std::string> base = attrs;
  apply_layout_attrs(base, layout);
  set_ec_attrs(base, codec->k(), codec->m(), 0, codec->name(), len, full_crc);

  auto shard_attrs = [&](int i) {
    auto a = base;
    a[kEcAttrI] = std::to_string(i);
    return a;
  };

  PreparedVersion pv;
  const auto a0 = shard_attrs(0);
  if (!store->prepare_put(oid, shards[0].data(), shards[0].size(), a0, replace_attrs,
                          std::nullopt, pv, err)) {
    return fail("store_error", err);
  }

  int total_ok = 1;
  for (int i = 1; i < codec->shard_count(); ++i) {
    const auto& t = placement.acting_set[static_cast<std::size_t>(i)];
    PreparedVersion sv = pv;
    sv.size = shards[static_cast<std::size_t>(i)].size();
    sv.crc32c = crc32c(shards[static_cast<std::size_t>(i)].data(),
                       shards[static_cast<std::size_t>(i)].size());
    sv.inline_body = sv.size <= 64 * 1024;
    sv.fs_path.clear();
    const auto ai = shard_attrs(i);
    const auto* sd = shards[static_cast<std::size_t>(i)].data();
    const auto sl = shards[static_cast<std::size_t>(i)].size();
    bool done = false;
    if (t.node_id == cfg_.node_id) {
      done = local_install(t.aios_path, sv, sd, sl, ai, err);
    } else {
      auto r = object_install_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                     cfg_.auth_skew_ms, placement.epoch, t.aios_path, sv, sd,
                                     sl, ai);
      done = r.ok;
      if (!done) err = r.error;
    }
    if (done) ++total_ok;
    else
      AIOS_LOG_WARN("ec shard install failed i=", i, " ", err);
  }

  if (total_ok < quorum_need(placement)) {
    store->abort_version(oid, pv.seq, err);
    replicate_abort(placement, oid, pv.seq);
    return fail("quorum_failed", "ec shard quorum failed");
  }

  if (!store->publish_tip(oid, pv.seq, err)) {
    store->abort_version(oid, pv.seq, err);
    replicate_abort(placement, oid, pv.seq);
    return fail("store_error", err);
  }
  replicate_publish(placement, oid, pv.seq);

  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  r.replicas = total_ok;
  r.placement = placement;
  r.attrs = a0;
  r.info = ObjectInfo{};
  r.info->oid = oid;
  r.info->seq = pv.seq;
  r.info->size = len;
  r.info->crc32c = full_crc;
  r.info->crc32c_known = true;
  return r;
}

ApiResult ObjectService::reconstruct_ec_object(
    const Placement& placement, const std::string& oid, std::optional<std::uint64_t> seq,
    const std::unordered_map<std::string, std::string>& tip_attrs) {
  auto meta = parse_ec_attrs(tip_attrs);
  if (!meta) return fail("store_error", "missing ec attrs");
  std::string err;
  auto codec = make_erasure_codec(meta->k, meta->m, meta->codec, err);
  if (!codec || codec->m() != meta->m) return fail("store_error", "unsupported ec profile");
  if (static_cast<int>(placement.acting_set.size()) < codec->shard_count()) {
    return fail("no_targets", "acting set smaller than k+m");
  }

  std::vector<std::optional<std::vector<std::uint8_t>>> shards(
      static_cast<std::size_t>(codec->shard_count()));
  int got = 0;
  for (int i = 0; i < codec->shard_count(); ++i) {
    const auto& t = placement.acting_set[static_cast<std::size_t>(i)];
    if (t.node_id == cfg_.node_id) {
      auto* s = stores_.get(t.aios_path);
      if (!s) continue;
      auto data = s->get(oid, seq, err);
      if (!data) continue;
      shards[static_cast<std::size_t>(i)] = std::move(*data);
      ++got;
    } else {
      ObjectRpcResult r;
      if (seq.has_value()) {
        // Versioned remote get via ranged full read when seq set.
        auto st = object_stat_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                     cfg_.auth_skew_ms, map_.epoch, t.aios_path, oid);
        if (!st.ok) continue;
        r = object_get_range_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                    cfg_.auth_skew_ms, map_.epoch, t.aios_path, oid, 0,
                                    static_cast<std::size_t>(st.size), seq);
        if (r.ok && !r.raw.empty()) {
          shards[static_cast<std::size_t>(i)] = std::move(r.raw);
          ++got;
        } else if (r.ok && r.data) {
          shards[static_cast<std::size_t>(i)] = std::move(*r.data);
          ++got;
        }
      } else {
        r = object_get_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                              cfg_.auth_skew_ms, map_.epoch, t.aios_path, oid);
        if (!r.ok || !r.data) continue;
        shards[static_cast<std::size_t>(i)] = std::move(*r.data);
        ++got;
      }
    }
  }
  if (got < meta->k) return fail("quorum_failed", "not enough ec shards to reconstruct");

  std::vector<std::uint8_t> full;
  if (!codec->decode(shards, static_cast<std::size_t>(meta->full_size), full, err)) {
    return fail("store_error", err);
  }
  if (meta->full_crc_known && crc32c(full.data(), full.size()) != meta->full_crc) {
    return fail("crc_mismatch", "reconstructed object crc mismatch");
  }

  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  r.placement = placement;
  r.attrs = tip_attrs;
  r.data = std::move(full);
  r.info = ObjectInfo{};
  r.info->oid = oid;
  r.info->size = meta->full_size;
  r.info->crc32c = meta->full_crc;
  r.info->crc32c_known = meta->full_crc_known;
  if (seq.has_value()) r.info->seq = *seq;
  return r;
}

ApiResult ObjectService::api_put(const std::string& oid, const std::uint8_t* data,
                                std::size_t len,
                                const std::unordered_map<std::string, std::string>& attrs,
                                bool replace_attrs,
                                const std::vector<AttrPrecondition>& preds,
                                std::optional<std::uint32_t> expected_crc32c,
                                const LayoutRequest& layout_req) {
  std::lock_guard lock(mu_);
  ObjectLayout layout;
  std::string err;
  if (!resolve_object_layout(cfg_, layout_req, layout, err)) {
    return fail("bad_request", err);
  }
  auto placement = place(oid, map_, layout.n);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

  if (layout.is_ec()) {
    return commit_ec_put(store, placement, oid, data, len, attrs, replace_attrs,
                         expected_crc32c, layout);
  }

  auto put_attrs = attrs;
  apply_layout_attrs(put_attrs, layout);
  PreparedVersion pv;
  if (!store->prepare_put(oid, data, len, put_attrs, replace_attrs, expected_crc32c, pv,
                          err)) {
    if (err == "crc32c mismatch") return fail("crc_mismatch", err);
    return fail("store_error", err);
  }
  return commit_prepared(store, placement, pv, data, len, put_attrs);
}

ApiResult ObjectService::api_put_file(
    const std::string& oid, const std::string& staging_abs_path, std::uint64_t size,
    std::uint32_t crc32c_val, const std::unordered_map<std::string, std::string>& attrs,
    bool replace_attrs, const std::vector<AttrPrecondition>& preds,
    std::optional<std::uint32_t> expected_crc32c, const LayoutRequest& layout_req) {
  std::lock_guard lock(mu_);
  ObjectLayout layout;
  std::string err;
  if (!resolve_object_layout(cfg_, layout_req, layout, err)) {
    return fail("bad_request", err);
  }
  auto placement = place(oid, map_, layout.n);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

  if (layout.is_ec()) {
    constexpr std::uint64_t kEcMemLimit = 16ull * 1024ull * 1024ull;
    if (size > kEcMemLimit) {
      return fail("bad_request", "ec v1 supports objects up to 16 MiB");
    }
    std::ifstream in(staging_abs_path, std::ios::binary);
    if (!in) return fail("store_error", "cannot open staging file");
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    if (size > 0) {
      in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
      if (static_cast<std::uint64_t>(in.gcount()) != size) {
        return fail("store_error", "short read of staging file");
      }
    }
    return commit_ec_put(store, placement, oid, buf.data(), buf.size(), attrs, replace_attrs,
                         expected_crc32c.value_or(crc32c_val), layout);
  }

  auto put_attrs = attrs;
  apply_layout_attrs(put_attrs, layout);
  PreparedVersion pv;
  if (!store->prepare_put_file(oid, staging_abs_path, size, crc32c_val, put_attrs,
                               replace_attrs, expected_crc32c, pv, err)) {
    if (err == "crc32c mismatch") return fail("crc_mismatch", err);
    return fail("store_error", err);
  }
  return commit_prepared(store, placement, pv, nullptr, 0, put_attrs);
}

ApiResult ObjectService::api_put_redirect(
    const std::string& oid, const std::string& target_oid,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    const std::vector<AttrPrecondition>& preds) {
  std::lock_guard lock(mu_);
  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

  PreparedVersion pv;
  if (!store->prepare_redirect(oid, target_oid, attrs, replace_attrs, pv, err)) {
    return fail("store_error", err);
  }
  auto r = commit_prepared(store, placement, pv, nullptr, 0, attrs);
  if (r.ok) r.redirect_oid = target_oid;
  return r;
}

ApiResult ObjectService::api_put_range(
    const std::string& oid, std::uint64_t offset, const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    const std::vector<AttrPrecondition>& preds, const LayoutRequest& layout_req) {
  std::lock_guard lock(mu_);
  ObjectLayout layout;
  std::string err;
  if (!resolve_object_layout(cfg_, layout_req, layout, err)) {
    return fail("bad_request", err);
  }
  if (layout.is_ec()) {
    return fail("bad_request", "ranged put not supported for erasure-coded objects");
  }
  auto placement = place(oid, map_, layout.n);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

  {
    auto tip_attrs = store->list_attrs(oid, err);
    if (attrs_are_ec(tip_attrs)) {
      return fail("bad_request", "ranged put not supported for erasure-coded objects");
    }
  }

  auto put_attrs = attrs;
  apply_layout_attrs(put_attrs, layout);
  PreparedVersion pv;
  if (!store->prepare_put_range(oid, offset, data, len, put_attrs, replace_attrs, pv, err)) {
    return fail("store_error", err);
  }
  auto full = store->get(oid, pv.seq, err);
  if (!full) {
    store->abort_version(oid, pv.seq, err);
    return fail("store_error", err);
  }
  PreparedVersion install = pv;
  install.inline_body = false;
  return commit_prepared(store, placement, install, full->data(), full->size(), put_attrs);
}

ApiResult ObjectService::api_get(const std::string& oid, std::optional<std::uint64_t> offset,
                                std::optional<std::uint64_t> end_inclusive,
                                const std::vector<AttrPrecondition>& preds,
                                std::optional<std::uint64_t> seq) {
  std::lock_guard lock(mu_);
  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");

  std::string err;
  ObjectStore* store = nullptr;
  std::optional<ObjectInfo> info;
  std::unordered_map<std::string, std::string> attrs;
  for (const auto& t : placement.acting_set) {
    if (t.node_id != cfg_.node_id) continue;
    auto* s = stores_.get(t.aios_path);
    if (!s) continue;
    if (!store) store = s;  // remember a local store for pred checks / non-EC path
    auto st = s->stat(oid, seq, err);
    if (!st) continue;
    if (st->is_delete && !seq.has_value()) continue;
    store = s;
    info = st;
    attrs = s->list_attrs(oid, err);
    break;
  }
  if (!store) return fail("not_local", "no local replica for oid");

  if (!seq.has_value()) {
    auto pr = check_preds_on(store, oid, preds, err);
    if (pr == PrecondResult::NotFound) return fail("not_found", err);
    if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);
  }

  // Degraded read: no local tip — pull attrs from a remote acting-set member.
  if (!info) {
    for (const auto& t : placement.acting_set) {
      if (t.node_id == cfg_.node_id) continue;
      auto st = object_stat_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                   cfg_.auth_skew_ms, map_.epoch, t.aios_path, oid);
      if (!st.ok) continue;
      if (st.body.contains("attrs") && st.body["attrs"].is_object()) {
        for (auto it = st.body["attrs"].begin(); it != st.body["attrs"].end(); ++it) {
          if (it.value().is_string()) attrs[it.key()] = it.value().get<std::string>();
        }
      }
      if (attrs_are_ec(attrs)) {
        info = ObjectInfo{};
        info->oid = oid;
        info->seq = st.body.value("seq", static_cast<std::uint64_t>(0));
        break;
      }
    }
  }

  if (!info) return fail("not_found", "object not found");
  if (info->is_delete && !seq.has_value()) return fail("not_found", "object not found");

  if (!info->redirect_oid.empty()) {
    ApiResult r;
    r.ok = true;
    r.epoch = map_.epoch;
    r.info = info;
    r.attrs = attrs;
    r.placement = placement;
    r.redirect_oid = info->redirect_oid;
    r.code = "redirect";
    return r;
  }

  // EC objects carry aios.ec.* attrs. Non-EC tips (e.g. txn-prepared full copies) still
  // use the normal local read path even when the cluster default layout is ec.
  if (attrs_are_ec(attrs)) {
    const int n = placement_n_for_attrs(attrs, map_.replica_count);
    placement = place(oid, map_, n);
    if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
    auto rec = reconstruct_ec_object(placement, oid, seq, attrs);
    if (!rec.ok) return rec;
    rec.info->seq = info->seq;
    rec.info->mtime_ms = info->mtime_ms;
    rec.info->ctime_ms = info->ctime_ms;
    if (!offset.has_value()) return rec;
    if (!rec.data) return fail("store_error", "ec reconstruct produced no data");
    if (*offset >= rec.data->size()) return fail("range_unsatisfiable", "range unsatisfiable");
    std::uint64_t end = end_inclusive.value_or(rec.data->size() - 1);
    if (end >= rec.data->size()) end = rec.data->size() - 1;
    if (end < *offset) return fail("range_unsatisfiable", "range unsatisfiable");
    std::vector<std::uint8_t> slice(rec.data->begin() + static_cast<std::ptrdiff_t>(*offset),
                                    rec.data->begin() + static_cast<std::ptrdiff_t>(end + 1));
    rec.data = std::move(slice);
    return rec;
  }

  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  r.info = info;
  r.attrs = attrs;
  r.placement = placement;

  if (!offset.has_value()) {
    constexpr std::uint64_t kStreamThreshold = 256u * 1024u;
    if (!info->inline_body && info->size >= kStreamThreshold) {
      if (auto path = store->fs_body_path(oid, seq, err)) {
        r.body_path = *path;
        return r;
      }
    }
    r.data = store->get(oid, seq, err);
    if (!r.data) {
      if (info->is_delete) {
        r.data = std::vector<std::uint8_t>{};
        return r;
      }
      return fail("not_found", err);
    }
    return r;
  }
  if (*offset >= info->size) return fail("range_unsatisfiable", "range unsatisfiable");
  std::uint64_t end = end_inclusive.value_or(info->size - 1);
  if (end >= info->size) end = info->size - 1;
  if (end < *offset) return fail("range_unsatisfiable", "range unsatisfiable");
  const std::size_t len = static_cast<std::size_t>(end - *offset + 1);
  r.data = store->get_range(oid, seq, *offset, len, err);
  if (!r.data) {
    if (err == "range unsatisfiable") return fail("range_unsatisfiable", err);
    return fail("store_error", err);
  }
  return r;
}

ApiResult ObjectService::api_head(const std::string& oid,
                                 const std::vector<AttrPrecondition>& preds,
                                 std::optional<std::uint64_t> seq) {
  return api_get(oid, std::nullopt, std::nullopt, preds, seq);
}

ApiResult ObjectService::api_del(const std::string& oid,
                                const std::vector<AttrPrecondition>& preds) {
  std::lock_guard lock(mu_);
  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

  PreparedVersion pv;
  if (!store->prepare_delete(oid, pv, err)) {
    if (err == "object not found") return fail("not_found", err);
    return fail("store_error", err);
  }
  return commit_prepared(store, placement, pv, nullptr, 0, {});
}

ApiResult ObjectService::api_list(const std::string& prefix, const std::string& attr_eq_key,
                                  const std::string& attr_eq_value, std::size_t limit,
                                  const std::string& cursor, bool include_attrs,
                                  bool cluster) {
  std::lock_guard lock(mu_);
  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;

  auto list_local = [&](const std::string& local_cursor) -> ApiResult {
    ApiResult lr;
    lr.ok = true;
    lr.epoch = map_.epoch;
    for (const auto& path : stores_.paths()) {
      auto* store = stores_.get(path);
      if (!store) continue;
      std::string err;
      auto part = store->list(prefix, attr_eq_key, attr_eq_value, limit, local_cursor,
                              include_attrs, err);
      if (!err.empty() && lr.list.objects.empty()) {
        return fail("store_error", err);
      }
      for (auto& o : part.objects) {
        lr.list.objects.push_back(std::move(o));
        if (limit > 0 && lr.list.objects.size() >= limit) {
          lr.list.next_cursor = lr.list.objects.back().oid;
          return lr;
        }
      }
    }
    return lr;
  };

  if (!cluster) {
    return list_local(cursor);
  }

  // Scatter-gather by unique node; merge sorted by oid; cursor = last oid.
  std::vector<ObjectListEntry> merged;
  std::unordered_set<std::string> seen_nodes;
  for (const auto& t : map_.targets) {
    if (!seen_nodes.insert(t.node_id).second) continue;
    ObjectListResult part;
    if (t.node_id == cfg_.node_id) {
      auto lr = list_local(cursor);
      if (!lr.ok) return lr;
      part = std::move(lr.list);
    } else {
      auto remote =
          object_list_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                             cfg_.auth_skew_ms, map_.epoch, prefix, attr_eq_key,
                             attr_eq_value, limit, cursor, include_attrs);
      if (!remote.ok) {
        AIOS_LOG_WARN("cluster list from ", t.addr, " failed: ", remote.error);
        continue;
      }
      part = std::move(remote.list);
    }
    for (auto& o : part.objects) {
      if (!cursor.empty() && o.oid <= cursor) continue;
      if (!prefix.empty() && o.oid.rfind(prefix, 0) != 0) continue;
      merged.push_back(std::move(o));
    }
  }
  std::sort(merged.begin(), merged.end(),
            [](const ObjectListEntry& a, const ObjectListEntry& b) { return a.oid < b.oid; });
  // Dedup by oid (same object may appear if listed from multiple nodes incorrectly).
  {
    std::vector<ObjectListEntry> uniq;
    for (auto& o : merged) {
      if (!uniq.empty() && uniq.back().oid == o.oid) continue;
      uniq.push_back(std::move(o));
    }
    merged.swap(uniq);
  }
  for (auto& o : merged) {
    r.list.objects.push_back(std::move(o));
    if (limit > 0 && r.list.objects.size() >= limit) {
      r.list.next_cursor = r.list.objects.back().oid;
      return r;
    }
  }
  return r;
}

ApiResult ObjectService::api_list_versions(const std::string& oid) {
  std::lock_guard lock(mu_);
  auto placement = place(oid, map_);
  ObjectStore* store = nullptr;
  std::string err;
  for (const auto& t : placement.acting_set) {
    if (t.node_id != cfg_.node_id) continue;
    store = stores_.get(t.aios_path);
    if (store) break;
  }
  if (!store) {
    for (const auto& path : stores_.paths()) {
      store = stores_.get(path);
      if (store) break;
    }
  }
  if (!store) return fail("not_local", "no local store");
  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  r.versions = store->list_versions(oid, err);
  return r;
}

ApiResult ObjectService::api_purge_version(const std::string& oid, std::uint64_t seq,
                                           bool allow_tip) {
  std::lock_guard lock(mu_);
  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    return fail("not_primary", "this node is not primary for oid");
  }
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  if (!store->purge_version(oid, seq, allow_tip, err)) return fail("store_error", err);
  // Best-effort fan-out.
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    const auto& t = placement.acting_set[i];
    if (t.node_id == cfg_.node_id) {
      local_abort(t.aios_path, oid, seq, err);
      continue;
    }
    object_abort_version_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                cfg_.auth_skew_ms, placement.epoch, t.aios_path, oid, seq);
  }
  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  return r;
}

ApiResult ObjectService::api_trim_versions(const std::string& oid, int keep) {
  std::lock_guard lock(mu_);
  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    return fail("not_primary", "this node is not primary for oid");
  }
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  if (keep <= 0) keep = store->options().max_versions;
  if (!store->trim_versions(oid, keep, err)) return fail("store_error", err);
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    const auto& t = placement.acting_set[i];
    if (t.node_id == cfg_.node_id) {
      auto* s = stores_.get(t.aios_path);
      if (s) s->trim_versions(oid, keep, err);
      continue;
    }
    object_purge_versions_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                 cfg_.auth_skew_ms, placement.epoch, t.aios_path, oid, keep);
  }
  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  return r;
}

Frame ObjectService::handle_stage_begin(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;
  const std::string oid = body.value("oid", "");
  const std::string aios_path = body.value("aios_path", "");
  const auto seq = body.value("seq", static_cast<std::uint64_t>(0));
  if (oid.empty() || aios_path.empty() || seq == 0) {
    return reply_err(map_.epoch, "bad_request", "oid/aios_path/seq required");
  }
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(map_.epoch, "store_error", "no local store");
  std::string path, err;
  if (!store->stage_path_for(oid, seq, path, err)) {
    return reply_err(map_.epoch, "store_error", err);
  }
  if (!store->stage_truncate(path, err)) {
    return reply_err(map_.epoch, "store_error", err);
  }
  return reply_ok(map_.epoch);
}

Frame ObjectService::handle_stage_data(const Frame& req) {
  Frame errf;
  if (!epoch_ok(req.body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;
  const std::string oid = req.body.value("oid", "");
  const std::string aios_path = req.body.value("aios_path", "");
  const auto seq = req.body.value("seq", static_cast<std::uint64_t>(0));
  const auto offset = req.body.value("offset", static_cast<std::uint64_t>(0));
  if (oid.empty() || aios_path.empty() || seq == 0) {
    return reply_err(map_.epoch, "bad_request", "oid/aios_path/seq required");
  }
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(map_.epoch, "store_error", "no local store");
  std::string path, err;
  if (!store->stage_path_for(oid, seq, path, err)) {
    return reply_err(map_.epoch, "store_error", err);
  }
  if (!store->stage_pwrite(path, offset, req.raw.data(), req.raw.size(), err)) {
    return reply_err(map_.epoch, "store_error", err);
  }
  return reply_ok(map_.epoch);
}

Frame ObjectService::handle_stage_commit(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;
  const std::string oid = body.value("oid", "");
  const std::string aios_path = body.value("aios_path", "");
  if (oid.empty() || aios_path.empty()) {
    return reply_err(map_.epoch, "bad_request", "oid/aios_path required");
  }
  auto* store = stores_.get(aios_path);
  if (!store) return reply_err(map_.epoch, "store_error", "no local store");

  PreparedVersion v;
  v.oid = oid;
  v.seq = body.value("seq", static_cast<std::uint64_t>(0));
  v.prev_tip = body.value("base_seq", static_cast<std::uint64_t>(0));
  v.size = body.value("size", static_cast<std::uint64_t>(0));
  v.crc32c = body.value("crc32c", 0u);
  v.inline_body = false;
  v.is_delete = body.value("is_delete", false);
  v.fs_path = body.value("fs_path", "");
  v.redirect_oid = body.value("redirect", "");
  if (v.seq == 0) return reply_err(map_.epoch, "bad_request", "seq required");

  auto attrs = parse_attrs_json(body);
  std::string err;
  if (!v.is_delete && v.redirect_oid.empty() && v.size > 0) {
    std::string staging;
    if (!store->stage_path_for(oid, v.seq, staging, err)) {
      return reply_err(map_.epoch, "store_error", err);
    }
    std::string rel;
    if (!store->place_staging_as_version(oid, v.seq, staging, rel, err)) {
      return reply_err(map_.epoch, "store_error", err);
    }
    v.fs_path = rel;
  }
  if (!store->install_version(v, nullptr, 0, attrs, err)) {
    return reply_err(map_.epoch, "store_error", err);
  }
  return reply_ok(map_.epoch);
}

Frame ObjectService::handle_list(const nlohmann::json& body) {
  Frame errf;
  if (!epoch_ok(body.value("epoch", static_cast<std::uint64_t>(0)), errf)) return errf;
  const std::string prefix = body.value("prefix", "");
  const std::string attr_key = body.value("attr_eq_key", "");
  const std::string attr_val = body.value("attr_eq_value", "");
  const std::size_t limit = body.value("limit", static_cast<std::size_t>(1000));
  const std::string cursor = body.value("cursor", "");
  const bool include_attrs = body.value("attrs", false);

  // Local-only listing for scatter-gather leaves.
  auto r = api_list(prefix, attr_key, attr_val, limit, cursor, include_attrs,
                    /*cluster=*/false);
  if (!r.ok) return reply_err(map_.epoch, r.code, r.error);
  Frame f = reply_ok(map_.epoch);
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& o : r.list.objects) {
    nlohmann::json jo = {{"oid", o.oid},
                         {"seq", o.seq},
                         {"size", o.size},
                         {"mtime_ms", o.mtime_ms},
                         {"crc32c", o.crc32c},
                         {"is_delete", o.is_delete},
                         {"redirect_oid", o.redirect_oid}};
    if (include_attrs) jo["attrs"] = o.attrs;
    arr.push_back(std::move(jo));
  }
  f.body["objects"] = std::move(arr);
  f.body["next_cursor"] = r.list.next_cursor;
  return f;
}

namespace {

std::string make_txn_id() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::ostringstream os;
  os << std::hex << aios::now_ms() << "-" << rng();
  return os.str();
}

std::string txn_oid(const std::string& txn_id) { return "txn/" + txn_id; }

}  // namespace

ApiResult ObjectService::api_prepare_put(
    const std::string& oid, const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs,
    const std::vector<AttrPrecondition>& preds, std::optional<std::uint32_t> expected_crc32c) {
  std::lock_guard lock(mu_);
  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);
  PreparedVersion pv;
  if (!store->prepare_put(oid, data, len, attrs, replace_attrs, expected_crc32c, pv, err)) {
    if (err == "crc32c mismatch") return fail("crc_mismatch", err);
    return fail("store_error", err);
  }
  return install_prepared(store, placement, pv, data, len, attrs);
}

ApiResult ObjectService::api_prepare_put_file(
    const std::string& oid, const std::string& staging_abs_path, std::uint64_t size,
    std::uint32_t crc32c_val, const std::unordered_map<std::string, std::string>& attrs,
    bool replace_attrs, const std::vector<AttrPrecondition>& preds,
    std::optional<std::uint32_t> expected_crc32c) {
  std::lock_guard lock(mu_);
  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);
  PreparedVersion pv;
  if (!store->prepare_put_file(oid, staging_abs_path, size, crc32c_val, attrs, replace_attrs,
                               expected_crc32c, pv, err)) {
    if (err == "crc32c mismatch") return fail("crc_mismatch", err);
    return fail("store_error", err);
  }
  return install_prepared(store, placement, pv, nullptr, 0, attrs);
}

ApiResult ObjectService::api_prepare_delete(const std::string& oid,
                                           const std::vector<AttrPrecondition>& preds) {
  std::lock_guard lock(mu_);
  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);
  PreparedVersion pv;
  if (!store->prepare_delete(oid, pv, err)) {
    if (err == "object not found") return fail("not_found", err);
    return fail("store_error", err);
  }
  return install_prepared(store, placement, pv, nullptr, 0, {});
}

ApiResult ObjectService::api_publish_version(const std::string& oid, std::uint64_t seq) {
  std::lock_guard lock(mu_);
  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  if (!store->publish_tip(oid, seq, err)) return fail("store_error", err);
  replicate_publish(placement, oid, seq);
  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  r.placement = placement;
  if (auto st = store->stat(oid, err)) r.info = st;
  return r;
}

ApiResult ObjectService::api_abort_prepared(const std::string& oid, std::uint64_t seq) {
  std::lock_guard lock(mu_);
  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not primary for oid");
    r.placement = placement;
    return r;
  }
  std::string err;
  auto* store = primary_store(placement, err);
  if (!store) return fail("store_error", err);
  if (!store->abort_version(oid, seq, err)) return fail("store_error", err);
  replicate_abort(placement, oid, seq);
  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  r.placement = placement;
  return r;
}

ApiResult ObjectService::load_txn_state(const std::string& txn_id, nlohmann::json& state_out) {
  const auto oid = txn_oid(txn_id);
  auto got = api_get(oid, std::nullopt, std::nullopt, {}, std::nullopt);
  if (!got.ok) return got;
  if (got.data->empty()) return fail("bad_request", "empty txn state");
  try {
    state_out = nlohmann::json::parse(got.data->begin(), got.data->end());
  } catch (const std::exception& e) {
    return fail("bad_request", std::string("bad txn json: ") + e.what());
  }
  return got;
}

ApiResult ObjectService::save_txn_state(const std::string& txn_id, const nlohmann::json& state) {
  const auto oid = txn_oid(txn_id);
  const auto body = state.dump();
  return api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                 {{"aios.txn", "1"}}, true, {},
                 crc32c(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()),
                 layout_request_replica());
}

ApiResult ObjectService::require_txn_primary(const std::string& txn_id,
                                             nlohmann::json& state_out) {
  const auto oid = txn_oid(txn_id);
  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "this node is not txn coordinator");
    r.placement = placement;
    return r;
  }
  auto loaded = load_txn_state(txn_id, state_out);
  if (!loaded.ok) return loaded;
  loaded.placement = placement;
  return loaded;
}

ApiResult ObjectService::api_txn_begin() {
  std::lock_guard lock(mu_);
  // Pick a txn id whose primary is this node (coordinator = primary for txn/<id>).
  for (int attempt = 0; attempt < 64; ++attempt) {
    const auto id = make_txn_id();
    const auto oid = txn_oid(id);
    auto placement = place(oid, map_);
    if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
    if (placement.acting_set[0].node_id != cfg_.node_id) continue;
    nlohmann::json state = {{"txn_id", id},
                            {"state", "open"},
                            {"ops", nlohmann::json::array()}};
    const auto body = state.dump();
    std::vector<AttrPrecondition> create_preds = {
        {AttrPrecondition::Kind::MustNotExist, {}, {}}};
    auto saved =
        api_put(oid, reinterpret_cast<const std::uint8_t*>(body.data()), body.size(),
                {{"aios.txn", "1"}}, true, create_preds,
                crc32c(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()),
                layout_request_replica());
    if (!saved.ok) {
      if (saved.code == "precondition_failed") continue;
      return saved;
    }
    ApiResult r;
    r.ok = true;
    r.epoch = map_.epoch;
    r.placement = placement;
    r.attrs["txn_id"] = id;
    // Stash JSON in body for HTTP.
    const auto s = state.dump();
    r.data = std::vector<std::uint8_t>(s.begin(), s.end());
    return r;
  }
  return fail("store_error", "could not allocate txn id");
}

ApiResult ObjectService::api_txn_get(const std::string& txn_id) {
  std::lock_guard lock(mu_);
  nlohmann::json state;
  auto r = require_txn_primary(txn_id, state);
  if (!r.ok) return r;
  const auto s = state.dump();
  r.data = std::vector<std::uint8_t>(s.begin(), s.end());
  r.attrs["txn_id"] = txn_id;
  return r;
}

ApiResult ObjectService::api_txn_prepare_put(
    const std::string& txn_id, const std::string& oid, const std::uint8_t* data,
    std::size_t len, const std::unordered_map<std::string, std::string>& attrs,
    const std::vector<AttrPrecondition>& preds, std::optional<std::uint32_t> expected_crc32c) {
  std::lock_guard lock(mu_);
  nlohmann::json state;
  auto tr = require_txn_primary(txn_id, state);
  if (!tr.ok) return tr;
  if (state.value("state", "") != "open") return fail("conflict", "txn not open");

  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");

  std::unordered_map<std::string, std::string> put_attrs = attrs;
  put_attrs["aios.txn"] = txn_id;

  ApiResult prep;
  if (placement.acting_set[0].node_id == cfg_.node_id) {
    prep = api_prepare_put(oid, data, len, put_attrs, true, preds, expected_crc32c);
  } else {
    auto remote = object_prepare_put_remote(
        placement.acting_set[0].addr, cfg_.node_id, advertise_, cfg_.cluster_key,
        cfg_.auth_skew_ms, map_.epoch, placement.acting_set[0].aios_path, oid, data, len,
        put_attrs);
    if (!remote.ok) {
      auto r = fail(remote.code.empty() ? "rpc_error" : remote.code, remote.error);
      r.placement = placement;
      return r;
    }
    prep.ok = true;
    prep.epoch = remote.epoch;
    prep.placement = placement;
    prep.info = ObjectInfo{};
    prep.info->oid = oid;
    prep.info->seq = remote.body.value("seq", static_cast<std::uint64_t>(0));
  }
  if (!prep.ok) return prep;
  if (!prep.info || prep.info->seq == 0) return fail("store_error", "prepare missing seq");

  state["ops"].push_back({{"oid", oid},
                          {"seq", prep.info->seq},
                          {"kind", "put"},
                          {"primary", placement.acting_set[0].node_id},
                          {"addr", placement.acting_set[0].addr},
                          {"aios_path", placement.acting_set[0].aios_path}});
  auto saved = save_txn_state(txn_id, state);
  if (!saved.ok) {
    // Best-effort abort prepared version.
    if (placement.acting_set[0].node_id == cfg_.node_id) {
      api_abort_prepared(oid, prep.info->seq);
    } else {
      object_abort_prepared_remote(placement.acting_set[0].addr, cfg_.node_id, advertise_,
                                   cfg_.cluster_key, cfg_.auth_skew_ms, map_.epoch,
                                   placement.acting_set[0].aios_path, oid, prep.info->seq);
    }
    return saved;
  }
  prep.attrs["txn_id"] = txn_id;
  return prep;
}

ApiResult ObjectService::api_txn_prepare_put_file(
    const std::string& txn_id, const std::string& oid, const std::string& staging_abs_path,
    std::uint64_t size, std::uint32_t crc32c_val,
    const std::unordered_map<std::string, std::string>& attrs,
    const std::vector<AttrPrecondition>& preds, std::optional<std::uint32_t> expected_crc32c) {
  std::lock_guard lock(mu_);
  nlohmann::json state;
  auto tr = require_txn_primary(txn_id, state);
  if (!tr.ok) return tr;
  if (state.value("state", "") != "open") return fail("conflict", "txn not open");

  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  // Large file prepare currently requires local primary (staging path is local).
  if (placement.acting_set[0].node_id != cfg_.node_id) {
    auto r = fail("not_primary", "large txn put requires oid primary as coordinator host");
    r.placement = placement;
    return r;
  }
  std::unordered_map<std::string, std::string> put_attrs = attrs;
  put_attrs["aios.txn"] = txn_id;
  auto prep = api_prepare_put_file(oid, staging_abs_path, size, crc32c_val, put_attrs, true,
                                   preds, expected_crc32c);
  if (!prep.ok) return prep;
  state["ops"].push_back({{"oid", oid},
                          {"seq", prep.info->seq},
                          {"kind", "put"},
                          {"primary", cfg_.node_id},
                          {"addr", advertise_},
                          {"aios_path", placement.acting_set[0].aios_path}});
  auto saved = save_txn_state(txn_id, state);
  if (!saved.ok) {
    api_abort_prepared(oid, prep.info->seq);
    return saved;
  }
  prep.attrs["txn_id"] = txn_id;
  return prep;
}

ApiResult ObjectService::api_txn_prepare_delete(const std::string& txn_id, const std::string& oid,
                                              const std::vector<AttrPrecondition>& preds) {
  std::lock_guard lock(mu_);
  nlohmann::json state;
  auto tr = require_txn_primary(txn_id, state);
  if (!tr.ok) return tr;
  if (state.value("state", "") != "open") return fail("conflict", "txn not open");

  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");

  ApiResult prep;
  if (placement.acting_set[0].node_id == cfg_.node_id) {
    prep = api_prepare_delete(oid, preds);
  } else {
    auto remote = object_prepare_delete_remote(
        placement.acting_set[0].addr, cfg_.node_id, advertise_, cfg_.cluster_key,
        cfg_.auth_skew_ms, map_.epoch, placement.acting_set[0].aios_path, oid);
    if (!remote.ok) {
      return fail(remote.code.empty() ? "rpc_error" : remote.code, remote.error);
    }
    prep.ok = true;
    prep.epoch = remote.epoch;
    prep.placement = placement;
    prep.info = ObjectInfo{};
    prep.info->oid = oid;
    prep.info->seq = remote.body.value("seq", static_cast<std::uint64_t>(0));
    prep.info->is_delete = true;
  }
  if (!prep.ok) return prep;
  state["ops"].push_back({{"oid", oid},
                          {"seq", prep.info->seq},
                          {"kind", "delete"},
                          {"primary", placement.acting_set[0].node_id},
                          {"addr", placement.acting_set[0].addr},
                          {"aios_path", placement.acting_set[0].aios_path}});
  auto saved = save_txn_state(txn_id, state);
  if (!saved.ok) {
    if (placement.acting_set[0].node_id == cfg_.node_id) {
      api_abort_prepared(oid, prep.info->seq);
    } else {
      object_abort_prepared_remote(placement.acting_set[0].addr, cfg_.node_id, advertise_,
                                   cfg_.cluster_key, cfg_.auth_skew_ms, map_.epoch,
                                   placement.acting_set[0].aios_path, oid, prep.info->seq);
    }
    return saved;
  }
  return prep;
}

ApiResult ObjectService::api_txn_commit(const std::string& txn_id) {
  std::lock_guard lock(mu_);
  nlohmann::json state;
  auto tr = require_txn_primary(txn_id, state);
  if (!tr.ok) return tr;
  if (state.value("state", "") != "open") return fail("conflict", "txn not open");
  state["state"] = "committing";
  auto mid = save_txn_state(txn_id, state);
  if (!mid.ok) return mid;

  std::vector<nlohmann::json> ops = state.value("ops", nlohmann::json::array());
  std::sort(ops.begin(), ops.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.value("oid", "") < b.value("oid", "");
  });

  std::vector<nlohmann::json> published;
  for (const auto& op : ops) {
    const auto oid = op.value("oid", "");
    const auto seq = op.value("seq", static_cast<std::uint64_t>(0));
    const auto primary = op.value("primary", "");
    const auto addr = op.value("addr", "");
    const auto aios_path = op.value("aios_path", "");
    bool ok = false;
    if (primary == cfg_.node_id) {
      ok = api_publish_version(oid, seq).ok;
    } else {
      ok = object_publish_prepared_remote(addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                          cfg_.auth_skew_ms, map_.epoch, aios_path, oid, seq)
               .ok;
    }
    if (!ok) {
      // Abort remaining (including failed) prepared versions.
      for (const auto& mop : ops) {
        bool already = false;
        for (const auto& p : published) {
          if (p.value("oid", "") == mop.value("oid", "")) {
            already = true;
            break;
          }
        }
        if (already) continue;
        const auto moid = mop.value("oid", "");
        const auto mseq = mop.value("seq", static_cast<std::uint64_t>(0));
        if (mop.value("primary", "") == cfg_.node_id) {
          api_abort_prepared(moid, mseq);
        } else {
          object_abort_prepared_remote(mop.value("addr", ""), cfg_.node_id, advertise_,
                                       cfg_.cluster_key, cfg_.auth_skew_ms, map_.epoch,
                                       mop.value("aios_path", ""), moid, mseq);
        }
      }
      state["state"] = "aborted";
      state["error"] = "publish failed for " + oid;
      save_txn_state(txn_id, state);
      return fail("quorum_failed", "txn commit failed");
    }
    published.push_back(op);
  }

  state["state"] = "committed";
  state["ops"] = ops;
  auto saved = save_txn_state(txn_id, state);
  if (!saved.ok) return saved;
  const auto s = state.dump();
  saved.data = std::vector<std::uint8_t>(s.begin(), s.end());
  saved.attrs["txn_id"] = txn_id;
  return saved;
}

ApiResult ObjectService::api_txn_abort(const std::string& txn_id) {
  std::lock_guard lock(mu_);
  nlohmann::json state;
  auto tr = require_txn_primary(txn_id, state);
  if (!tr.ok) return tr;
  const auto cur = state.value("state", "");
  if (cur == "committed") return fail("conflict", "txn already committed");
  for (const auto& op : state.value("ops", nlohmann::json::array())) {
    const auto oid = op.value("oid", "");
    const auto seq = op.value("seq", static_cast<std::uint64_t>(0));
    if (op.value("primary", "") == cfg_.node_id) {
      api_abort_prepared(oid, seq);
    } else {
      object_abort_prepared_remote(op.value("addr", ""), cfg_.node_id, advertise_,
                                   cfg_.cluster_key, cfg_.auth_skew_ms, map_.epoch,
                                   op.value("aios_path", ""), oid, seq);
    }
  }
  state["state"] = "aborted";
  auto saved = save_txn_state(txn_id, state);
  if (!saved.ok) return saved;
  const auto s = state.dump();
  saved.data = std::vector<std::uint8_t>(s.begin(), s.end());
  return saved;
}

}  // namespace aios
