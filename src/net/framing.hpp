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
};

constexpr std::uint8_t kProtoVersion = 1;
constexpr char kMagic[4] = {'A', 'I', 'O', 'S'};
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kMaxBodySize = 16u * 1024u * 1024u;

struct Frame {
  MsgType type{MsgType::Ping};
  std::uint16_t flags{0};
  nlohmann::json body = nlohmann::json::object();
};

// Encode frame into bytes (header + body).
std::vector<std::uint8_t> encode_frame(const Frame& frame);

// Decode a complete frame from buffer. Returns false if incomplete or invalid.
// On success, consumed is set to bytes used.
bool decode_frame(const std::uint8_t* data, std::size_t len, Frame& out,
                  std::size_t& consumed, std::string& err);

const char* msg_type_name(MsgType t);

}  // namespace aios
