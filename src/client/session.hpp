#pragma once

#include "client/error.hpp"
#include "client/put_layout.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aios {

struct SessionConfig {
  std::string endpoint{"127.0.0.1:7480"};
  // Shared cluster key (legacy: full access, HMAC keyed by the key itself).
  // Leave empty when authenticating as a principal instead.
  std::string cluster_key{};
  // Ticket auth (util/ticket.hpp): principal name + its 64-hex key. The session
  // obtains a ticket on first use, renews it at half-life, and signs requests
  // with the derived session key; the principal key never leaves the process.
  std::string principal{};
  std::string principal_key{};
  // Optional workload label sent as x-aios-app-label on every request.
  std::string app_label{};
  // Per-socket read/write deadline. Applied as SO_RCVTIMEO / SO_SNDTIMEO on a
  // blocking native fd; I/O uses recv/send so Asio cannot swallow the timeout.
  // Also bounds name resolution + TCP connect for a new pooled connection.
  int socket_timeout_ms{30000};
  // Extra host:port values absolute 307/301/302 Location targets may use.
  // The configured endpoint is always allowed. Relative Locations stay on the
  // current hop host. When an absolute redirect is not yet allowlisted, Session
  // once refreshes peers from GET /admin/cluster on the bootstrap endpoint.
  std::vector<std::string> redirect_peers{};
  // Durability data path: "auto" (cluster GET /map io_path), "server" (PUT to
  // primary; aiosd fans out), or "client" (prepare + parallel install + publish).
  std::string io_path{"auto"};
};

struct LockResult {
  std::string token;
  std::int64_t expires_ms{0};
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
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  const SessionConfig& config() const { return cfg_; }
  void set_app_label(std::string label) { cfg_.app_label = std::move(label); }
  const std::string& app_label() const { return cfg_.app_label; }

  HttpResponse request(const std::string& method, const std::string& target,
                       std::unordered_map<std::string, std::string> headers = {},
                       const std::string& body = {}, int max_redirects = 5);
  // Like request(), but the first hop is `http_addr` (host:port). Empty uses
  // the session endpoint. Used by the client I/O path to fan out installs.
  HttpResponse request_peer(const std::string& http_addr, const std::string& method,
                            const std::string& target,
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
  // Optional PutLayout sets x-aios-layout / storage-class / ec-* on the request.
  std::uint64_t put_bytes(const std::string& oid, const std::string& body,
                          const std::unordered_map<std::string, std::string>& attrs = {},
                          std::optional<std::uint64_t> expected_cas = std::nullopt,
                          const std::optional<std::string>& lock_token = std::nullopt,
                          const PutLayout& layout = {});

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
  LockResult lock_acquire(const std::string& oid, int ttl_ms = 30000);
  // break_requested_out: a peer asked for the lease back (lock_break); the
  // server will not extend past its grace deadline and the holder should flush
  // and release.
  void lock_renew(const std::string& oid, const std::string& token, int ttl_ms = 30000,
                  std::int64_t* expires_ms_out = nullptr, bool* break_requested_out = nullptr);
  void lock_release(const std::string& oid, const std::string& token);
  bool lock_try_acquire(const std::string& oid, std::string& token_out, int ttl_ms = 30000,
                        std::int64_t* expires_ms_out = nullptr);
  // Ask the current holder to give the lease back within grace_ms. Returns the
  // deadline (server clock, ms) by which it will be gone, or 0 if nobody holds it.
  std::int64_t lock_break(const std::string& oid, int grace_ms = 5000);
  // Acquire, and when the lease is held by someone else, request a break and
  // wait (up to max_wait_ms) for it to be released. Returns false on timeout.
  bool lock_acquire_wait(const std::string& oid, std::string& token_out, int ttl_ms = 30000,
                         int max_wait_ms = 15000, std::int64_t* expires_ms_out = nullptr);

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

  // Principal mode only. Fetches a ticket now (or renews when `force`), throwing
  // client_error("unauthorized", ...) if the cluster refuses the principal.
  // Called implicitly by request(); exposed so callers can fail fast at startup.
  void ensure_ticket(bool force = false);
  bool uses_ticket() const { return !cfg_.principal.empty(); }
  // Expiry of the current ticket in unix ms (0 = none yet).
  std::int64_t ticket_expires_ms() const;
  // Current ticket and its session key, for tools that sign requests themselves.
  struct TicketMaterial {
    std::string ticket;
    std::string session_key;
    std::int64_t expires_ms{0};
  };
  TicketMaterial ticket_material() const;

 private:
  void parse_endpoint();
  void add_auth(std::unordered_map<std::string, std::string>& headers, const std::string& method,
                const std::string& target, const std::string& body) const;
  HttpResponse post_ticket_request(const std::string& body);
  static std::string next_nonce();
  static void validate_header_value(const std::string& value, const char* what);
  static ObjectSnapshot parse_object_meta(const HttpResponse& resp, bool with_body);

  static std::string normalize_host_port(std::string host, std::string port);
  void allow_redirect_peer(const std::string& http_addr);
  bool redirect_allowed(const std::string& host, const std::string& port) const;
  void refresh_redirect_allowlist();
  // One-shot GET to the bootstrap endpoint; does not follow redirects.
  HttpResponse bootstrap_get(const std::string& path);

  bool use_client_io();
  std::uint64_t put_bytes_client(const std::string& oid, const std::string& body,
                                 std::unordered_map<std::string, std::string> headers,
                                 std::uint64_t new_cas);

  SessionConfig cfg_;
  std::string host_;
  std::string port_;
  // One Session is shared by every FUSE/S3/XRootD worker thread; the allowlist
  // is read on each redirect and grown by the one-shot cluster refresh.
  mutable std::mutex allow_mu_;
  std::unordered_set<std::string> redirect_allow_;
  std::atomic<bool> redirect_refreshed_{false};
  std::atomic<bool> refreshing_allowlist_{false};
  // Cluster io_path from GET /map ("server" | "client"); empty until fetched.
  mutable std::mutex io_mu_;
  std::string cluster_io_path_;
  // Current ticket (principal mode). Renewal happens under ticket_mu_ so a burst
  // of expired-ticket 401s from many threads costs one grant, not one each.
  mutable std::mutex ticket_mu_;
  std::string ticket_;
  std::string session_key_;
  std::int64_t ticket_issued_ms_{0};
  std::int64_t ticket_expires_ms_{0};
  struct ConnPool;
  std::unique_ptr<ConnPool> pool_;
};

}  // namespace aios
