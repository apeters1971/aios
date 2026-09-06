#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aios {

enum class MsgType : std::uint8_t {
  Hello = 1,
  Membership = 2,
  FsTable = 3,
  Gossip = 4,
  Ping = 5,
  Pong = 6,
  ObjectPut = 7,
  ObjectGet = 8,
  ObjectDel = 9,
  ObjectStat = 10,
  ObjectReply = 11,
  ObjectPutRange = 12,
  ObjectPublishTip = 13,
  ObjectAbortVersion = 14,
  ObjectListVersions = 15,
  ObjectPurgeVersions = 16,
  ObjectStageBegin = 17,   // start FS body staging for install
  ObjectStageData = 18,    // raw chunk (kFlagRawBody)
  ObjectStageCommit = 19,  // finalize staged file → install_version
  ObjectList = 20,         // list tip objects on a node (local stores)
  MapRpc = 21,             // cluster-map consensus (vote / append); reply is ObjectReply
  ObjectInstallRange = 22, // replica applies a ranged write over its tip (raw body = the write)
};

constexpr std::uint8_t kProtoVersion = 1;
constexpr char kMagic[4] = {'A', 'I', 'O', 'S'};
constexpr std::size_t kHeaderSize = 12;
// Max TCP++ frame body (JSON or json+raw chunk). Object bodies may be larger via staging.
// Must exceed kStageChunkSize by the [u32be json_len][json] envelope (stage/get-range).
constexpr std::size_t kMaxBodySize = 32u * 1024u * 1024u;
// One chunk per typical large object keeps install RTTs down (was 4 MiB).
constexpr std::size_t kStageChunkSize = 16u * 1024u * 1024u;
// flags bit0: body is [u32be json_len][json][raw]
constexpr std::uint16_t kFlagRawBody = 0x0001;

struct Frame {
  MsgType type{MsgType::Ping};
  std::uint16_t flags{0};
  nlohmann::json body = nlohmann::json::object();
  // Optional binary trailer (ObjectStageData / ObjectPutRange / ranged get).
  // When raw_off > 0, `raw` owns [json envelope][payload] and the payload starts
  // at raw_off — used by read_frame to avoid a full-body memcpy on decode.
  // raw_ext_* references caller-owned bytes for zero-copy sends (shared fan-out).
  std::vector<std::uint8_t> raw;
  std::size_t raw_off{0};
  const std::uint8_t* raw_ext{nullptr};
  std::size_t raw_ext_len{0};

  const std::uint8_t* raw_data() const noexcept {
    if (raw_ext) return raw_ext_len ? raw_ext : nullptr;
    return raw.size() <= raw_off ? nullptr : raw.data() + raw_off;
  }
  std::size_t raw_size() const noexcept {
    if (raw_ext) return raw_ext_len;
    return raw.size() <= raw_off ? 0 : raw.size() - raw_off;
  }
  bool raw_empty() const noexcept { return raw_size() == 0; }

  void clear_raw() {
    raw.clear();
    raw_off = 0;
    raw_ext = nullptr;
    raw_ext_len = 0;
  }

  // Slide payload to offset 0 (memmove). Call before handing `raw` to APIs that
  // expect a bare buffer.
  void compact_raw() {
    if (raw_off == 0) return;
    if (raw_off >= raw.size()) {
      clear_raw();
      return;
    }
    raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(raw_off));
    raw_off = 0;
  }
};

// Encode frame into bytes (header + body).
std::vector<std::uint8_t> encode_frame(const Frame& frame);

// Decode a complete frame from buffer. Returns false if incomplete or invalid.
// On success, consumed is set to bytes used.
bool decode_frame(const std::uint8_t* data, std::size_t len, Frame& out,
                  std::size_t& consumed, std::string& err);

const char* msg_type_name(MsgType t);

}  // namespace aios
