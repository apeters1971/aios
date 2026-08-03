#include "metrics/ops_counters.hpp"

#include "metrics/app_label.hpp"

#include <sstream>

namespace aios {
namespace {

std::uint64_t j_u64(const nlohmann::json& j, const char* key) {
  if (!j.contains(key)) return 0;
  try {
    return j.at(key).get<std::uint64_t>();
  } catch (...) {
    return 0;
  }
}

void prom_line(std::ostringstream& os, const char* name, const std::string& node_id,
               const std::string* app_label, std::uint64_t v) {
  os << name << "{node_id=\"" << node_id << '"';
  if (app_label && !app_label->empty()) {
    os << ",app_label=\"" << *app_label << '"';
  }
  os << "} " << v << '\n';
}

void prom_counter_family(std::ostringstream& os, const char* name, const char* help,
                         const std::string& node_id, std::uint64_t total_v,
                         const std::vector<std::pair<std::string, std::uint64_t>>& labeled) {
  os << "# HELP " << name << ' ' << help << '\n';
  os << "# TYPE " << name << " counter\n";
  prom_line(os, name, node_id, nullptr, total_v);
  for (const auto& [label, v] : labeled) {
    prom_line(os, name, node_id, &label, v);
  }
}

using LabelSnap = std::vector<std::pair<std::string, const OpsCounters*>>;

}  // namespace

nlohmann::json OpsCounters::to_json() const {
  return nlohmann::json{
      {"http_requests", http_requests.load()},
      {"put", put.load()},
      {"put_range", put_range.load()},
      {"append", append.load()},
      {"get", get.load()},
      {"head", head.load()},
      {"del", del.load()},
      {"list", list.load()},
      {"put_bytes", put_bytes.load()},
      {"get_bytes", get_bytes.load()},
      {"append_bytes", append_bytes.load()},
      {"lock_acquire", lock_acquire.load()},
      {"watch", watch.load()},
      {"pubsub_publish", pubsub_publish.load()},
      {"gossip_rounds", gossip_rounds.load()},
      {"repair_scanned", repair_scanned.load()},
      {"repair_repaired", repair_repaired.load()},
      {"repair_failed", repair_failed.load()},
      {"errors", errors.load()},
  };
}

void OpsCounters::load_json(const nlohmann::json& j) {
  if (!j.is_object()) return;
  http_requests.store(j_u64(j, "http_requests"));
  put.store(j_u64(j, "put"));
  put_range.store(j_u64(j, "put_range"));
  append.store(j_u64(j, "append"));
  get.store(j_u64(j, "get"));
  head.store(j_u64(j, "head"));
  del.store(j_u64(j, "del"));
  list.store(j_u64(j, "list"));
  put_bytes.store(j_u64(j, "put_bytes"));
  get_bytes.store(j_u64(j, "get_bytes"));
  append_bytes.store(j_u64(j, "append_bytes"));
  lock_acquire.store(j_u64(j, "lock_acquire"));
  watch.store(j_u64(j, "watch"));
  pubsub_publish.store(j_u64(j, "pubsub_publish"));
  gossip_rounds.store(j_u64(j, "gossip_rounds"));
  repair_scanned.store(j_u64(j, "repair_scanned"));
  repair_repaired.store(j_u64(j, "repair_repaired"));
  repair_failed.store(j_u64(j, "repair_failed"));
  errors.store(j_u64(j, "errors"));
}

void OpsCounters::add_from(const OpsCounters& o) {
  http_requests.fetch_add(o.http_requests.load());
  put.fetch_add(o.put.load());
  put_range.fetch_add(o.put_range.load());
  append.fetch_add(o.append.load());
  get.fetch_add(o.get.load());
  head.fetch_add(o.head.load());
  del.fetch_add(o.del.load());
  list.fetch_add(o.list.load());
  put_bytes.fetch_add(o.put_bytes.load());
  get_bytes.fetch_add(o.get_bytes.load());
  append_bytes.fetch_add(o.append_bytes.load());
  lock_acquire.fetch_add(o.lock_acquire.load());
  watch.fetch_add(o.watch.load());
  pubsub_publish.fetch_add(o.pubsub_publish.load());
  gossip_rounds.fetch_add(o.gossip_rounds.load());
  repair_scanned.fetch_add(o.repair_scanned.load());
  repair_repaired.fetch_add(o.repair_repaired.load());
  repair_failed.fetch_add(o.repair_failed.load());
  errors.fetch_add(o.errors.load());
}

OpsCounters* OpsRegistry::label_bucket() {
  const auto& label = AppLabelScope::current();
  if (label.empty()) return nullptr;
  std::lock_guard lock(mu_);
  auto it = by_label_.find(label);
  if (it != by_label_.end()) return it->second.get();
  if (by_label_.size() >= kAppLabelMaxDistinct) {
    auto& slot = by_label_[kAppLabelOverflow];
    if (!slot) slot = std::make_unique<OpsCounters>();
    return slot.get();
  }
  auto [ins, _] = by_label_.emplace(label, std::make_unique<OpsCounters>());
  return ins->second.get();
}

void OpsRegistry::note_http_request() {
  total_.http_requests.fetch_add(1, std::memory_order_relaxed);
  if (auto* b = label_bucket()) b->http_requests.fetch_add(1, std::memory_order_relaxed);
}

void OpsRegistry::note_put(std::uint64_t bytes) {
  total_.put.fetch_add(1, std::memory_order_relaxed);
  total_.put_bytes.fetch_add(bytes, std::memory_order_relaxed);
  if (auto* b = label_bucket()) {
    b->put.fetch_add(1, std::memory_order_relaxed);
    b->put_bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

void OpsRegistry::note_put_range(std::uint64_t bytes) {
  total_.put_range.fetch_add(1, std::memory_order_relaxed);
  total_.put_bytes.fetch_add(bytes, std::memory_order_relaxed);
  if (auto* b = label_bucket()) {
    b->put_range.fetch_add(1, std::memory_order_relaxed);
    b->put_bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

void OpsRegistry::note_append(std::uint64_t bytes) {
  total_.append.fetch_add(1, std::memory_order_relaxed);
  total_.append_bytes.fetch_add(bytes, std::memory_order_relaxed);
  if (auto* b = label_bucket()) {
    b->append.fetch_add(1, std::memory_order_relaxed);
    b->append_bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

void OpsRegistry::note_get(std::uint64_t bytes) {
  total_.get.fetch_add(1, std::memory_order_relaxed);
  total_.get_bytes.fetch_add(bytes, std::memory_order_relaxed);
  if (auto* b = label_bucket()) {
    b->get.fetch_add(1, std::memory_order_relaxed);
    b->get_bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

void OpsRegistry::note_reclass_get_to_head(std::uint64_t get_bytes) {
  total_.get.fetch_sub(1, std::memory_order_relaxed);
  total_.get_bytes.fetch_sub(get_bytes, std::memory_order_relaxed);
  total_.head.fetch_add(1, std::memory_order_relaxed);
  if (auto* b = label_bucket()) {
    b->get.fetch_sub(1, std::memory_order_relaxed);
    b->get_bytes.fetch_sub(get_bytes, std::memory_order_relaxed);
    b->head.fetch_add(1, std::memory_order_relaxed);
  }
}

void OpsRegistry::note_del() {
  total_.del.fetch_add(1, std::memory_order_relaxed);
  if (auto* b = label_bucket()) b->del.fetch_add(1, std::memory_order_relaxed);
}

void OpsRegistry::note_list() {
  total_.list.fetch_add(1, std::memory_order_relaxed);
  if (auto* b = label_bucket()) b->list.fetch_add(1, std::memory_order_relaxed);
}

void OpsRegistry::note_lock_acquire() {
  total_.lock_acquire.fetch_add(1, std::memory_order_relaxed);
  if (auto* b = label_bucket()) b->lock_acquire.fetch_add(1, std::memory_order_relaxed);
}

void OpsRegistry::note_watch() {
  total_.watch.fetch_add(1, std::memory_order_relaxed);
  if (auto* b = label_bucket()) b->watch.fetch_add(1, std::memory_order_relaxed);
}

void OpsRegistry::note_pubsub_publish() {
  total_.pubsub_publish.fetch_add(1, std::memory_order_relaxed);
  if (auto* b = label_bucket()) b->pubsub_publish.fetch_add(1, std::memory_order_relaxed);
}

void OpsRegistry::note_error() {
  total_.errors.fetch_add(1, std::memory_order_relaxed);
  if (auto* b = label_bucket()) b->errors.fetch_add(1, std::memory_order_relaxed);
}

void OpsRegistry::note_gossip_round() {
  total_.gossip_rounds.fetch_add(1, std::memory_order_relaxed);
}

void OpsRegistry::note_repair(std::uint64_t scanned, std::uint64_t repaired,
                              std::uint64_t failed) {
  total_.repair_scanned.fetch_add(scanned, std::memory_order_relaxed);
  total_.repair_repaired.fetch_add(repaired, std::memory_order_relaxed);
  total_.repair_failed.fetch_add(failed, std::memory_order_relaxed);
}

nlohmann::json OpsRegistry::by_label_json() const {
  std::lock_guard lock(mu_);
  nlohmann::json out = nlohmann::json::object();
  for (const auto& [k, v] : by_label_) {
    if (v) out[k] = v->to_json();
  }
  return out;
}

nlohmann::json OpsRegistry::to_admin_json() const {
  return nlohmann::json{{"ops", total_.to_json()}, {"ops_by_label", by_label_json()}};
}

std::string OpsRegistry::to_prometheus(const std::string& node_id) const {
  LabelSnap snap;
  {
    std::lock_guard lock(mu_);
    snap.reserve(by_label_.size());
    for (const auto& [k, v] : by_label_) {
      if (v) snap.emplace_back(k, v.get());
    }
  }

  auto labeled = [&](auto getter) {
    std::vector<std::pair<std::string, std::uint64_t>> out;
    out.reserve(snap.size());
    for (const auto& [k, c] : snap) out.emplace_back(k, getter(*c));
    return out;
  };

  std::ostringstream os;
  prom_counter_family(os, "aios_http_requests_total", "HTTP requests handled", node_id,
                      total_.http_requests.load(),
                      labeled([](const OpsCounters& c) { return c.http_requests.load(); }));
  prom_counter_family(os, "aios_ops_put_total", "Successful object puts", node_id,
                      total_.put.load(),
                      labeled([](const OpsCounters& c) { return c.put.load(); }));
  prom_counter_family(os, "aios_ops_put_range_total", "Successful ranged puts", node_id,
                      total_.put_range.load(),
                      labeled([](const OpsCounters& c) { return c.put_range.load(); }));
  prom_counter_family(os, "aios_ops_append_total", "Successful atomic appends", node_id,
                      total_.append.load(),
                      labeled([](const OpsCounters& c) { return c.append.load(); }));
  prom_counter_family(os, "aios_ops_get_total", "Successful object gets", node_id,
                      total_.get.load(),
                      labeled([](const OpsCounters& c) { return c.get.load(); }));
  prom_counter_family(os, "aios_ops_head_total", "Successful object heads", node_id,
                      total_.head.load(),
                      labeled([](const OpsCounters& c) { return c.head.load(); }));
  prom_counter_family(os, "aios_ops_del_total", "Successful object deletes", node_id,
                      total_.del.load(),
                      labeled([](const OpsCounters& c) { return c.del.load(); }));
  prom_counter_family(os, "aios_ops_list_total", "Successful object lists", node_id,
                      total_.list.load(),
                      labeled([](const OpsCounters& c) { return c.list.load(); }));
  prom_counter_family(os, "aios_ops_put_bytes_total", "Bytes accepted on put/put_file", node_id,
                      total_.put_bytes.load(),
                      labeled([](const OpsCounters& c) { return c.put_bytes.load(); }));
  prom_counter_family(os, "aios_ops_get_bytes_total", "Bytes returned on get", node_id,
                      total_.get_bytes.load(),
                      labeled([](const OpsCounters& c) { return c.get_bytes.load(); }));
  prom_counter_family(os, "aios_ops_append_bytes_total", "Bytes appended", node_id,
                      total_.append_bytes.load(),
                      labeled([](const OpsCounters& c) { return c.append_bytes.load(); }));
  prom_counter_family(os, "aios_ops_lock_acquire_total", "Successful lock acquires", node_id,
                      total_.lock_acquire.load(),
                      labeled([](const OpsCounters& c) { return c.lock_acquire.load(); }));
  prom_counter_family(os, "aios_ops_watch_total", "Watch polls completed with an event", node_id,
                      total_.watch.load(),
                      labeled([](const OpsCounters& c) { return c.watch.load(); }));
  prom_counter_family(os, "aios_ops_pubsub_publish_total", "Pub/sub publishes", node_id,
                      total_.pubsub_publish.load(),
                      labeled([](const OpsCounters& c) { return c.pubsub_publish.load(); }));
  prom_counter_family(os, "aios_gossip_rounds_total", "Gossip timer rounds", node_id,
                      total_.gossip_rounds.load(), {});
  prom_counter_family(os, "aios_repair_scanned_total", "Oids scanned by repair", node_id,
                      total_.repair_scanned.load(), {});
  prom_counter_family(os, "aios_repair_repaired_total", "Oids repaired", node_id,
                      total_.repair_repaired.load(), {});
  prom_counter_family(os, "aios_repair_failed_total", "Repair failures", node_id,
                      total_.repair_failed.load(), {});
  prom_counter_family(os, "aios_ops_errors_total", "Failed object API results", node_id,
                      total_.errors.load(),
                      labeled([](const OpsCounters& c) { return c.errors.load(); }));
  return os.str();
}

}  // namespace aios
