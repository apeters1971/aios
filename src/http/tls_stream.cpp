#include "http/tls_stream.hpp"

#include "http/sock_io.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

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

TlsStream::TlsStream(int fd) : fd_(fd) {}

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
