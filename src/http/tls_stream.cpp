#include "http/tls_stream.hpp"

#include "http/sock_io.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cerrno>
#include <cstring>

namespace aios {
namespace {

std::string openssl_error_string() {
  std::string out;
  while (unsigned long e = ERR_get_error()) {
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    if (!out.empty()) out += "; ";
    out += buf;
  }
  return out.empty() ? std::string("unknown OpenSSL error") : out;
}

}  // namespace

TlsServerContext::~TlsServerContext() {
  if (ctx_) SSL_CTX_free(ctx_);
}

std::shared_ptr<TlsServerContext> TlsServerContext::load(const std::string& cert_pem,
                                                         const std::string& key_pem,
                                                         const std::string& chain_pem,
                                                         std::string& err) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) {
    err = "SSL_CTX_new: " + openssl_error_string();
    return nullptr;
  }
  std::shared_ptr<TlsServerContext> out(new TlsServerContext(ctx));
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE);
  // Server-side only: we never ask S3 clients for certificates.
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

  // The cert file may itself be a full chain (leaf first, as Let's Encrypt's
  // fullchain.pem); a separate chain file appends further intermediates.
  if (SSL_CTX_use_certificate_chain_file(ctx, cert_pem.c_str()) != 1) {
    err = "load certificate " + cert_pem + ": " + openssl_error_string();
    return nullptr;
  }
  if (!chain_pem.empty()) {
    BIO* bio = BIO_new_file(chain_pem.c_str(), "r");
    if (!bio) {
      err = "open chain " + chain_pem + ": " + openssl_error_string();
      return nullptr;
    }
    int added = 0;
    while (X509* x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
      // add0: the context takes ownership.
      if (SSL_CTX_add0_chain_cert(ctx, x) != 1) {
        X509_free(x);
        BIO_free(bio);
        err = "add chain cert from " + chain_pem + ": " + openssl_error_string();
        return nullptr;
      }
      ++added;
    }
    BIO_free(bio);
    ERR_clear_error();  // PEM_read at EOF leaves a "no start line" error queued
    if (added == 0) {
      err = "no certificates found in chain file " + chain_pem;
      return nullptr;
    }
  }
  if (SSL_CTX_use_PrivateKey_file(ctx, key_pem.c_str(), SSL_FILETYPE_PEM) != 1) {
    err = "load private key " + key_pem + ": " + openssl_error_string();
    return nullptr;
  }
  if (SSL_CTX_check_private_key(ctx) != 1) {
    err = "private key does not match certificate: " + openssl_error_string();
    return nullptr;
  }
  return out;
}

TlsClientContext::~TlsClientContext() {
  if (ctx_) SSL_CTX_free(ctx_);
}

std::shared_ptr<TlsClientContext> TlsClientContext::create(const std::string& ca_pem,
                                                           bool insecure, std::string& err) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    err = "SSL_CTX_new: " + openssl_error_string();
    return nullptr;
  }
  std::shared_ptr<TlsClientContext> out(new TlsClientContext(ctx, insecure));
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
  if (insecure) {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    return out;
  }
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
  if (!ca_pem.empty()) {
    if (SSL_CTX_load_verify_locations(ctx, ca_pem.c_str(), nullptr) != 1) {
      err = "load CA " + ca_pem + ": " + openssl_error_string();
      return nullptr;
    }
  } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
    err = "system CA store: " + openssl_error_string();
    return nullptr;
  }
  return out;
}

bool split_endpoint_scheme(const std::string& endpoint, bool& tls_out, std::string& hostport_out) {
  tls_out = false;
  hostport_out = endpoint;
  auto strip = [&](const char* prefix, bool tls) {
    const std::size_t n = std::strlen(prefix);
    if (endpoint.size() > n && endpoint.compare(0, n, prefix) == 0) {
      tls_out = tls;
      hostport_out = endpoint.substr(n);
      while (!hostport_out.empty() && hostport_out.back() == '/') hostport_out.pop_back();
      return true;
    }
    return false;
  };
  if (strip("https://", true) || strip("http://", false)) return true;
  return endpoint.find("://") == std::string::npos;
}

TlsStream::TlsStream(int fd) : fd_(fd) {}

TlsStream::TlsStream(int fd, std::shared_ptr<TlsClientContext> ctx, const std::string& host)
    : fd_(fd), cctx_(std::move(ctx)) {
  if (!cctx_) return;
  ssl_ = SSL_new(cctx_->native());
  if (!ssl_) return;
  SSL_set_fd(ssl_, fd_);
  // SNI + hostname verification. An IP literal has no name to match, so only
  // set the host check for DNS names (IPs are matched via the SAN IP entries
  // when present; a plain IP with a CN-only cert needs --tls-insecure or a
  // cert that carries an IP SAN).
  const bool is_ip = !host.empty() && (host.find_first_not_of("0123456789.") == std::string::npos ||
                                       host.find(':') != std::string::npos);
  if (!is_ip) SSL_set_tlsext_host_name(ssl_, host.c_str());
  if (!cctx_->insecure()) {
    X509_VERIFY_PARAM* vp = SSL_get0_param(ssl_);
    X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (is_ip) X509_VERIFY_PARAM_set1_ip_asc(vp, host.c_str());
    else X509_VERIFY_PARAM_set1_host(vp, host.c_str(), 0);
  }
}

bool TlsStream::connect(std::string& err_out) {
  if (!ssl_) {
    if (cctx_) {
      err_out = "SSL_new failed";
      return false;
    }
    return true;
  }
  ERR_clear_error();
  const int r = SSL_connect(ssl_);
  if (r == 1) {
    handshaken_ = true;
    return true;
  }
  const int reason = SSL_get_error(ssl_, r);
  if (reason == SSL_ERROR_SYSCALL) {
    const int e = errno;
    err_out = e == 0 ? "server closed during TLS handshake (plain HTTP endpoint?)"
                     : std::string(strerror(e));
    if (e == EAGAIN || e == EWOULDBLOCK) err_out = "TLS handshake timed out";
  } else {
    const long vr = SSL_get_verify_result(ssl_);
    err_out = openssl_error_string();
    if (vr != X509_V_OK) {
      err_out += std::string(" (certificate: ") + X509_verify_cert_error_string(vr) + ")";
    }
  }
  return false;
}

TlsStream::TlsStream(int fd, std::shared_ptr<TlsServerContext> ctx)
    : fd_(fd), ctx_(std::move(ctx)) {
  if (ctx_) {
    ssl_ = SSL_new(ctx_->native());
    if (ssl_) SSL_set_fd(ssl_, fd_);
  }
}

TlsStream::~TlsStream() {
  if (ssl_) {
    // One-way close_notify; do not wait for the peer's (it may be gone).
    if (handshaken_) SSL_shutdown(ssl_);
    SSL_free(ssl_);
  }
}

bool TlsStream::accept(std::string& err_out) {
  if (!ssl_) {
    if (ctx_) {
      err_out = "SSL_new failed";
      return false;
    }
    return true;
  }
  ERR_clear_error();
  const int r = SSL_accept(ssl_);
  if (r == 1) {
    handshaken_ = true;
    return true;
  }
  const int reason = SSL_get_error(ssl_, r);
  if (reason == SSL_ERROR_SYSCALL) {
    const int e = errno;
    err_out = e == 0 ? "peer closed during handshake" : std::string(strerror(e));
    if (e == EAGAIN || e == EWOULDBLOCK) err_out = "handshake timed out";
  } else {
    err_out = openssl_error_string();
  }
  return false;
}

long TlsStream::read_some(void* out, std::size_t n, int& err_out) {
  if (!ssl_) return fd_read_some(fd_, out, n, err_out);
  for (;;) {
    ERR_clear_error();
    const int r = SSL_read(ssl_, out, static_cast<int>(n));
    if (r > 0) {
      err_out = 0;
      return r;
    }
    const int reason = SSL_get_error(ssl_, r);
    if (reason == SSL_ERROR_ZERO_RETURN) {
      err_out = 0;
      return 0;
    }
    if (reason == SSL_ERROR_SYSCALL) {
      if (errno == EINTR) continue;
      // errno 0 here means the peer went away without close_notify (curl,
      // most SDKs): report it as a clean EOF like the plain path does.
      err_out = errno;
      return errno == 0 ? 0 : -1;
    }
    err_out = EPROTO;
    return -1;
  }
}

bool TlsStream::write_all(const void* in, std::size_t n, int& err_out) {
  if (!ssl_) return fd_write_all(fd_, in, n, err_out);
  const auto* p = static_cast<const char*>(in);
  std::size_t done = 0;
  while (done < n) {
    ERR_clear_error();
    const int r = SSL_write(ssl_, p + done, static_cast<int>(n - done));
    if (r > 0) {
      done += static_cast<std::size_t>(r);
      continue;
    }
    const int reason = SSL_get_error(ssl_, r);
    if (reason == SSL_ERROR_SYSCALL && errno == EINTR) continue;
    err_out = reason == SSL_ERROR_SYSCALL ? (errno ? errno : EPIPE) : EPROTO;
    return false;
  }
  err_out = 0;
  return true;
}

}  // namespace aios
