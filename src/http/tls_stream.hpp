#pragma once

// Blocking byte stream over an accepted socket, plain or TLS.
//
// The front-ends drive their sockets with raw recv/send so that SO_RCVTIMEO /
// SO_SNDTIMEO bound every blocking call (see sock_io.hpp). TLS keeps that model:
// OpenSSL runs directly on the same blocking fd, so a stalled peer still
// surfaces as a timeout from the syscall underneath SSL_read/SSL_write.

#include <cstddef>
#include <memory>
#include <string>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace aios {

// Server-side TLS configuration: certificate + private key (+ optional chain),
// all PEM. Shared by every connection of a listener.
class TlsServerContext {
 public:
  ~TlsServerContext();
  TlsServerContext(const TlsServerContext&) = delete;
  TlsServerContext& operator=(const TlsServerContext&) = delete;

  // Loads the files, enforces TLS 1.2+, checks that key and cert match.
  static std::shared_ptr<TlsServerContext> load(const std::string& cert_pem,
                                                const std::string& key_pem,
                                                const std::string& chain_pem, std::string& err);

  SSL_CTX* native() const { return ctx_; }

 private:
  explicit TlsServerContext(SSL_CTX* ctx) : ctx_(ctx) {}
  SSL_CTX* ctx_{nullptr};
};

class TlsStream {
 public:
  // Plain stream over fd.
  explicit TlsStream(int fd);
  // TLS stream over fd when ctx is set (call accept() before any I/O); plain
  // when ctx is null, so listeners can pass their optional context through.
  TlsStream(int fd, std::shared_ptr<TlsServerContext> ctx);
  ~TlsStream();
  TlsStream(const TlsStream&) = delete;
  TlsStream& operator=(const TlsStream&) = delete;

  bool tls() const { return ssl_ != nullptr; }
  int fd() const { return fd_; }

  // Server handshake. No-op (true) for plain streams. On failure err_out
  // describes the reason (a plaintext client on the TLS port is the common one).
  bool accept(std::string& err_out);

  // Same contract as fd_read_some / fd_write_all in sock_io.hpp: read_some
  // returns bytes read, 0 on clean EOF, -1 on error (err_out = errno or a
  // synthetic EPROTO for a TLS-level failure).
  long read_some(void* out, std::size_t n, int& err_out);
  bool write_all(const void* in, std::size_t n, int& err_out);

 private:
  int fd_{-1};
  std::shared_ptr<TlsServerContext> ctx_;
  SSL* ssl_{nullptr};
  bool handshaken_{false};
};

}  // namespace aios
