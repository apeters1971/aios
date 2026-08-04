#pragma once

#include "client/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

struct SessionConfig {
  std::string endpoint{"127.0.0.1:7480"};
  std::string cluster_key;
  // Optional workload label sent as x-aios-app-label on every request.
  std::string app_label;
};

struct HttpResponse {
  int status{-1};
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

struct ObjectSnapshot {
  bool exists{false};
  std::uint64_t seq{0};
  std::uint64_t cas{0};
  std::uint64_t size{0};
  std::string body;
  std::unordered_map<std::string, std::string> attrs;
};

struct AppendResult {
  std::uint64_t offset{0};
  std::uint64_t size{0};
  std::uint64_t seq{0};
  std::uint64_t epoch{0};
};

struct ListObject {
  std::string oid;
  std::uint64_t size{0};
  std::int64_t mtime_ms{0};
};

struct ListResult {
  std::vector<ListObject> objects;
  std::string next_cursor;
};

// Placement-aware HTTP session (HMAC + 307 follow).
class Session {
 public:
  static constexpr std::size_t kMaxBodyBytes = 16u * 1024u * 1024u;

  explicit Session(SessionConfig cfg);

  const SessionConfig& config() const { return cfg_; }
  void set_app_label(std::string label) { cfg_.app_label = std::move(label); }
  const std::string& app_label() const { return cfg_.app_label; }

  HttpResponse request(const std::string& method, const std::string& target,
                       std::unordered_map<std::string, std::string> headers = {},
                       const std::string& body = {}, int max_redirects = 5);

  ObjectSnapshot get_object(const std::string& oid);
  ObjectSnapshot head_object(const std::string& oid);
  // Inclusive end (HTTP Range bytes=start-end). Empty if oid missing.
  ObjectSnapshot get_range(const std::string& oid, std::uint64_t start,
                           std::uint64_t end_inclusive);

  // Put body with STL attrs + CAS. expected_cas=0 means create-or-first-write.
  // On success returns new cas. Throws client_error on conflict/http errors.
  std::uint64_t put_object(const std::string& oid, const std::string& body,
                           const std::string& stl_type, std::uint64_t expected_cas,
                           const std::optional<std::string>& lock_token = std::nullopt,
                           int stl_v = 1);

  // Generic full PUT. If expected_cas has_value, uses attr aios.posix.cas:
  // nullopt = unconditional; 0 = create-if-absent; N = require cas==N, write N+1.
  // Returns new cas (0 if CAS not used).
  std::uint64_t put_bytes(const std::string& oid, const std::string& body,
                          const std::unordered_map<std::string, std::string>& attrs = {},
                          std::optional<std::uint64_t> expected_cas = std::nullopt,
                          const std::optional<std::string>& lock_token = std::nullopt);

  // Partial PUT via Content-Range (replica tips only on server).
  void put_range(const std::string& oid, std::uint64_t offset, const std::string& data,
                 const std::optional<std::string>& lock_token = std::nullopt);

  void delete_object(const std::string& oid,
                     const std::optional<std::string>& lock_token = std::nullopt);

  ListResult list_prefix(const std::string& prefix, std::size_t limit = 256,
                         const std::string& cursor = {});

  // Atomic append (follows 307). Returns allocated offset + new size/seq.
  AppendResult append(const std::string& oid, const std::string& data,
                      const std::optional<std::string>& lock_token = std::nullopt);

  // Lock API on arbitrary oid.
  std::string lock_acquire(const std::string& oid, int ttl_ms = 30000);
  void lock_renew(const std::string& oid, const std::string& token, int ttl_ms = 30000);
  void lock_release(const std::string& oid, const std::string& token);
  bool lock_try_acquire(const std::string& oid, std::string& token_out, int ttl_ms = 30000);

  // Cross-object transactions (HTTP /txn). Prepare uses aios.posix.cas like put_bytes
  // when expected_cas is set. Pass lock_token when the oid is locked by this client.
  std::string txn_begin();
  void txn_prepare_put(const std::string& txn_id, const std::string& oid,
                       const std::string& body,
                       std::optional<std::uint64_t> expected_cas = std::nullopt,
                       const std::optional<std::string>& lock_token = std::nullopt,
                       const std::unordered_map<std::string, std::string>& attrs = {});
  void txn_prepare_delete(const std::string& txn_id, const std::string& oid,
                          const std::optional<std::string>& lock_token = std::nullopt);
  void txn_commit(const std::string& txn_id);
  void txn_abort(const std::string& txn_id);

  static std::string url_encode_oid(const std::string& oid);
  static std::string stl_oid(const std::string& type, const std::string& name);

 private:
  void parse_endpoint();
  void add_auth(std::unordered_map<std::string, std::string>& headers, const std::string& method,
                const std::string& target) const;
  static ObjectSnapshot parse_object_meta(const HttpResponse& resp, bool with_body);

  SessionConfig cfg_;
  std::string host_;
  std::string port_;
};

}  // namespace aios
