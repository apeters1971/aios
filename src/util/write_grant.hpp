#pragma once

// HMAC write grant for the client I/O path (io_path: client).
//
// The object primary mints a grant after reserving a seq. Any acting-set node
// can verify it with the cluster key and install that unpublished version.
// Clients treat the blob as opaque; they must not be able to forge one without
// the cluster key.

#include "cluster/cluster_map.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aios {

struct WriteGrant {
  std::string oid;
  std::uint64_t seq{0};
  std::uint64_t epoch{0};
  std::string layout;  // "replica" | "ec"
  int n{0};
  int ec_k{0};
  int ec_m{0};
  std::string ec_codec;
  std::string storage_class;
  std::uint64_t full_size{0};
  std::uint32_t full_crc{0};
  std::int64_t expires_ms{0};
  std::vector<StorageTarget> acting_set;

  bool is_ec() const { return layout == "ec"; }
};

// Compact JSON including "sig". Empty on failure (`err` set).
std::string seal_write_grant(const WriteGrant& g, const std::string& cluster_key, std::string& err);

// Rejects malformed, forged, and (unless allow_expired) expired grants.
std::optional<WriteGrant> open_write_grant(const std::string& blob, const std::string& cluster_key,
                                           std::int64_t now_ms, std::string& err,
                                           bool allow_expired = false);

}  // namespace aios
