#include "metrics/ops_counters.hpp"

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

void prom_counter(std::ostringstream& os, const char* name, const char* help,
                  const std::string& node_id, std::uint64_t v) {
  os << "# HELP " << name << ' ' << help << '\n';
  os << "# TYPE " << name << " counter\n";
  os << name << "{node_id=\"" << node_id << "\"} " << v << '\n';
}

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

std::string OpsCounters::to_prometheus(const std::string& node_id) const {
  std::ostringstream os;
  prom_counter(os, "aios_http_requests_total", "HTTP requests handled", node_id,
               http_requests.load());
  prom_counter(os, "aios_ops_put_total", "Successful object puts", node_id, put.load());
  prom_counter(os, "aios_ops_put_range_total", "Successful ranged puts", node_id,
               put_range.load());
  prom_counter(os, "aios_ops_append_total", "Successful atomic appends", node_id, append.load());
  prom_counter(os, "aios_ops_get_total", "Successful object gets", node_id, get.load());
  prom_counter(os, "aios_ops_head_total", "Successful object heads", node_id, head.load());
  prom_counter(os, "aios_ops_del_total", "Successful object deletes", node_id, del.load());
  prom_counter(os, "aios_ops_list_total", "Successful object lists", node_id, list.load());
  prom_counter(os, "aios_ops_put_bytes_total", "Bytes accepted on put/put_file", node_id,
               put_bytes.load());
  prom_counter(os, "aios_ops_get_bytes_total", "Bytes returned on get", node_id, get_bytes.load());
  prom_counter(os, "aios_ops_append_bytes_total", "Bytes appended", node_id, append_bytes.load());
  prom_counter(os, "aios_ops_lock_acquire_total", "Successful lock acquires", node_id,
               lock_acquire.load());
  prom_counter(os, "aios_ops_watch_total", "Watch polls completed with an event", node_id,
               watch.load());
  prom_counter(os, "aios_ops_pubsub_publish_total", "Pub/sub publishes", node_id,
               pubsub_publish.load());
  prom_counter(os, "aios_gossip_rounds_total", "Gossip timer rounds", node_id,
               gossip_rounds.load());
  prom_counter(os, "aios_repair_scanned_total", "Oids scanned by repair", node_id,
               repair_scanned.load());
  prom_counter(os, "aios_repair_repaired_total", "Oids repaired", node_id, repair_repaired.load());
  prom_counter(os, "aios_repair_failed_total", "Repair failures", node_id, repair_failed.load());
  prom_counter(os, "aios_ops_errors_total", "Failed object API results", node_id, errors.load());
  return os.str();
}

}  // namespace aios
