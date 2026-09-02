// Ticket (cephx / krb5 style) authentication: sealing, grant protocol, HTTP
// verifier, keyring at rest, and end-to-end through HttpServer + Session.
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "client/session.hpp"
#include "http/http_auth.hpp"
#include "http/http_server.hpp"
#include "http/principal_store.hpp"
#include "util/auth.hpp"
#include "util/log.hpp"
#include "util/ticket.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

namespace fs = std::filesystem;
using tcp = boost::asio::ip::tcp;
using aios::test::DualStoreFixture;

const std::string kClusterKey = "550e8400-e29b-41d4-a716-446655440000";

int pid_port(int base) { return base + static_cast<int>(::getpid() % 200); }

struct HttpFixture {
  DualStoreFixture fx;
  std::string host{"127.0.0.1"};
  std::string port;
  boost::asio::io_context ioc;
  std::unique_ptr<aios::HttpServer> http;
  std::thread th;

  template <typename Fn>
  HttpFixture(const char* prefix, int base_port, Fn&& configure)
      : fx(prefix, 2, 2, "nvme"), port(std::to_string(pid_port(base_port))) {
    fx.cfg.http_listen = host + ":" + port;
    configure(fx.cfg);
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership);
    http->start();
    th = std::thread([this] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  HttpFixture(const char* prefix, int base_port)
      : HttpFixture(prefix, base_port, [](aios::Config&) {}) {}
  ~HttpFixture() {
    http.reset();
    ioc.stop();
    if (th.joinable()) th.join();
  }

  std::string endpoint() const { return host + ":" + port; }

  aios::SessionConfig shared_cfg() const {
    aios::SessionConfig c;
    c.endpoint = endpoint();
    c.cluster_key = fx.cfg.cluster_key;
    return c;
  }
  aios::SessionConfig principal_cfg(const std::string& name, const std::string& key) const {
    aios::SessionConfig c;
    c.endpoint = endpoint();
    c.principal = name;
    c.principal_key = key;
    return c;
  }

  // Creates a principal through the admin API with the shared key; returns its key.
  nlohmann::json create_principal(const std::string& name, const std::string& role,
                                  std::vector<std::string> caps = {}) {
    aios::Session admin(shared_cfg());
    nlohmann::json body{{"name", name}, {"role", role}, {"caps", caps}};
    auto r = admin.request("POST", "/admin/api/principals", {}, body.dump());
    EXPECT_EQ(r.status, 201) << r.body;
    return nlohmann::json::parse(r.body);
  }
};

// Raw one-shot HTTP for requests Session refuses to build (bad tickets, replays).
struct RawHttp {
  int status{-1};
  std::string body;
};

RawHttp raw_request(const std::string& host, const std::string& port, const std::string& method,
                    const std::string& target,
                    std::unordered_map<std::string, std::string> headers, const std::string& body) {
  boost::asio::io_context ioc;
  tcp::socket sock(ioc);
  tcp::resolver resolver(ioc);
  boost::system::error_code ec;
  auto endpoints = resolver.resolve(host, port, ec);
  boost::asio::connect(sock, endpoints, ec);
  RawHttp out;
  if (ec) return out;
  struct timeval tv{5, 0};
  ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  headers["content-length"] = std::to_string(body.size());
  headers["connection"] = "close";
  std::ostringstream req;
  req << method << ' ' << target << " HTTP/1.1\r\nHost: " << host << ':' << port << "\r\n";
  for (const auto& [k, v] : headers) req << k << ": " << v << "\r\n";
  req << "\r\n" << body;
  const auto s = req.str();
  boost::asio::write(sock, boost::asio::buffer(s), ec);
  boost::asio::streambuf buf;
  boost::asio::read(sock, buf, ec);
  std::istream is(&buf);
  std::string line;
  std::getline(is, line);
  if (line.size() > 12) out.status = std::stoi(line.substr(9, 3));
  while (std::getline(is, line) && line != "\r") {
  }
  std::stringstream rest;
  rest << is.rdbuf();
  out.body = rest.str();
  return out;
}

std::string code_of(const std::string& body) {
  try {
    return nlohmann::json::parse(body).value("code", "");
  } catch (...) {
    return {};
  }
}

}  // namespace

// --- util/ticket.hpp ----------------------------------------------------------

TEST(TicketSeal, RoundTripAndTamperResistance) {
  aios::TicketSealer sealer(kClusterKey);
  aios::Ticket t;
  t.principal = "client.alice";
  t.role = aios::PrincipalRole::Client;
  t.caps = {"alice/", "shared/"};
  t.session_key = aios::random_hex(32);
  t.issued_ms = 1000;
  t.expires_ms = 5000;
  std::string err;
  const auto blob = sealer.seal(t, err);
  ASSERT_FALSE(blob.empty()) << err;
  ASSERT_EQ(blob.rfind(aios::kTicketPrefix, 0), 0u);

  auto back = sealer.open(blob, 2000, err);
  ASSERT_TRUE(back) << err;
  EXPECT_EQ(back->principal, "client.alice");
  EXPECT_EQ(back->role, aios::PrincipalRole::Client);
  EXPECT_EQ(back->caps, t.caps);
  EXPECT_EQ(back->session_key, t.session_key);
  EXPECT_EQ(back->expires_ms, 5000);

  // Expired.
  EXPECT_FALSE(sealer.open(blob, 5000, err));
  EXPECT_EQ(err, "ticket expired");
  EXPECT_TRUE(sealer.open(blob, 5000, err, /*allow_expired=*/true));

  // Any bit flip in the ciphertext or tag fails authentication.
  std::string flipped = blob;
  flipped[flipped.size() / 2] = flipped[flipped.size() / 2] == 'A' ? 'B' : 'A';
  EXPECT_FALSE(sealer.open(flipped, 2000, err));

  // Sealed under another cluster key: not ours.
  aios::TicketSealer other("another-cluster");
  EXPECT_FALSE(other.open(blob, 2000, err));
  EXPECT_NE(err.find("not issued by this cluster"), std::string::npos);

  // Garbage.
  EXPECT_FALSE(sealer.open("t1.!!!", 2000, err));
  EXPECT_FALSE(sealer.open("stl", 2000, err));
  EXPECT_FALSE(sealer.open(std::string(aios::kTicketPrefix) + "QUJD", 2000, err));
}

TEST(TicketSeal, CapsMatchPrefixes) {
  aios::Ticket unrestricted;
  EXPECT_TRUE(unrestricted.allows_oid("anything"));
  EXPECT_TRUE(unrestricted.allows_prefix(""));

  aios::Ticket t;
  t.caps = {"alice/", "shared/x"};
  EXPECT_TRUE(t.allows_oid("alice/doc"));
  EXPECT_TRUE(t.allows_oid("shared/xyz"));
  EXPECT_FALSE(t.allows_oid("alice"));
  EXPECT_FALSE(t.allows_oid("bob/doc"));
  EXPECT_FALSE(t.allows_oid("shared/"));
  EXPECT_TRUE(t.allows_prefix("alice/"));
  EXPECT_TRUE(t.allows_prefix("alice/sub/"));
  EXPECT_FALSE(t.allows_prefix("ali"));
  EXPECT_FALSE(t.allows_prefix(""));
  EXPECT_FALSE(t.allows_prefix("shared/"));
}

TEST(TicketGrant, MutualAuthenticationRoundTrip) {
  aios::TicketSealer sealer(kClusterKey);
  aios::Principal p;
  p.name = "client.alice";
  p.key = aios::generate_principal_key();
  p.role = aios::PrincipalRole::Client;
  p.caps = {"alice/"};

  const std::int64_t now = 1'000'000;
  const auto req = aios::make_ticket_request(p.name, p.key, now);
  EXPECT_EQ(req.proof.size(), 64u);
  EXPECT_EQ(req.nonce.size(), 32u);

  std::string err;
  auto reply = aios::grant_ticket(req, p, sealer, now + 10, 60'000, 3'600'000, err);
  ASSERT_TRUE(reply) << err;
  EXPECT_EQ(reply->expires_ms, now + 10 + 3'600'000);
  EXPECT_EQ(reply->role, "client");

  // Client derives the same session key and validates the server proof.
  auto session_key = aios::verify_ticket_reply(*reply, p.key, req.nonce, err);
  ASSERT_TRUE(session_key) << err;
  auto opened = sealer.open(reply->ticket, now + 20, err);
  ASSERT_TRUE(opened) << err;
  EXPECT_EQ(opened->session_key, *session_key);
  EXPECT_EQ(opened->principal, p.name);
  EXPECT_EQ(opened->caps, p.caps);

  // A client holding the wrong key cannot validate the reply (impostor server
  // or wrong credentials): no silent success with a useless session key.
  EXPECT_FALSE(aios::verify_ticket_reply(*reply, aios::generate_principal_key(), req.nonce, err));

  // Wrong proof, wrong principal, stale timestamp.
  auto bad = req;
  bad.proof[0] = bad.proof[0] == 'a' ? 'b' : 'a';
  EXPECT_FALSE(aios::grant_ticket(bad, p, sealer, now, 60'000, 1000, err));
  EXPECT_EQ(err, "bad proof");
  aios::Principal q = p;
  q.name = "client.bob";
  EXPECT_FALSE(aios::grant_ticket(req, q, sealer, now, 60'000, 1000, err));
  EXPECT_FALSE(aios::grant_ticket(req, p, sealer, now + 120'000, 60'000, 1000, err));
  EXPECT_NE(err.find("skew"), std::string::npos);

  // JSON round trip of both messages.
  auto req2 = aios::ticket_request_from_json(aios::ticket_request_to_json(req));
  ASSERT_TRUE(req2);
  EXPECT_EQ(req2->proof, req.proof);
  auto rep2 = aios::ticket_reply_from_json(aios::ticket_reply_to_json(*reply));
  ASSERT_TRUE(rep2);
  EXPECT_EQ(rep2->ticket, reply->ticket);
  EXPECT_FALSE(aios::ticket_request_from_json(nlohmann::json{{"principal", "x"}}));
  EXPECT_FALSE(aios::ticket_request_from_json(nlohmann::json::array()));
}

// kernel/aios_http/auth.c builds these strings with snprintf; pin the exact
// layouts so a change on either side shows up here rather than as a mount that
// fails with -EACCES.
TEST(TicketGrant, KernelCanonicalStringsPinned) {
  const std::string principal = "fs.node7";
  const std::string key = aios::generate_principal_key();
  const std::int64_t ts = 1725300000123;
  const std::string nonce = "0123456789abcdef0123456789abcdef";
  char buf[512];
  std::snprintf(buf, sizeof(buf), "aios-ticket-req-v1\n%s\n%lld\n%s", principal.c_str(),
                static_cast<long long>(ts), nonce.c_str());
  EXPECT_EQ(aios::ticket_request_canonical(principal, ts, nonce), buf);

  const std::string server_nonce = "fedcba9876543210fedcba9876543210";
  std::snprintf(buf, sizeof(buf), "aios-session-v1\n%s\n%s", nonce.c_str(), server_nonce.c_str());
  EXPECT_EQ(aios::derive_session_key(key, nonce, server_nonce), aios::hmac_sha256_hex(key, buf));

  const std::string ticket = "t1.QUJD";
  const std::int64_t expires = ts + 3600000;
  std::snprintf(buf, sizeof(buf), "aios-ticket-reply-v1\n%s\n%s\n%s\n%lld", nonce.c_str(),
                ticket.c_str(), server_nonce.c_str(), static_cast<long long>(expires));
  EXPECT_EQ(aios::ticket_reply_canonical(nonce, ticket, server_nonce, expires), buf);

  // The kernel keys the proof HMAC with the 64-char hex session key as ASCII,
  // and signs requests the same way: HMAC key = the hex string, not its bytes.
  const auto sk = aios::derive_session_key(key, nonce, server_nonce);
  EXPECT_EQ(sk.size(), 64u);
  EXPECT_EQ(aios::hmac_sha256_hex(sk, "x"), aios::http_sign(sk, "x"));
}

TEST(TicketHttpAuth, VerifierAcceptsTicketCredentialAndRefusesForgeries) {
  aios::TicketSealer sealer(kClusterKey);
  aios::Ticket t;
  t.principal = "client.alice";
  t.role = aios::PrincipalRole::Client;
  t.session_key = aios::random_hex(32);
  t.issued_ms = aios::now_ms();
  t.expires_ms = t.issued_ms + 60'000;
  std::string err;
  const auto ticket = sealer.seal(t, err);
  ASSERT_FALSE(ticket.empty());

  auto sign = [&](const std::string& credential, const std::string& key) {
    std::unordered_map<std::string, std::string> h;
    const std::string date = std::to_string(aios::now_ms());
    h["x-aios-date"] = date;
    h["x-aios-content-sha256"] = "UNSIGNED-PAYLOAD";
    h[aios::kHttpNonceHeader] = aios::random_hex(8);
    const std::string sh = "x-aios-content-sha256;x-aios-date";
    const auto canon = aios::http_canonical("GET", "/o/x", date, sh, h, "UNSIGNED-PAYLOAD");
    h["authorization"] = "AIOS-HMAC-SHA256 Credential=" + credential + ", SignedHeaders=" + sh +
                         ", Signature=" + aios::http_sign(key, canon);
    return h;
  };
  aios::HttpAuthPolicy policy;
  policy.cluster_key = kClusterKey;
  policy.sealer = &sealer;
  policy.skew_ms = 60'000;

  auto ok = aios::http_auth_verify("GET", "/o/x", sign(ticket, t.session_key), "UNSIGNED-PAYLOAD",
                                   policy, nullptr);
  EXPECT_TRUE(ok.ok) << ok.error;
  EXPECT_EQ(ok.principal, "client.alice");
  EXPECT_EQ(ok.role, aios::PrincipalRole::Client);
  EXPECT_FALSE(ok.is_admin());
  ASSERT_TRUE(ok.ticket);

  // Legacy shared key still works and is treated as the node role.
  auto legacy = aios::http_auth_verify("GET", "/o/x", sign("stl", kClusterKey), "UNSIGNED-PAYLOAD",
                                       policy, nullptr);
  EXPECT_TRUE(legacy.ok) << legacy.error;
  EXPECT_TRUE(legacy.principal.empty());
  EXPECT_TRUE(legacy.is_admin());

  // A stolen ticket without the session key is useless.
  auto stolen = aios::http_auth_verify("GET", "/o/x", sign(ticket, kClusterKey), "UNSIGNED-PAYLOAD",
                                       policy, nullptr);
  EXPECT_FALSE(stolen.ok);
  EXPECT_EQ(stolen.code, "bad_signature");
  EXPECT_TRUE(stolen.principal.empty());

  // A ticket minted by another cluster.
  aios::TicketSealer other("other");
  const auto foreign = other.seal(t, err);
  auto forged = aios::http_auth_verify("GET", "/o/x", sign(foreign, t.session_key),
                                       "UNSIGNED-PAYLOAD", policy, nullptr);
  EXPECT_FALSE(forged.ok);
  EXPECT_EQ(forged.code, "bad_ticket");

  // Expired ticket gets its own code so clients know to renew.
  aios::Ticket old = t;
  old.expires_ms = aios::now_ms() - 1;
  const auto expired = sealer.seal(old, err);
  auto ex = aios::http_auth_verify("GET", "/o/x", sign(expired, t.session_key), "UNSIGNED-PAYLOAD",
                                   policy, nullptr);
  EXPECT_FALSE(ex.ok);
  EXPECT_EQ(ex.code, "ticket_expired");

  // Policy: no sealer => tickets refused; shared key refused when disallowed.
  aios::HttpAuthPolicy no_tickets = policy;
  no_tickets.sealer = nullptr;
  EXPECT_EQ(aios::http_auth_verify("GET", "/o/x", sign(ticket, t.session_key), "UNSIGNED-PAYLOAD",
                                   no_tickets, nullptr)
                .code,
            "bad_ticket");
  aios::HttpAuthPolicy no_shared = policy;
  no_shared.allow_shared_key = false;
  auto refused = aios::http_auth_verify("GET", "/o/x", sign("stl", kClusterKey), "UNSIGNED-PAYLOAD",
                                        no_shared, nullptr);
  EXPECT_FALSE(refused.ok);
  EXPECT_EQ(refused.code, "shared_key_refused");
  EXPECT_TRUE(aios::http_auth_verify("GET", "/o/x", sign(ticket, t.session_key),
                                     "UNSIGNED-PAYLOAD", no_shared, nullptr)
                  .ok);
}

TEST(PrincipalKeyring, EncryptedAtRest) {
  std::vector<aios::Principal> ps(2);
  ps[0].name = "client.alice";
  ps[0].key = aios::generate_principal_key();
  ps[0].caps = {"alice/"};
  ps[1].name = "admin.ops";
  ps[1].key = aios::generate_principal_key();
  ps[1].role = aios::PrincipalRole::Admin;
  std::string err;
  const auto body = aios::PrincipalStore::encrypt_keyring(ps, kClusterKey, err);
  ASSERT_FALSE(body.empty()) << err;
  // Neither key nor name is visible in the stored object.
  EXPECT_EQ(body.find(ps[0].key), std::string::npos);
  EXPECT_EQ(body.find("alice"), std::string::npos);

  std::vector<aios::Principal> back;
  ASSERT_TRUE(aios::PrincipalStore::decrypt_keyring(body, kClusterKey, back, err)) << err;
  ASSERT_EQ(back.size(), 2u);
  EXPECT_EQ(back[0].key, ps[0].key);
  EXPECT_EQ(back[0].caps, ps[0].caps);
  EXPECT_EQ(back[1].role, aios::PrincipalRole::Admin);

  EXPECT_FALSE(aios::PrincipalStore::decrypt_keyring(body, "wrong", back, err));
  EXPECT_FALSE(aios::PrincipalStore::decrypt_keyring("{}", kClusterKey, back, err));
  EXPECT_FALSE(aios::PrincipalStore::decrypt_keyring("not json", kClusterKey, back, err));
}

// --- end to end ---------------------------------------------------------------

TEST(TicketE2E, PrincipalLifecycleRolesAndCaps) {
  HttpFixture f("aios-ticket-e2e", 27700);

  // Bootstrap: with the shared key, create two principals on a node that does
  // not run the admin UI (principal management works everywhere).
  ASSERT_FALSE(f.fx.cfg.admin);
  auto alice = f.create_principal("client.alice", "client", {"alice/"});
  auto ops = f.create_principal("admin.ops", "admin");
  ASSERT_EQ(alice.value("key", "").size(), 64u);
  {
    aios::Session admin(f.shared_cfg());
    auto r = admin.request("GET", "/admin/api/principals");
    ASSERT_EQ(r.status, 200) << r.body;
    auto j = nlohmann::json::parse(r.body);
    ASSERT_EQ(j["principals"].size(), 2u);
    for (const auto& p : j["principals"]) EXPECT_EQ(p.value("key", ""), "***");
    // Duplicate name and bad role are rejected.
    EXPECT_EQ(admin.request("POST", "/admin/api/principals", {},
                            nlohmann::json{{"name", "client.alice"}}.dump())
                  .status,
              409);
    EXPECT_EQ(admin.request("POST", "/admin/api/principals", {},
                            nlohmann::json{{"name", "x"}, {"role", "root"}}.dump())
                  .status,
              400);
    // The keyring object itself is unreachable over HTTP, even with the shared key.
    auto g = admin.request("GET", "/o/" + aios::Session::url_encode_oid(aios::kPrincipalKeyringOid));
    EXPECT_EQ(g.status, 403);
    EXPECT_EQ(code_of(g.body), "reserved_oid");
    EXPECT_EQ(admin.request("PUT", "/o/auth%2Fevil", {}, "x").status, 403);
    EXPECT_EQ(admin.request("DELETE", "/o/auth%2Fprincipals").status, 403);
  }

  // Alice: ticket obtained implicitly, full object API inside her caps.
  {
    aios::Session s(f.principal_cfg("client.alice", alice["key"]));
    EXPECT_EQ(s.ticket_expires_ms(), 0);
    s.put_bytes("alice/doc", "hello");
    EXPECT_GT(s.ticket_expires_ms(), aios::now_ms());
    EXPECT_EQ(s.get_object("alice/doc").body, "hello");
    auto listed = s.list_prefix("alice/");
    ASSERT_EQ(listed.objects.size(), 1u);
    EXPECT_EQ(listed.objects[0].oid, "alice/doc");
    const auto m = s.ticket_material();
    EXPECT_EQ(m.ticket.rfind(aios::kTicketPrefix, 0), 0u);
    EXPECT_EQ(m.session_key.size(), 64u);

    // Outside her caps: object ops, txn prepare and listing are all 403.
    auto r = s.request("PUT", "/o/bob%2Fdoc", {}, "x");
    EXPECT_EQ(r.status, 403) << r.body;
    EXPECT_EQ(code_of(r.body), "forbidden_cap");
    EXPECT_EQ(s.request("GET", "/o/bob%2Fdoc").status, 403);
    EXPECT_EQ(s.request("GET", "/o?prefix=bob/").status, 403);
    EXPECT_EQ(s.request("GET", "/o?prefix=ali").status, 403);
    EXPECT_EQ(s.request("GET", "/o").status, 403);
    const auto txn = s.txn_begin();
    EXPECT_THROW(s.txn_prepare_put(txn, "bob/doc", "x"), aios::client_error);
    s.txn_abort(txn);
    // A big streamed PUT is refused before the body is accepted.
    auto big = s.request("PUT", "/o/bob%2Fbig", {}, std::string(300 * 1024, 'b'));
    EXPECT_EQ(big.status, 403);

    // Role: client cannot touch the admin surface (403, not 404), but may read
    // the peer list clients use for redirects.
    r = s.request("GET", "/admin/api/principals");
    EXPECT_EQ(r.status, 403) << r.body;
    EXPECT_EQ(code_of(r.body), "forbidden_role");
    EXPECT_EQ(s.request("GET", "/metrics").status, 403);
    EXPECT_EQ(s.request("POST", "/admin/api/principals", {}, "{}").status, 403);
    EXPECT_NE(s.request("GET", "/admin/cluster").status, 403);
  }

  // Ops: admin role, full object access, may manage principals.
  {
    aios::Session s(f.principal_cfg("admin.ops", ops["key"]));
    EXPECT_EQ(s.get_object("alice/doc").body, "hello");
    s.put_bytes("bob/doc", "bob");
    auto r = s.request("GET", "/admin/api/principals");
    EXPECT_EQ(r.status, 200) << r.body;
    // Even admins cannot read the keyring object.
    EXPECT_EQ(s.request("GET", "/o/auth%2Fprincipals").status, 403);
  }

  // Unknown principal and wrong key are indistinguishable failures.
  {
    aios::Session unknown(f.principal_cfg("client.nobody", aios::generate_principal_key()));
    try {
      unknown.get_object("alice/doc");
      FAIL() << "unknown principal got a ticket";
    } catch (const aios::client_error& e) {
      EXPECT_EQ(std::string(e.code()), "bad_proof") << e.what();
    }
    aios::Session wrong(f.principal_cfg("client.alice", aios::generate_principal_key()));
    try {
      wrong.get_object("alice/doc");
      FAIL() << "wrong key got a ticket";
    } catch (const aios::client_error& e) {
      EXPECT_EQ(std::string(e.code()), "bad_proof") << e.what();
    }
  }

  // Rotate: the old key stops granting tickets, tickets already issued keep
  // working until they expire; delete: no more tickets at all.
  {
    aios::Session old_key(f.principal_cfg("client.alice", alice["key"]));
    EXPECT_EQ(old_key.get_object("alice/doc").body, "hello");

    aios::Session admin(f.shared_cfg());
    auto r = admin.request("POST", "/admin/api/principals/client.alice/rotate", {}, "{}");
    ASSERT_EQ(r.status, 200) << r.body;
    const std::string new_key = nlohmann::json::parse(r.body).value("key", "");
    ASSERT_EQ(new_key.size(), 64u);
    EXPECT_NE(new_key, alice.value("key", ""));

    EXPECT_EQ(old_key.get_object("alice/doc").body, "hello") << "issued ticket must survive";
    aios::Session stale(f.principal_cfg("client.alice", alice["key"]));
    EXPECT_THROW(stale.get_object("alice/doc"), aios::client_error);
    aios::Session fresh(f.principal_cfg("client.alice", new_key));
    EXPECT_EQ(fresh.get_object("alice/doc").body, "hello");

    r = admin.request("DELETE", "/admin/api/principals/client.alice");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(admin.request("DELETE", "/admin/api/principals/client.alice").status, 404);
    aios::Session gone(f.principal_cfg("client.alice", new_key));
    EXPECT_THROW(gone.get_object("alice/doc"), aios::client_error);
  }
}

TEST(TicketE2E, GrantEndpointReplayAndMalformedRequests) {
  HttpFixture f("aios-ticket-grant", 27900);
  auto p = f.create_principal("client.carol", "client");
  const std::string key = p["key"];

  const auto req = aios::make_ticket_request("client.carol", key, aios::now_ms());
  const auto body = aios::ticket_request_to_json(req).dump();
  auto first = raw_request(f.host, f.port, "POST", "/auth/ticket", {}, body);
  ASSERT_EQ(first.status, 200) << first.body;
  auto reply = aios::ticket_reply_from_json(nlohmann::json::parse(first.body));
  ASSERT_TRUE(reply);
  std::string err;
  EXPECT_TRUE(aios::verify_ticket_reply(*reply, key, req.nonce, err)) << err;

  // Same proof again: replayed, no second ticket.
  auto again = raw_request(f.host, f.port, "POST", "/auth/ticket", {}, body);
  EXPECT_EQ(again.status, 401);
  EXPECT_EQ(code_of(again.body), "replayed");

  // Malformed bodies are 400, not crashes.
  EXPECT_EQ(raw_request(f.host, f.port, "POST", "/auth/ticket", {}, "not json").status, 400);
  EXPECT_EQ(raw_request(f.host, f.port, "POST", "/auth/ticket", {}, "[]").status, 400);
  EXPECT_EQ(raw_request(f.host, f.port, "POST", "/auth/ticket", {},
                        R"({"principal":"../x","ts":1,"nonce":"abcdefghijklmnop","proof":"00"})")
                .status,
            400);

  // Stale timestamp is reported as skew.
  auto stale = aios::make_ticket_request("client.carol", key, aios::now_ms() - 3'600'000);
  auto st = raw_request(f.host, f.port, "POST", "/auth/ticket", {},
                        aios::ticket_request_to_json(stale).dump());
  EXPECT_EQ(st.status, 401);
  EXPECT_EQ(code_of(st.body), "date_skew");

  // GET on the grant endpoint is just an unauthenticated request.
  EXPECT_EQ(raw_request(f.host, f.port, "GET", "/auth/ticket", {}, "").status, 401);
}

TEST(TicketE2E, ShortLivedTicketsAreRenewedTransparently) {
  HttpFixture f("aios-ticket-renew", 28100,
                [](aios::Config& c) { c.http_ticket_lifetime_ms = 600; });
  auto p = f.create_principal("client.dave", "client");
  aios::Session s(f.principal_cfg("client.dave", p["key"]));
  s.put_bytes("dave/a", "1");
  const auto first = s.ticket_material();
  // Past the half-life: the next request renews before signing.
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  EXPECT_EQ(s.get_object("dave/a").body, "1");
  const auto second = s.ticket_material();
  EXPECT_NE(first.ticket, second.ticket);
  EXPECT_GT(second.expires_ms, first.expires_ms);
  // Past expiry entirely (no request in between): still transparent.
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  EXPECT_EQ(s.get_object("dave/a").body, "1");
  EXPECT_NE(s.ticket_material().ticket, second.ticket);
}

TEST(TicketE2E, LoopbackOnlySharedKeyPolicyStillServesLocalClients) {
  // With http_shared_key_clients=loopback the test peer is loopback, so the
  // shared key keeps working here; the refusal path is covered by the verifier
  // unit test. This pins that the knob does not break the daemon's own gateway.
  HttpFixture f("aios-ticket-loopback", 28300,
                [](aios::Config& c) { c.http_shared_key_clients = "loopback"; });
  aios::Session s(f.shared_cfg());
  s.put_bytes("k/v", "1");
  EXPECT_EQ(s.get_object("k/v").body, "1");
  auto p = f.create_principal("client.erin", "client");
  aios::Session e(f.principal_cfg("client.erin", p["key"]));
  EXPECT_EQ(e.get_object("k/v").body, "1");
}
