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
    case MsgType::ObjectPut:
      return "ObjectPut";
    case MsgType::ObjectGet:
      return "ObjectGet";
    case MsgType::ObjectDel:
      return "ObjectDel";
    case MsgType::ObjectStat:
      return "ObjectStat";
    case MsgType::ObjectReply:
      return "ObjectReply";
    case MsgType::ObjectPutRange:
      return "ObjectPutRange";
    case MsgType::ObjectPublishTip:
      return "ObjectPublishTip";
    case MsgType::ObjectAbortVersion:
      return "ObjectAbortVersion";
    case MsgType::ObjectListVersions:
      return "ObjectListVersions";
    case MsgType::ObjectPurgeVersions:
      return "ObjectPurgeVersions";
  }
  return "Unknown";
}

std::vector<std::uint8_t> encode_frame(const Frame& frame) {
  const std::string json = frame.body.dump();
  std::vector<std::uint8_t> payload;
  std::uint16_t flags = frame.flags;

  if (!frame.raw.empty() || (flags & kFlagRawBody)) {
    flags |= kFlagRawBody;
    if (json.size() > 0xffffffffu) throw std::runtime_error("json too large");
    const std::uint32_t jlen = static_cast<std::uint32_t>(json.size());
    payload.resize(4 + json.size() + frame.raw.size());
    const std::uint32_t jlen_be = htonl(jlen);
    std::memcpy(payload.data(), &jlen_be, 4);
    if (!json.empty()) {
      std::memcpy(payload.data() + 4, json.data(), json.size());
    }
    if (!frame.raw.empty()) {
      std::memcpy(payload.data() + 4 + json.size(), frame.raw.data(), frame.raw.size());
    }
  } else {
    payload.assign(json.begin(), json.end());
  }

  if (payload.size() > kMaxBodySize) {
    throw std::runtime_error("frame body too large");
  }
  std::vector<std::uint8_t> out(kHeaderSize + payload.size());
  std::memcpy(out.data(), kMagic, 4);
  out[4] = kProtoVersion;
  out[5] = static_cast<std::uint8_t>(frame.type);
  const std::uint16_t flags_be = htons(flags);
  std::memcpy(out.data() + 6, &flags_be, 2);
  const std::uint32_t len_be = htonl(static_cast<std::uint32_t>(payload.size()));
  std::memcpy(out.data() + 8, &len_be, 4);
  if (!payload.empty()) {
    std::memcpy(out.data() + kHeaderSize, payload.data(), payload.size());
  }
  return out;
}

bool decode_frame(const std::uint8_t* data, std::size_t len, Frame& out,
                  std::size_t& consumed, std::string& err) {
  consumed = 0;
  out.raw.clear();
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
  const std::uint8_t* payload = data + kHeaderSize;
  if (flags & kFlagRawBody) {
    if (body_len < 4) {
      err = "raw body missing json_len";
      return false;
    }
    std::uint32_t jlen_be = 0;
    std::memcpy(&jlen_be, payload, 4);
    const std::uint32_t jlen = ntohl(jlen_be);
    if (4u + jlen > body_len) {
      err = "raw body json_len overflow";
      return false;
    }
    if (jlen > 0) {
      try {
        body = nlohmann::json::parse(reinterpret_cast<const char*>(payload + 4),
                                     reinterpret_cast<const char*>(payload + 4 + jlen));
      } catch (const std::exception& e) {
        err = std::string("json parse: ") + e.what();
        return false;
      }
    }
    const std::size_t raw_len = body_len - 4 - jlen;
    if (raw_len > 0) {
      out.raw.assign(payload + 4 + jlen, payload + 4 + jlen + raw_len);
    }
  } else if (body_len > 0) {
    try {
      body = nlohmann::json::parse(reinterpret_cast<const char*>(payload),
                                   reinterpret_cast<const char*>(payload + body_len));
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
