// Native TLS on the S3 listener (s3_tls_cert / s3_tls_key / s3_tls_chain).
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "http/http_server.hpp"
#include "http/s3_iam.hpp"
#include "http/s3_server.hpp"
#include "http/tls_stream.hpp"
#include "util/auth.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <boost/asio.hpp>

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

namespace fs = std::filesystem;
using tcp = boost::asio::ip::tcp;
using aios::test::DualStoreFixture;

int pid_port(int base) { return base + static_cast<int>(::getpid() % 200); }

// Self-signed cert + key for 127.0.0.1, written as PEM. Returns false on error.
bool make_self_signed(const fs::path& cert_pem, const fs::path& key_pem, const char* cn) {
  EVP_PKEY* pkey = EVP_RSA_gen(2048);
  if (!pkey) return false;

  X509* x = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
  X509_gmtime_adj(X509_get_notBefore(x), -60);
  X509_gmtime_adj(X509_get_notAfter(x), 3600);
  X509_set_pubkey(x, pkey);
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
  X509_set_issuer_name(x, name);
  if (X509_sign(x, pkey, EVP_sha256()) == 0) return false;

  bool ok = true;
  if (FILE* f = std::fopen(cert_pem.c_str(), "w")) {
    ok = PEM_write_X509(f, x) == 1;
    std::fclose(f);
  } else {
    ok = false;
  }
  if (ok) {
    if (FILE* f = std::fopen(key_pem.c_str(), "w")) {
      ok = PEM_write_PrivateKey(f, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
      std::fclose(f);
    } else {
      ok = false;
    }
  }
  X509_free(x);
  EVP_PKEY_free(pkey);
  return ok;
}

std::string amz_now() {
  const auto t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

std::string hex_lower(const std::string& raw) {
  static const char* hexd = "0123456789abcdef";
  std::string out(raw.size() * 2, '\0');
  for (std::size_t i = 0; i < raw.size(); ++i) {
    auto c = static_cast<unsigned char>(raw[i]);
    out[i * 2] = hexd[c >> 4];
    out[i * 2 + 1] = hexd[c & 0xf];
  }
  return out;
}

void sign_s3(const std::string& method, const std::string& uri, const std::string& hostport,
             const std::string& payload_hash, const std::string& access, const std::string& secret,
             std::unordered_map<std::string, std::string>& headers) {
  using namespace aios;
  const std::string region = "us-east-1";
  const auto amz = amz_now();
  const auto ds = amz.substr(0, 8);
  headers["x-amz-date"] = amz;
  headers["x-amz-content-sha256"] = payload_hash;
  headers["host"] = hostport;
  const std::string canon_headers = "host:" + hostport + "\nx-amz-content-sha256:" + payload_hash +
                                    "\nx-amz-date:" + amz + "\n";
  const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
  std::ostringstream canon;
  canon << method << '\n' << uri << "\n\n" << canon_headers << signed_headers << '\n'
        << payload_hash;
  std::ostringstream sts;
  sts << "AWS4-HMAC-SHA256\n" << amz << '\n' << ds << '/' << region << "/s3/aws4_request\n"
      << sha256_hex(canon.str());
  auto k = hmac_sha256_raw("AWS4" + secret, ds);
  k = hmac_sha256_raw(k, region);
  k = hmac_sha256_raw(k, "s3");
  k = hmac_sha256_raw(k, "aws4_request");
  headers["authorization"] = "AWS4-HMAC-SHA256 Credential=" + access + "/" + ds + "/" + region +
                             "/s3/aws4_request, SignedHeaders=" + signed_headers +
                             ", Signature=" + hex_lower(hmac_sha256_raw(k, sts.str()));
}

struct Resp {
  int status{-1};
  std::string body;
  std::string error;
};

// Minimal blocking client: TLS (OpenSSL on a blocking fd) or plain.
Resp exchange(const std::string& host, const std::string& port, bool use_tls,
              const std::string& method, const std::string& target,
              std::unordered_map<std::string, std::string> headers, const std::string& body) {
  Resp out;
  boost::asio::io_context ioc;
  tcp::socket sock(ioc);
  tcp::resolver resolver(ioc);
  boost::system::error_code ec;
  auto eps = resolver.resolve(host, port, ec);
  boost::asio::connect(sock, eps, ec);
  if (ec) {
    out.error = "connect: " + ec.message();
    return out;
  }
  const int fd = static_cast<int>(sock.native_handle());
  struct timeval tv{5, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  headers["content-length"] = std::to_string(body.size());
  headers["connection"] = "close";
  std::ostringstream req;
  req << method << ' ' << target << " HTTP/1.1\r\n";
  for (const auto& [k, v] : headers) req << k << ": " << v << "\r\n";
  req << "\r\n" << body;
  const auto s = req.str();

  std::string raw;
  if (use_tls) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    // Self-signed in the test: skip verification; what matters is that the
    // server completes a handshake and speaks HTTP inside it.
    SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);
    if (SSL_connect(ssl) != 1) {
      out.error = "tls handshake failed";
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      return out;
    }
    SSL_write(ssl, s.data(), static_cast<int>(s.size()));
    char buf[4096];
    for (;;) {
      const int n = SSL_read(ssl, buf, sizeof(buf));
      if (n <= 0) break;
      raw.append(buf, static_cast<std::size_t>(n));
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
  } else {
    ::send(fd, s.data(), s.size(), 0);
    char buf[4096];
    for (;;) {
      const auto n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) break;
      raw.append(buf, static_cast<std::size_t>(n));
    }
  }
  if (raw.size() > 12 && raw.rfind("HTTP/1.", 0) == 0) out.status = std::stoi(raw.substr(9, 3));
  const auto hdr_end = raw.find("\r\n\r\n");
  if (hdr_end != std::string::npos) out.body = raw.substr(hdr_end + 4);
  return out;
}

struct TlsS3Fixture {
  DualStoreFixture fx;
  fs::path cert, key;
  std::string http_port, s3_port, s3_hostport;
  std::shared_ptr<aios::S3IamStore> iam;
  boost::asio::io_context ioc;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
  std::thread th;
  std::unique_ptr<aios::HttpServer> http;
  std::unique_ptr<aios::S3Server> s3;

  TlsS3Fixture(const char* prefix, int http_base, int s3_base, bool tls)
      : fx(prefix, 2, 2, "nvme"), work(boost::asio::make_work_guard(ioc)) {
    cert = fx.root / "s3.crt";
    key = fx.root / "s3.key";
    if (tls) {
      if (!make_self_signed(cert, key, "127.0.0.1")) throw std::runtime_error("cert gen failed");
      fx.cfg.s3_tls_cert = cert.string();
      fx.cfg.s3_tls_key = key.string();
    }
    http_port = std::to_string(pid_port(http_base));
    s3_port = std::to_string(pid_port(s3_base));
    s3_hostport = "127.0.0.1:" + s3_port;
    fx.cfg.http_listen = "127.0.0.1:" + http_port;
    fx.cfg.s3_listen = s3_hostport;
    fx.cfg.s3_volume = "s3";
    fx.cfg.s3_access_key = "aios";
    fx.cfg.http_idle_timeout_ms = 2000;
    iam = std::make_shared<aios::S3IamStore>(fx.cfg, *fx.svc);
    th = std::thread([&] { ioc.run(); });
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership, iam);
    http->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    s3 = std::make_unique<aios::S3Server>(ioc, fx.cfg, "127.0.0.1:" + http_port, iam);
    s3->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ~TlsS3Fixture() {
    if (s3) s3->stop();
    s3.reset();
    http.reset();
    work.reset();
    ioc.stop();
    if (th.joinable()) th.join();
  }

  Resp s3_req(bool tls, const std::string& method, const std::string& path,
              const std::string& body) {
    std::unordered_map<std::string, std::string> h;
    sign_s3(method, path, s3_hostport, aios::sha256_hex(body), "aios", fx.cfg.cluster_key, h);
    return exchange("127.0.0.1", s3_port, tls, method, path, h, body);
  }
};

}  // namespace

TEST(S3Tls, ContextLoadValidatesFiles) {
  const auto root = aios::test::temp_root("aios-s3tls-ctx");
  const auto cert = root / "c.pem";
  const auto key = root / "k.pem";
  ASSERT_TRUE(make_self_signed(cert, key, "127.0.0.1"));
  std::string err;
  EXPECT_TRUE(aios::TlsServerContext::load(cert.string(), key.string(), "", err)) << err;
  // Cert file may double as chain file (leaf appended again is harmless).
  EXPECT_TRUE(aios::TlsServerContext::load(cert.string(), key.string(), cert.string(), err)) << err;

  EXPECT_FALSE(aios::TlsServerContext::load((root / "missing.pem").string(), key.string(), "", err));
  EXPECT_NE(err.find("load certificate"), std::string::npos) << err;
  EXPECT_FALSE(aios::TlsServerContext::load(cert.string(), (root / "missing.pem").string(), "", err));
  EXPECT_NE(err.find("load private key"), std::string::npos) << err;

  // Key that does not belong to the certificate.
  const auto cert2 = root / "c2.pem";
  const auto key2 = root / "k2.pem";
  ASSERT_TRUE(make_self_signed(cert2, key2, "other"));
  EXPECT_FALSE(aios::TlsServerContext::load(cert.string(), key2.string(), "", err));
  // OpenSSL rejects the key at load time ("key values mismatch"); older builds
  // only notice in check_private_key.
  EXPECT_TRUE(err.find("mismatch") != std::string::npos ||
              err.find("does not match") != std::string::npos)
      << err;

  // Chain file without certificates.
  std::ofstream(root / "empty.pem") << "not a certificate\n";
  EXPECT_FALSE(
      aios::TlsServerContext::load(cert.string(), key.string(), (root / "empty.pem").string(), err));
  EXPECT_NE(err.find("no certificates"), std::string::npos) << err;
  fs::remove_all(root);
}

TEST(S3Tls, StartRefusesHalfConfiguredOrBadTls) {
  DualStoreFixture fx("aios-s3tls-start", 2, 2, "nvme");
  fx.cfg.http_listen = "127.0.0.1:" + std::to_string(pid_port(29300));
  fx.cfg.s3_listen = "127.0.0.1:" + std::to_string(pid_port(29500));
  boost::asio::io_context ioc;
  {
    auto cfg = fx.cfg;
    cfg.s3_tls_cert = (fx.root / "nope.pem").string();
    aios::S3Server s3(ioc, cfg, fx.cfg.http_listen);
    EXPECT_THROW(s3.start(), std::runtime_error);
  }
  {
    auto cfg = fx.cfg;
    cfg.s3_tls_cert = (fx.root / "nope.pem").string();
    cfg.s3_tls_key = (fx.root / "nope.key").string();
    aios::S3Server s3(ioc, cfg, fx.cfg.http_listen);
    try {
      s3.start();
      FAIL() << "started with unreadable certificate";
    } catch (const std::runtime_error& e) {
      EXPECT_NE(std::string(e.what()).find("S3 TLS"), std::string::npos) << e.what();
    }
  }
}

TEST(S3Tls, SigV4OverHttpsRoundTripAndPlaintextRefused) {
  TlsS3Fixture f("aios-s3tls-e2e", 29320, 29520, /*tls=*/true);
  ASSERT_TRUE(f.s3->tls());

  auto mk = f.s3_req(true, "PUT", "/tlsbucket", "");
  EXPECT_EQ(mk.status, 200) << mk.error << mk.body;
  const std::string payload(100 * 1024, 't');
  auto put = f.s3_req(true, "PUT", "/tlsbucket/obj", payload);
  EXPECT_EQ(put.status, 200) << put.error << put.body;
  auto get = f.s3_req(true, "GET", "/tlsbucket/obj", "");
  EXPECT_EQ(get.status, 200) << get.error;
  EXPECT_EQ(get.body, payload);

  // Wrong secret still gets the S3 error, now inside TLS.
  {
    std::unordered_map<std::string, std::string> h;
    sign_s3("GET", "/tlsbucket/obj", f.s3_hostport, aios::sha256_hex(""), "aios", "wrong", h);
    auto r = exchange("127.0.0.1", f.s3_port, true, "GET", "/tlsbucket/obj", h, "");
    EXPECT_EQ(r.status, 403) << r.body;
  }

  // Plain HTTP on the HTTPS port: no handshake, connection closed, nothing served.
  auto plain = f.s3_req(false, "GET", "/tlsbucket/obj", "");
  EXPECT_EQ(plain.status, -1);
  EXPECT_TRUE(plain.body.empty());

  // The object landed on the same volume a plain listener would use.
  TlsS3Fixture g("aios-s3tls-plain", 29340, 29540, /*tls=*/false);
  ASSERT_FALSE(g.s3->tls());
  auto ok = g.s3_req(false, "PUT", "/plainbucket", "");
  EXPECT_EQ(ok.status, 200) << ok.error << ok.body;
  // And a TLS client on a plain port fails its handshake cleanly.
  auto bad = g.s3_req(true, "GET", "/plainbucket", "");
  EXPECT_EQ(bad.status, -1);
  EXPECT_EQ(bad.error, "tls handshake failed");
}
