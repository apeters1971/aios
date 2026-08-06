#pragma once

#include "client/error.hpp"
#include "client/mode.hpp"
#include "client/session.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace aios {
namespace changelog {

inline constexpr std::uint32_t kMagic = 0x6b504f41u;  // 'AOPk' LE
inline constexpr int kMetaVersion = 2;
inline constexpr std::uint64_t kAutoCompactBytes = 1u * 1024u * 1024u;

enum class Op : std::uint32_t {
  Put = 1,
  Erase = 2,
  Clear = 3,
  Insert = 4,
  PushBack = 5,
  PushFront = 6,
  PopBack = 7,
  PopFront = 8,
  SetAt = 9,
  EraseAt = 10,
  Compact = 11,
};

struct Record {
  std::uint64_t op_id{0};
  Op op{Op::Clear};
  std::vector<std::string> args;
};

struct Meta {
  std::string type;
  std::uint64_t next_op{1};
  std::uint64_t log_bytes{0};
  std::uint64_t snapshot_op{0};
  std::uint64_t cas{0};
  bool exists{false};
};

std::string encode_record(const Record& rec);
// Decode complete records; returns bytes consumed (prefix of buf).
std::size_t decode_records(const std::string& buf, std::vector<Record>& out);

std::string meta_oid(const std::string& type, const std::string& name);
std::string log_oid(const std::string& type, const std::string& name);
std::string snap_oid(const std::string& type, const std::string& name);

// Append-only changelog for one named STL container (map/set/list/deque/unordered_map).
class Log {
 public:
  Log(Session& session, std::string type, std::string name);

  const std::string& type() const { return type_; }
  const std::string& name() const { return name_; }
  const std::string& meta_oid() const { return meta_oid_; }
  const std::string& log_oid() const { return log_oid_; }
  const std::string& snap_oid() const { return snap_oid_; }

  // Load meta; if tip is aios_stl:1, migrate using v1_body as snapshot seed.
  Meta open(sync_mode mode);

  Meta load_meta();
  void store_meta(Meta& m, sync_mode mode);

  // Apply records with op_id > *applied_op. May reset *applied_op to snapshot_op
  // and invoke on_snapshot(snapshot_json) when a reload from snap is required.
  Meta pull(std::uint64_t* applied_op, const std::function<void(const std::string&)>& on_snapshot,
            const std::function<void(const Record&)>& apply);

  std::uint64_t append_op(Op op, std::vector<std::string> args, sync_mode mode);
  // Returns highest op_id written (0 if records empty).
  std::uint64_t append_ops(std::vector<Record> records, sync_mode mode);

  // Acquire the log lock, invoke `rebuild` (which must refresh local state via pull
  // and return snapshot_json + applied_op), then write the snapshot and truncate
  // the log. Building the snapshot under the lock closes the race where a peer
  // append lands after a stale snapshot was taken but before truncate.
  void compact(sync_mode mode,
               const std::function<std::pair<std::string, std::uint64_t>()>& rebuild);

  std::string load_snapshot_body();

 private:
  void ensure_meta(Meta& m, sync_mode mode);
  void cas_put_meta(Meta& m, sync_mode mode,
                    const std::optional<std::string>& lock_token = std::nullopt);
  void migrate_v1(const std::string& v1_body, std::uint64_t cas, sync_mode mode);

  Session* session_;
  std::string type_;
  std::string name_;
  std::string meta_oid_;
  std::string log_oid_;
  std::string snap_oid_;
};

}  // namespace changelog
}  // namespace aios
