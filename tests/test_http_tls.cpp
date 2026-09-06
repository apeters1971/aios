// TLS on the HTTP API listener (http_tls_cert / http_tls_key / http_tls_ca) and
// the client side of it: aios::Session, libaios_posix, the bench client.
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include "bench/http_bench.hpp"
#include "client/session.hpp"
#include "http/http_server.hpp"
#include "http/tls_stream.hpp"
#include "posix/aios_posix.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <boost/asio.hpp>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
using aios::test::DualStoreFixture;

int pid_port(int base) { return base + static_cast<int>(::getpid() % 200); }

// Self-signed cert for 127.0.0.1. with_ip_san: also carry an IP SAN so a
// verifying client can pin it; without one only --tls-insecure connects.
bool make_self_signed(const fs::path& cert_pem, const fs::path& key_pem, bool with_ip_san) {
  EVP_PKEY* pkey = EVP_RSA_gen(2048);
  if (!pkey) return false;
  X509* x = X509_new();
  X509_set_version(x, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
  X509_gmtime_adj(X509_get_notBefore(x), -60);
  X509_gmtime_adj(X509_get_notAfter(x), 3600);
  X509_set_pubkey(x, pkey);
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0);
  X509_set_issuer_name(x, name);
  if (with_ip_san) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
    X509_EXTENSION* ext =
        X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, "IP:127.0.0.1");
    if (!ext) return false;
    X509_add_ext(x, ext, -1);
    X509_EXTENSION_free(ext);
  }
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

struct TlsHttpFixture {
  DualStoreFixture fx;
  fs::path cert, key;
  std::string port;
  std::string hostport;
  boost::asio::io_context ioc;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
  std::thread th;
  std::unique_ptr<aios::HttpServer> http;

  explicit TlsHttpFixture(const char* prefix, bool with_ip_san = true)
      : fx(prefix), work(boost::asio::make_work_guard(ioc)) {
    cert = fx.root / "http.crt";
    key = fx.root / "http.key";
    if (!make_self_signed(cert, key, with_ip_san)) throw std::runtime_error("cert gen failed");
    fx.cfg.http_tls_cert = cert.string();
    fx.cfg.http_tls_key = key.string();
    port = std::to_string(pid_port(19700));
    hostport = "127.0.0.1:" + port;
    fx.cfg.http_listen = hostport;
    fx.cfg.http_idle_timeout_ms = 2000;
    th = std::thread([&] { ioc.run(); });
    http = std::make_unique<aios::HttpServer>(ioc, fx.cfg, *fx.svc, fx.membership);
    http->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ~TlsHttpFixture() {
    http.reset();
    work.reset();
    ioc.stop();
    if (th.joinable()) th.join();
  }

  aios::SessionConfig session_cfg(bool insecure, const std::string& ca = {}) const {
    aios::SessionConfig c;
    c.endpoint = "https://" + hostport;
    c.cluster_key = fx.cfg.cluster_key;
    c.tls_insecure = insecure;
    c.tls_ca = ca;
    c.socket_timeout_ms = 3000;
    return c;
  }
};

}  // namespace

TEST(HttpTls, PlainClientIsRefusedAndTlsClientsWork) {
  TlsHttpFixture t("aios-http-tls");

  // Plain HTTP against the TLS listener: the handshake fails, nothing is served.
  {
    aios::SessionConfig c;
    c.endpoint = t.hostport;
    c.cluster_key = t.fx.cfg.cluster_key;
    c.socket_timeout_ms = 2000;
    aios::Session s(c);
    EXPECT_THROW(s.put_bytes("tls/plain", "x"), std::exception);
  }

  // "https://" endpoint + insecure: full object API over TLS, including the
  // lock API and a redirect-following append.
  {
    aios::Session s(t.session_cfg(true));
    EXPECT_TRUE(s.config().tls);
    EXPECT_EQ(s.config().endpoint, t.hostport) << "scheme stripped";
    s.put_bytes("tls/obj", std::string(70000, 'q'));
    auto snap = s.get_object("tls/obj");
    ASSERT_TRUE(snap.exists);
    EXPECT_EQ(snap.body.size(), 70000u);
    auto lk = s.lock_acquire("tls/obj", 2000);
    ASSERT_FALSE(lk.token.empty());
    s.put_bytes("tls/obj", "small", {}, std::nullopt, lk.token);
    s.lock_release("tls/obj", lk.token);
    auto ap = s.append("tls/log", "hello");
    EXPECT_EQ(ap.offset, 0u);
    EXPECT_EQ(s.get_object("tls/obj").body, "small");
  }

  // Verifying client pinned to the server certificate (it carries an IP SAN).
  {
    aios::Session s(t.session_cfg(false, t.cert.string()));
    EXPECT_EQ(s.get_object("tls/obj").body, "small");
  }

  // Verifying client trusting a different (valid) CA: certificate is rejected.
  {
    const fs::path other_crt = t.fx.root / "other.crt", other_key = t.fx.root / "other.key";
    ASSERT_TRUE(make_self_signed(other_crt, other_key, true));
    aios::Session s(t.session_cfg(false, other_crt.string()));
    EXPECT_THROW(s.get_object("tls/obj"), std::exception);
  }
  // A trust store that is not a CA bundle is refused when the session is built.
  {
    EXPECT_THROW((aios::Session(t.session_cfg(false, t.key.string()))), std::exception);
  }
}

TEST(HttpTls, CertificateWithoutIpSanNeedsInsecureFromAnIpClient) {
  TlsHttpFixture t("aios-http-tls-nosan", /*with_ip_san=*/false);
  {
    aios::Session s(t.session_cfg(false, t.cert.string()));
    EXPECT_THROW(s.get_object("tls/none"), std::exception) << "no IP SAN => hostname mismatch";
  }
  {
    aios::Session s(t.session_cfg(true));
    EXPECT_FALSE(s.get_object("tls/none").exists);
  }
}

TEST(HttpTls, PosixMountAndBenchClientOverTls) {
  TlsHttpFixture t("aios-http-tls-posix");

  // libaios_posix (what aios-fuse, aios-fusell and the S3 gateway mount use).
  {
    const std::string ep = "https://" + t.hostport;
    aios_posix_config cfg{};
    cfg.endpoint = ep.c_str();
    cfg.cluster_key = t.fx.cfg.cluster_key.c_str();
    cfg.volume = "tlsvol";
    cfg.flags = AIOS_POSIX_F_TLS_INSECURE;
    int err = 0;
    aios_posix_fs* fs = aios_posix_mount(&cfg, &err);
    ASSERT_NE(fs, nullptr) << "mount errno " << err;
    aios_posix_stat st{};
    EXPECT_EQ(aios_posix_mkdir(fs, 1, "d", 0755, &st), 0);
    EXPECT_EQ(aios_posix_create(fs, st.ino, "f", 0644, &st), 0);
    EXPECT_EQ(aios_posix_lookup(fs, 1, "d", &st), 0);
    aios_posix_unmount(fs);

    // Without the insecure flag against a CN-only-trusted store the mount
    // itself has nothing to verify yet; the first request must fail.
    cfg.flags = 0;
    fs = aios_posix_mount(&cfg, &err);
    if (fs) {
      EXPECT_NE(aios_posix_lookup(fs, 1, "d", &st), 0);
      aios_posix_unmount(fs);
    }
  }

  // Bench client (raw HTTP path + aios::Session STL path).
  {
    aios::HttpBenchConfig c;
    c.endpoint = "https://" + t.hostport;
    c.cluster_key = t.fx.cfg.cluster_key;
    c.tls_insecure = true;
    c.threads = 2;
    c.ops = 4;
    c.warmup = 1;
    c.sizes = {1024};
    c.prefix = "tlsbench";
    auto r = aios::run_http_bench(c);
    ASSERT_FALSE(r.contains("error")) << r.dump();
    ASSERT_TRUE(r["results"].is_array());
    EXPECT_FALSE(r["results"].empty());
    for (const auto& phase : r["results"]) {
      EXPECT_EQ(phase.value("err", 1), 0) << phase.dump();
    }
  }
}

TEST(HttpTls, ServerRefusesHalfConfiguredTls) {
  DualStoreFixture fx("aios-http-tls-half");
  boost::asio::io_context ioc;
  fx.cfg.http_listen = "127.0.0.1:" + std::to_string(pid_port(19700));
  fx.cfg.http_tls_cert = (fx.root / "missing.crt").string();
  // cert without key
  EXPECT_THROW((aios::HttpServer(ioc, fx.cfg, *fx.svc, fx.membership)), std::exception);
  fx.cfg.http_tls_key = (fx.root / "missing.key").string();
  // both set but unreadable
  EXPECT_THROW((aios::HttpServer(ioc, fx.cfg, *fx.svc, fx.membership)), std::exception);
}
