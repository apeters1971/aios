#include "object/object_service.hpp"

#include "cluster/place.hpp"
#include "net/object_client.hpp"
#include "util/base64.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <mutex>

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

bool ObjectService::local_put(const std::string& aios_path, const std::string& oid,
                              const std::uint8_t* data, std::size_t len,
                              const std::unordered_map<std::string, std::string>& attrs,
                              std::string& err) {
  auto* store = stores_.get(aios_path);
  if (!store) {
    err = "no local store for " + aios_path;
    return false;
  }
  return store->put(oid, data, len, attrs, true, err);
}

bool ObjectService::local_put_range(const std::string& aios_path, const std::string& oid,
                                    std::uint64_t offset, const std::uint8_t* data,
                                    std::size_t len,
                                    const std::unordered_map<std::string, std::string>& attrs,
                                    bool replace_attrs, std::string& err) {
  auto* store = stores_.get(aios_path);
  if (!store) {
    err = "no local store for " + aios_path;
    return false;
  }
  return store->put_range(oid, offset, data, len, attrs, replace_attrs, err);
}

bool ObjectService::local_del(const std::string& aios_path, const std::string& oid,
                              std::string& err) {
  auto* store = stores_.get(aios_path);
  if (!store) {
    err = "no local store for " + aios_path;
    return false;
  }
  return store->del(oid, err);
}

int ObjectService::replicate_put(const Placement& placement, const std::string& oid,
                                 const std::uint8_t* data, std::size_t len,
                                 const std::unordered_map<std::string, std::string>& attrs) {
  int ok = 0;
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    const auto& t = placement.acting_set[i];
    if (t.node_id == cfg_.node_id) {
      std::string err;
      if (local_put(t.aios_path, oid, data, len, attrs, err)) {
        ++ok;
      } else {
        AIOS_LOG_WARN("local replica put failed ", t.aios_path, " oid=", oid, ": ", err);
      }
      continue;
    }
    auto r = object_put_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                               cfg_.auth_skew_ms, placement.epoch, t.aios_path, oid, data,
                               len, attrs, /*as_replica=*/true);
    if (r.ok) ++ok;
    else
      AIOS_LOG_WARN("remote replica put failed ", t.addr, ": ", r.error);
  }
  return ok;
}

int ObjectService::replicate_put_range(
    const Placement& placement, const std::string& oid, std::uint64_t offset,
    const std::uint8_t* data, std::size_t len,
    const std::unordered_map<std::string, std::string>& attrs, bool replace_attrs) {
  int ok = 0;
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    const auto& t = placement.acting_set[i];
    if (t.node_id == cfg_.node_id) {
      std::string err;
      if (local_put_range(t.aios_path, oid, offset, data, len, attrs, replace_attrs, err)) {
        ++ok;
      } else {
        AIOS_LOG_WARN("local replica put_range failed ", t.aios_path, ": ", err);
      }
      continue;
    }
    auto r = object_put_range_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                                     cfg_.auth_skew_ms, placement.epoch, t.aios_path, oid,
                                     offset, data, len, attrs, replace_attrs,
                                     /*as_replica=*/true);
    if (r.ok) ++ok;
    else
      AIOS_LOG_WARN("remote replica put_range failed ", t.addr, ": ", r.error);
  }
  return ok;
}

int ObjectService::replicate_del(const Placement& placement, const std::string& oid) {
  int ok = 0;
  for (std::size_t i = 1; i < placement.acting_set.size(); ++i) {
    const auto& t = placement.acting_set[i];
    if (t.node_id == cfg_.node_id) {
      std::string err;
      if (local_del(t.aios_path, oid, err) || err == "object not found") ++ok;
      continue;
    }
    auto r = object_del_remote(t.addr, cfg_.node_id, advertise_, cfg_.cluster_key,
                               cfg_.auth_skew_ms, placement.epoch, t.aios_path, oid, true);
    if (r.ok || r.code == "not_found") ++ok;
  }
  return ok;
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
    default:
      return reply_err(map_.epoch, "bad_type", "unsupported object message");
  }
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
  std::string derr;
  if (!body.contains("data_b64") || !body["data_b64"].is_string() ||
      !base64_decode(body["data_b64"].get<std::string>(), data, derr)) {
    return reply_err(map_.epoch, "bad_request", "invalid data_b64: " + derr);
  }

  std::unordered_map<std::string, std::string> attrs;
  if (body.contains("attrs") && body["attrs"].is_object()) {
    for (auto it = body["attrs"].begin(); it != body["attrs"].end(); ++it) {
      if (it.value().is_string()) attrs[it.key()] = it.value().get<std::string>();
    }
  }

  const auto placement = place(oid, map_);
  if (placement.acting_set.empty()) {
    return reply_err(map_.epoch, "no_targets", "no storage targets in cluster map");
  }

  if (role == "replica") {
    if (!in_acting_set(oid, map_, cfg_.node_id, aios_path)) {
      return reply_err(map_.epoch, "not_replica", "not in acting set for oid");
    }
    std::string err;
    if (!local_put(aios_path, oid, data.data(), data.size(), attrs, err)) {
      return reply_err(map_.epoch, "store_error", err);
    }
    return reply_ok(map_.epoch);
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

  std::string err;
  if (!local_put(aios_path, oid, data.data(), data.size(), attrs, err)) {
    return reply_err(map_.epoch, "store_error", err);
  }
  const int total_ok = 1 + replicate_put(placement, oid, data.data(), data.size(), attrs);
  if (total_ok < quorum_need(placement)) {
    return reply_err(map_.epoch, "quorum_failed", "quorum failed");
  }
  auto f = reply_ok(map_.epoch);
  f.body["replicas"] = total_ok;
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

  std::unordered_map<std::string, std::string> attrs;
  if (body.contains("attrs") && body["attrs"].is_object()) {
    for (auto it = body["attrs"].begin(); it != body["attrs"].end(); ++it) {
      if (it.value().is_string()) attrs[it.key()] = it.value().get<std::string>();
    }
  }

  const auto placement = place(oid, map_);
  if (placement.acting_set.empty()) {
    return reply_err(map_.epoch, "no_targets", "no storage targets");
  }

  if (role == "replica") {
    if (!in_acting_set(oid, map_, cfg_.node_id, aios_path)) {
      return reply_err(map_.epoch, "not_replica", "not in acting set");
    }
    std::string err;
    if (!local_put_range(aios_path, oid, offset, data, len, attrs, replace_attrs, err)) {
      return reply_err(map_.epoch, "store_error", err);
    }
    return reply_ok(map_.epoch);
  }

  if (!is_primary_for(oid, map_, cfg_.node_id, aios_path)) {
    return reply_err(map_.epoch, "not_primary", "not primary");
  }

  std::string err;
  if (!local_put_range(aios_path, oid, offset, data, len, attrs, replace_attrs, err)) {
    return reply_err(map_.epoch, "store_error", err);
  }
  const int total_ok =
      1 + replicate_put_range(placement, oid, offset, data, len, attrs, replace_attrs);
  if (total_ok < quorum_need(placement)) {
    return reply_err(map_.epoch, "quorum_failed", "quorum failed");
  }
  auto f = reply_ok(map_.epoch);
  f.body["replicas"] = total_ok;
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
  std::string err;
  auto data = store->get(oid, err);
  if (!data) return reply_err(map_.epoch, "not_found", err);
  auto f = reply_ok(map_.epoch);
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
  if (role == "replica") {
    if (!in_acting_set(oid, map_, cfg_.node_id, aios_path)) {
      return reply_err(map_.epoch, "not_replica", "not in acting set");
    }
    std::string err;
    if (!local_del(aios_path, oid, err) && err != "object not found") {
      return reply_err(map_.epoch, "store_error", err);
    }
    return reply_ok(map_.epoch);
  }
  if (!is_primary_for(oid, map_, cfg_.node_id, aios_path)) {
    return reply_err(map_.epoch, "not_primary", "not primary");
  }
  std::string err;
  if (!local_del(aios_path, oid, err) && err != "object not found") {
    return reply_err(map_.epoch, "store_error", err);
  }
  const int total_ok = 1 + replicate_del(placement, oid);
  if (total_ok < quorum_need(placement)) {
    return reply_err(map_.epoch, "quorum_failed", "quorum failed");
  }
  return reply_ok(map_.epoch);
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
  std::string err;
  auto info = store->stat(oid, err);
  if (!info) return reply_err(map_.epoch, "not_found", err);
  auto f = reply_ok(map_.epoch);
  f.body["size"] = info->size;
  f.body["mtime_ms"] = info->mtime_ms;
  f.body["ctime_ms"] = info->ctime_ms;
  f.body["shard"] = info->shard;
  f.body["inline_body"] = info->inline_body;
  return f;
}

ApiResult ObjectService::api_put(const std::string& oid, const std::uint8_t* data,
                                std::size_t len,
                                const std::unordered_map<std::string, std::string>& attrs,
                                bool replace_attrs,
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

  if (!store->put(oid, data, len, attrs, replace_attrs, err)) {
    return fail("store_error", err);
  }
  const int total_ok = 1 + replicate_put(placement, oid, data, len, attrs);
  if (total_ok < quorum_need(placement)) return fail("quorum_failed", "quorum failed");
  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  r.replicas = total_ok;
  r.placement = placement;
  return r;
}

ApiResult ObjectService::api_put_range(
    const std::string& oid, std::uint64_t offset, const std::uint8_t* data, std::size_t len,
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

  if (!store->put_range(oid, offset, data, len, attrs, replace_attrs, err)) {
    return fail("store_error", err);
  }
  const int total_ok =
      1 + replicate_put_range(placement, oid, offset, data, len, attrs, replace_attrs);
  if (total_ok < quorum_need(placement)) return fail("quorum_failed", "quorum failed");
  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  r.replicas = total_ok;
  r.placement = placement;
  return r;
}

ApiResult ObjectService::api_get(const std::string& oid, std::optional<std::uint64_t> offset,
                                std::optional<std::uint64_t> end_inclusive,
                                const std::vector<AttrPrecondition>& preds) {
  std::lock_guard lock(mu_);
  auto placement = place(oid, map_);
  if (placement.acting_set.empty()) return fail("no_targets", "no storage targets");
  // Prefer primary; allow any local acting-set member.
  ObjectStore* store = nullptr;
  std::string err;
  for (const auto& t : placement.acting_set) {
    if (t.node_id != cfg_.node_id) continue;
    store = stores_.get(t.aios_path);
    if (store) break;
  }
  if (!store) return fail("not_local", "no local replica for oid");
  auto pr = check_preds_on(store, oid, preds, err);
  if (pr == PrecondResult::NotFound) return fail("not_found", err);
  if (pr == PrecondResult::Conflict) return fail("precondition_failed", err);

  auto info = store->stat(oid, err);
  if (!info) return fail("not_found", err);

  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  r.info = info;
  r.attrs = store->list_attrs(oid, err);
  r.placement = placement;

  if (!offset.has_value()) {
    r.data = store->get(oid, err);
    if (!r.data) return fail("not_found", err);
    return r;
  }
  if (*offset >= info->size) return fail("range_unsatisfiable", "range unsatisfiable");
  std::uint64_t end = end_inclusive.value_or(info->size - 1);
  if (end >= info->size) end = info->size - 1;
  if (end < *offset) return fail("range_unsatisfiable", "range unsatisfiable");
  const std::size_t len = static_cast<std::size_t>(end - *offset + 1);
  r.data = store->get_range(oid, *offset, len, err);
  if (!r.data) {
    if (err == "range unsatisfiable") return fail("range_unsatisfiable", err);
    return fail("store_error", err);
  }
  return r;
}

ApiResult ObjectService::api_head(const std::string& oid,
                                 const std::vector<AttrPrecondition>& preds) {
  return api_get(oid, std::nullopt, std::nullopt, preds);
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

  if (!store->del(oid, err) && err != "object not found") return fail("store_error", err);
  const int total_ok = 1 + replicate_del(placement, oid);
  if (total_ok < quorum_need(placement)) return fail("quorum_failed", "quorum failed");
  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  r.replicas = total_ok;
  return r;
}

ApiResult ObjectService::api_list(const std::string& prefix, const std::string& attr_eq_key,
                                  const std::string& attr_eq_value, std::size_t limit,
                                  const std::string& cursor, bool include_attrs) {
  std::lock_guard lock(mu_);
  // LIST aggregates local stores only (this node's targets).
  ApiResult r;
  r.ok = true;
  r.epoch = map_.epoch;
  for (const auto& path : stores_.paths()) {
    auto* store = stores_.get(path);
    if (!store) continue;
    std::string err;
    auto part = store->list(prefix, attr_eq_key, attr_eq_value, limit, cursor, include_attrs,
                            err);
    if (!err.empty() && r.list.objects.empty()) {
      return fail("store_error", err);
    }
    for (auto& o : part.objects) {
      r.list.objects.push_back(std::move(o));
      if (limit > 0 && r.list.objects.size() >= limit) {
        r.list.next_cursor = path + "|" + part.next_cursor;
        return r;
      }
    }
    if (!part.next_cursor.empty()) {
      r.list.next_cursor = path + "|" + part.next_cursor;
      return r;
    }
  }
  return r;
}

}  // namespace aios
