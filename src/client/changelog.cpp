#include "client/changelog.hpp"

#include "client/wire.hpp"

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <thread>

namespace aios {
namespace changelog {
namespace {

struct LogLockGuard {
  Session* session{nullptr};
  std::string oid;
  std::string token;

  LogLockGuard(Session& s, const std::string& oid_in) : session(&s), oid(oid_in) {
    token = s.lock_acquire(oid_in).token;
  }

  ~LogLockGuard() {
    if (session && !token.empty()) {
      try {
        session->lock_release(oid, token);
      } catch (...) {
      }
    }
  }

  LogLockGuard(const LogLockGuard&) = delete;
  LogLockGuard& operator=(const LogLockGuard&) = delete;
};

bool valid_op(std::uint32_t op_u) {
  return op_u >= static_cast<std::uint32_t>(Op::Put) &&
         op_u <= static_cast<std::uint32_t>(Op::Compact);
}

void append_u32(std::string& out, std::uint32_t v) {
  out.push_back(static_cast<char>(v & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
  out.push_back(static_cast<char>((v >> 16) & 0xff));
  out.push_back(static_cast<char>((v >> 24) & 0xff));
}

void append_u64(std::string& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

bool read_u32(const std::string& buf, std::size_t& i, std::uint32_t& v) {
  if (i + 4 > buf.size()) return false;
  v = static_cast<std::uint8_t>(buf[i]) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[i + 1])) << 8) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[i + 2])) << 16) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[i + 3])) << 24);
  i += 4;
  return true;
}

bool read_u64(const std::string& buf, std::size_t& i, std::uint64_t& v) {
  if (i + 8 > buf.size()) return false;
  v = 0;
  for (int b = 0; b < 8; ++b) {
    v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[i + static_cast<std::size_t>(b)]))
         << (8 * b);
  }
  i += 8;
  return true;
}

void append_str(std::string& out, const std::string& s) {
  if (s.size() > 0xffffffffu) throw client_error("payload_too_large", "changelog string too large");
  append_u32(out, static_cast<std::uint32_t>(s.size()));
  out.append(s);
}

bool read_str(const std::string& buf, std::size_t& i, std::string& s) {
  std::uint32_t n = 0;
  if (!read_u32(buf, i, n)) return false;
  if (i + n > buf.size()) return false;
  s.assign(buf.data() + i, n);
  i += n;
  return true;
}

nlohmann::json make_meta_json(const Meta& m, sync_mode mode, const std::string& snap) {
  return nlohmann::json{{"aios_stl", kMetaVersion},
                        {"type", m.type},
                        {"mode_hint", wire::mode_name(mode)},
                        {"next_op", m.next_op},
                        {"log_bytes", m.log_bytes},
                        {"snapshot_op", m.snapshot_op},
                        {"snapshot_oid", snap}};
}

Meta parse_meta_json(const std::string& body, const char* expect_type) {
  auto j = wire::parse_json(body);
  if (!j.is_object() || j.value("aios_stl", 0) != kMetaVersion) {
    throw client_error("bad_request", "invalid aios_stl meta (want v2)");
  }
  if (j.value("type", "") != expect_type) {
    throw client_error("bad_request", std::string("stl type mismatch, expected ") + expect_type);
  }
  Meta m;
  m.type = expect_type;
  m.exists = true;
  m.next_op = j.value("next_op", static_cast<std::uint64_t>(1));
  m.log_bytes = j.value("log_bytes", static_cast<std::uint64_t>(0));
  m.snapshot_op = j.value("snapshot_op", static_cast<std::uint64_t>(0));
  return m;
}

}  // namespace

std::string encode_record(const Record& rec) {
  std::string payload;
  for (const auto& a : rec.args) append_str(payload, a);

  std::string header;
  append_u64(header, rec.op_id);
  append_u32(header, static_cast<std::uint32_t>(rec.op));
  append_u32(header, static_cast<std::uint32_t>(payload.size()));

  std::string out;
  append_u32(out, kMagic);
  append_u32(out, static_cast<std::uint32_t>(header.size()));
  out.append(header);
  out.append(payload);
  return out;
}

std::size_t decode_records(const std::string& buf, std::vector<Record>& out) {
  std::size_t i = 0;
  while (i + 8 <= buf.size()) {
    const std::size_t start = i;
    std::uint32_t magic = 0;
    std::uint32_t header_len = 0;
    if (!read_u32(buf, i, magic) || magic != kMagic) {
      throw client_error("bad_request", "changelog magic mismatch");
    }
    if (!read_u32(buf, i, header_len)) return start;
    if (header_len != 16) {
      throw client_error("bad_request", "changelog header_len must be 16");
    }
    if (i + header_len > buf.size()) return start;
    std::size_t h = i;
    Record rec;
    std::uint32_t op_u = 0;
    std::uint32_t payload_len = 0;
    if (!read_u64(buf, h, rec.op_id) || !read_u32(buf, h, op_u) || !read_u32(buf, h, payload_len)) {
      throw client_error("bad_request", "changelog header truncated");
    }
    if (rec.op_id == 0) {
      throw client_error("bad_request", "changelog invalid op_id");
    }
    if (!valid_op(op_u)) {
      throw client_error("bad_request", "changelog invalid op");
    }
    i += header_len;
    if (i + payload_len > buf.size()) return start;
    const std::string payload = buf.substr(i, payload_len);
    i += payload_len;
    rec.op = static_cast<Op>(op_u);
    std::size_t p = 0;
    while (p < payload.size()) {
      std::string s;
      if (!read_str(payload, p, s)) {
        throw client_error("bad_request", "changelog payload truncated");
      }
      rec.args.push_back(std::move(s));
    }
    out.push_back(std::move(rec));
  }
  return i;
}

std::string meta_oid(const std::string& type, const std::string& name) {
  return Session::stl_oid(type, name);
}

std::string log_oid(const std::string& type, const std::string& name) {
  return Session::stl_oid(type, name) + "/log";
}

std::string snap_oid(const std::string& type, const std::string& name) {
  return Session::stl_oid(type, name) + "/snap";
}

Log::Log(Session& session, std::string type, std::string name)
    : session_(&session),
      type_(std::move(type)),
      name_(std::move(name)),
      meta_oid_(changelog::meta_oid(type_, name_)),
      log_oid_(changelog::log_oid(type_, name_)),
      snap_oid_(changelog::snap_oid(type_, name_)) {}

void Log::cas_put_meta(Meta& m, sync_mode mode, const std::optional<std::string>& lock_token) {
  const auto body = make_meta_json(m, mode, snap_oid_).dump();
  m.cas = session_->put_object(meta_oid_, body, type_, m.cas, lock_token, kMetaVersion);
  m.exists = true;
}

void Log::store_meta(Meta& m, sync_mode mode) { cas_put_meta(m, mode); }

void Log::ensure_meta(Meta& m, sync_mode mode) {
  if (m.exists) return;
  m.type = type_;
  m.next_op = 1;
  m.log_bytes = 0;
  m.snapshot_op = 0;
  m.cas = 0;
  cas_put_meta(m, mode);
}

void Log::migrate_v1(const std::string& v1_body, std::uint64_t cas, sync_mode mode) {
  session_->put_object(snap_oid_, v1_body, type_, 0, std::nullopt, 1);
  session_->put_object(log_oid_, "", type_ + ".log", 0, std::nullopt, kMetaVersion);
  Meta m;
  m.type = type_;
  m.next_op = 1;
  m.log_bytes = 0;
  m.snapshot_op = 0;
  m.cas = cas;
  cas_put_meta(m, mode);
}

Meta Log::open(sync_mode mode) {
  auto snap = session_->get_object(meta_oid_);
  if (!snap.exists) {
    Meta m;
    m.type = type_;
    return m;
  }
  auto j = wire::parse_json(snap.body);
  const int ver = j.value("aios_stl", 0);
  if (ver == 1) {
    migrate_v1(snap.body, snap.cas, mode);
    return load_meta();
  }
  if (ver != kMetaVersion) {
    throw client_error("bad_request", "unsupported aios_stl version");
  }
  Meta m = parse_meta_json(snap.body, type_.c_str());
  m.cas = snap.cas;
  return m;
}

Meta Log::load_meta() {
  auto snap = session_->get_object(meta_oid_);
  Meta m;
  m.type = type_;
  if (!snap.exists) return m;
  auto j = wire::parse_json(snap.body);
  if (j.value("aios_stl", 0) == 1) {
    throw client_error("stl_v1", "v1 tip; call open()");
  }
  m = parse_meta_json(snap.body, type_.c_str());
  m.cas = snap.cas;
  return m;
}

std::string Log::load_snapshot_body() {
  auto snap = session_->get_object(snap_oid_);
  if (!snap.exists) return {};
  return snap.body;
}

Meta Log::pull(std::uint64_t* applied_op,
               const std::function<void(const std::string&)>& on_snapshot,
               const std::function<void(const Record&)>& apply) {
  Meta m = open(sync_mode::async);
  if (!m.exists) return m;

  if (*applied_op == 0) {
    const auto body = load_snapshot_body();
    if (!body.empty()) on_snapshot(body);
  }
  if (m.snapshot_op > 0 && *applied_op < m.snapshot_op) {
    const auto body = load_snapshot_body();
    if (!body.empty()) on_snapshot(body);
    *applied_op = m.snapshot_op;
  }

  auto head = session_->head_object(log_oid_);
  const std::uint64_t log_size = head.exists ? head.size : 0;
  if (log_size == 0) {
    m.log_bytes = 0;
    return m;
  }

  // Cursor: re-read full log and skip applied (logs stay small via compact).
  auto ranged = session_->get_range(log_oid_, 0, log_size - 1);
  if (!ranged.exists) return m;
  std::vector<Record> recs;
  const std::size_t consumed = decode_records(ranged.body, recs);
  if (consumed != ranged.body.size()) {
    throw client_error("bad_request", "incomplete changelog record");
  }
  std::sort(recs.begin(), recs.end(),
            [](const Record& a, const Record& b) { return a.op_id < b.op_id; });
  for (const auto& r : recs) {
    if (r.op_id <= *applied_op) continue;
    if (r.op_id == 0 || r.op_id >= m.next_op) {
      throw client_error("bad_request", "changelog invalid op_id");
    }
    if (r.op_id != *applied_op + 1) break;
    if (r.op == Op::Compact) {
      *applied_op = r.op_id;
      continue;
    }
    apply(r);
    *applied_op = r.op_id;
  }
  m.log_bytes = log_size;
  return m;
}

std::uint64_t Log::append_op(Op op, std::vector<std::string> args, sync_mode mode) {
  for (int attempt = 0; attempt < 16; ++attempt) {
    Meta m = open(mode);
    ensure_meta(m, mode);

    const std::uint64_t op_id = m.next_op;
    Record rec;
    rec.op_id = op_id;
    rec.op = op;
    rec.args = args;
    const std::string framed = encode_record(rec);

    Meta reserved = m;
    reserved.next_op = op_id + 1;
    try {
      cas_put_meta(reserved, mode);
    } catch (const client_error& e) {
      if (e.code() == "conflict") continue;
      throw;
    }

    AppendResult ar;
    for (int ltry = 0;; ++ltry) {
      try {
        ar = session_->append(log_oid_, framed);
        break;
      } catch (const client_error& e) {
        // Compact holds the log lock. Retry the same reserved op_id — do not
        // re-CAS meta or we leave a hole that stalls pull's contiguous apply.
        if (e.code() != "lock_held" || ltry >= 64) throw;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    for (int mtry = 0; mtry < 8; ++mtry) {
      Meta cur = load_meta();
      cur.log_bytes = ar.size;
      if (cur.next_op < op_id + 1) cur.next_op = op_id + 1;
      try {
        cas_put_meta(cur, mode);
        if (cur.log_bytes > kAutoCompactBytes) {
          // Caller may compact; leave threshold signal via log_bytes.
        }
        return op_id;
      } catch (const client_error& e) {
        if (e.code() == "conflict") continue;
        throw;
      }
    }
    return op_id;
  }
  throw client_error("conflict", "changelog append_op exhausted retries");
}

std::uint64_t Log::append_ops(std::vector<Record> records, sync_mode mode) {
  if (records.empty()) return 0;
  for (int attempt = 0; attempt < 16; ++attempt) {
    Meta m = open(mode);
    ensure_meta(m, mode);
    std::uint64_t next = m.next_op;
    std::string batch;
    for (auto& r : records) {
      r.op_id = next++;
      batch.append(encode_record(r));
    }
    Meta reserved = m;
    reserved.next_op = next;
    try {
      cas_put_meta(reserved, mode);
    } catch (const client_error& e) {
      if (e.code() == "conflict") continue;
      throw;
    }
    AppendResult ar;
    for (int ltry = 0;; ++ltry) {
      try {
        ar = session_->append(log_oid_, batch);
        break;
      } catch (const client_error& e) {
        if (e.code() != "lock_held" || ltry >= 64) throw;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    for (int mtry = 0; mtry < 8; ++mtry) {
      Meta cur = load_meta();
      cur.log_bytes = ar.size;
      if (cur.next_op < next) cur.next_op = next;
      try {
        cas_put_meta(cur, mode);
        return next - 1;
      } catch (const client_error& e) {
        if (e.code() == "conflict") continue;
        throw;
      }
    }
    return next - 1;
  }
  throw client_error("conflict", "changelog append_ops exhausted retries");
}

void Log::compact(sync_mode mode,
                  const std::function<std::pair<std::string, std::uint64_t>()>& rebuild) {
  LogLockGuard log_lock(*session_, log_oid_);
  const std::optional<std::string> lock_token = log_lock.token;

  Meta m = open(mode);
  ensure_meta(m, mode);

  // Peer appends are blocked by the log lock; rebuild sees a stable tip.
  const auto [snapshot_json, applied_op] = rebuild();

  auto snap_head = session_->head_object(snap_oid_);
  session_->put_object(snap_oid_, snapshot_json, type_, snap_head.cas, lock_token, 1);

  // next_op must stay contiguous with snapshot_op. Bumping past an unused
  // "fence" id (without writing a Compact record) left a hole that made pull
  // stop applying later ops after a rebuild-under-lock.
  const std::uint64_t fence_id = m.next_op;
  const std::uint64_t snap_op = applied_op > 0 ? applied_op : (fence_id > 0 ? fence_id - 1 : 0);
  const std::uint64_t new_next = std::max(fence_id, snap_op + 1);

  Meta reserved = m;
  reserved.next_op = new_next;
  reserved.snapshot_op = snap_op;
  reserved.log_bytes = 0;
  cas_put_meta(reserved, mode, lock_token);

  auto log_head = session_->head_object(log_oid_);
  session_->put_object(log_oid_, "", type_ + ".log", log_head.cas, lock_token, kMetaVersion);

  Meta cur = load_meta();
  cur.log_bytes = 0;
  cur.snapshot_op = snap_op;
  if (cur.next_op < new_next) cur.next_op = new_next;
  cas_put_meta(cur, mode, lock_token);
}

}  // namespace changelog
}  // namespace aios
