#include "net/framing.hpp"

#include <arpa/inet.h>

#include <cstring>
#include <stdexcept>

namespace aios {

const char* msg_type_name(MsgType t) {
  switch (t) {
    case MsgType::Hello:
      return "Hello";
    case MsgType::Membership:
      return "Membership";
    case MsgType::FsTable:
      return "FsTable";
    case MsgType::Gossip:
      return "Gossip";
    case MsgType::Ping:
      return "Ping";
    case MsgType::Pong:
      return "Pong";
  }
  return "Unknown";
}

std::vector<std::uint8_t> encode_frame(const Frame& frame) {
  const std::string body = frame.body.dump();
  if (body.size() > kMaxBodySize) {
    throw std::runtime_error("frame body too large");
  }
  std::vector<std::uint8_t> out(kHeaderSize + body.size());
  std::memcpy(out.data(), kMagic, 4);
  out[4] = kProtoVersion;
  out[5] = static_cast<std::uint8_t>(frame.type);
  const std::uint16_t flags_be = htons(frame.flags);
  std::memcpy(out.data() + 6, &flags_be, 2);
  const std::uint32_t len_be = htonl(static_cast<std::uint32_t>(body.size()));
  std::memcpy(out.data() + 8, &len_be, 4);
  if (!body.empty()) {
    std::memcpy(out.data() + kHeaderSize, body.data(), body.size());
  }
  return out;
}

bool decode_frame(const std::uint8_t* data, std::size_t len, Frame& out,
                  std::size_t& consumed, std::string& err) {
  consumed = 0;
  if (len < kHeaderSize) {
    err = "incomplete header";
    return false;
  }
  if (std::memcmp(data, kMagic, 4) != 0) {
    err = "bad magic";
    return false;
  }
  if (data[4] != kProtoVersion) {
    err = "unsupported version";
    return false;
  }
  const auto type = static_cast<MsgType>(data[5]);
  std::uint16_t flags_be = 0;
  std::memcpy(&flags_be, data + 6, 2);
  const std::uint16_t flags = ntohs(flags_be);
  std::uint32_t len_be = 0;
  std::memcpy(&len_be, data + 8, 4);
  const std::uint32_t body_len = ntohl(len_be);
  if (body_len > kMaxBodySize) {
    err = "body too large";
    return false;
  }
  if (len < kHeaderSize + body_len) {
    err = "incomplete body";
    return false;
  }
  nlohmann::json body = nlohmann::json::object();
  if (body_len > 0) {
    try {
      body = nlohmann::json::parse(
          reinterpret_cast<const char*>(data + kHeaderSize),
          reinterpret_cast<const char*>(data + kHeaderSize + body_len));
    } catch (const std::exception& e) {
      err = std::string("json parse: ") + e.what();
      return false;
    }
  }
  out.type = type;
  out.flags = flags;
  out.body = std::move(body);
  consumed = kHeaderSize + body_len;
  err.clear();
  return true;
}

}  // namespace aios
