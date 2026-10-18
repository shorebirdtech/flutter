// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/http_client.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "flutter/fml/logging.h"
#include "flutter/shell/common/shorebird/http_parser.h"
#include "flutter/shell/common/shorebird/http_trust.h"
#include "openssl/bio.h"
#include "openssl/err.h"
#include "openssl/pem.h"
#include "openssl/ssl.h"
#include "openssl/x509.h"
#include "openssl/x509v3.h"

namespace flutter {
namespace shorebird {

namespace {

constexpr char kUserAgent[] = "Shorebird-Updater";

using http_internal::ByteSource;
using http_internal::ResponseHead;
using http_internal::ResponseReader;
using http_internal::Url;

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void CloseSocket(SocketHandle s) {
  closesocket(s);
}
bool InterruptedOrRetry() {
  return false;
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void CloseSocket(SocketHandle s) {
  close(s);
}
bool InterruptedOrRetry() {
  return errno == EINTR;
}
#endif

#if defined(_WIN32)
bool EnsureWinsock() {
  static bool started = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return started;
}
#endif

// A connected TCP socket. Reads and writes block for at most the I/O
// timeout, and writing to a closed peer is an error, never SIGPIPE.
class Socket {
 public:
  Socket() = default;
  ~Socket() {
    if (handle_ != kInvalidSocket) {
      CloseSocket(handle_);
    }
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  HttpResult Connect(const Url& url, const HttpClientOptions& options) {
#if defined(_WIN32)
    if (!EnsureWinsock()) {
      return HttpResult::kTransportError;
    }
#endif
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    std::string port = std::to_string(url.port);
    if (getaddrinfo(url.host.c_str(), port.c_str(), &hints, &addresses) != 0 ||
        addresses == nullptr) {
      return HttpResult::kTransportError;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(addresses,
                                                             &freeaddrinfo);
    for (addrinfo* a = addresses; a != nullptr; a = a->ai_next) {
      if (TryConnect(a, options)) {
        return HttpResult::kOk;
      }
    }
    return HttpResult::kTransportError;
  }

  // Bytes read, 0 at end of stream, -1 on error or timeout.
  ptrdiff_t Read(uint8_t* buffer, size_t length) {
    int chunk = static_cast<int>(std::min<size_t>(length, 1 << 30));
    while (true) {
#if defined(_WIN32)
      int n = recv(handle_, reinterpret_cast<char*>(buffer), chunk, 0);
#else
      ssize_t n = recv(handle_, buffer, chunk, 0);
#endif
      if (n >= 0) {
        return static_cast<ptrdiff_t>(n);
      }
      if (!InterruptedOrRetry()) {
        return -1;
      }
    }
  }

  // Bytes written (possibly fewer than `length`), or -1.
  ptrdiff_t Write(const uint8_t* data, size_t length) {
    int chunk = static_cast<int>(std::min<size_t>(length, 1 << 30));
    while (true) {
#if defined(_WIN32)
      int n = send(handle_, reinterpret_cast<const char*>(data), chunk, 0);
#elif defined(MSG_NOSIGNAL)
      ssize_t n = send(handle_, data, chunk, MSG_NOSIGNAL);
#else
      // Apple: SO_NOSIGPIPE, set in TryConnect, does the same job.
      ssize_t n = send(handle_, data, chunk, 0);
#endif
      if (n >= 0) {
        return static_cast<ptrdiff_t>(n);
      }
      if (!InterruptedOrRetry()) {
        return -1;
      }
    }
  }

  bool WriteAll(const uint8_t* data, size_t length) {
    while (length > 0) {
      ptrdiff_t n = Write(data, length);
      if (n <= 0) {
        return false;
      }
      data += n;
      length -= static_cast<size_t>(n);
    }
    return true;
  }

 private:
  bool TryConnect(const addrinfo* address, const HttpClientOptions& options) {
    SocketHandle s =
        socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (s == kInvalidSocket) {
      return false;
    }
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (!SetBlocking(s, false) || !ConnectWithTimeout(s, address, options) ||
        !SetBlocking(s, true) || !SetIoTimeout(s, options.io_timeout)) {
      CloseSocket(s);
      return false;
    }
    int no_delay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
    handle_ = s;
    return true;
  }

  static bool SetBlocking(SocketHandle s, bool blocking) {
#if defined(_WIN32)
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
      return false;
    }
    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return fcntl(s, F_SETFL, flags) == 0;
#endif
  }

  static bool ConnectWithTimeout(SocketHandle s,
                                 const addrinfo* address,
                                 const HttpClientOptions& options) {
    int result = connect(s, address->ai_addr,
                         static_cast<int>(address->ai_addrlen));
    if (result == 0) {
      return true;
    }
#if defined(_WIN32)
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
      return false;
    }
    WSAPOLLFD poll_fd = {};
    poll_fd.fd = s;
    poll_fd.events = POLLWRNORM;
    int ready = WSAPoll(&poll_fd, 1,
                        static_cast<INT>(options.connect_timeout.count()));
#else
    if (errno != EINPROGRESS) {
      return false;
    }
    pollfd poll_fd = {};
    poll_fd.fd = s;
    poll_fd.events = POLLOUT;
    int ready;
    do {
      ready = poll(&poll_fd, 1, static_cast<int>(options.connect_timeout.count()));
    } while (ready < 0 && errno == EINTR);
#endif
    if (ready <= 0) {
      return false;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error),
                   &length) != 0) {
      return false;
    }
    return error == 0;
  }

  static bool SetIoTimeout(SocketHandle s, std::chrono::milliseconds timeout) {
#if defined(_WIN32)
    DWORD value = static_cast<DWORD>(timeout.count());
    const char* option = reinterpret_cast<const char*>(&value);
    int size = sizeof(value);
#else
    timeval value;
    value.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    value.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    const void* option = &value;
    socklen_t size = sizeof(value);
#endif
    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, option, size) == 0 &&
           setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, option, size) == 0;
  }

  SocketHandle handle_ = kInvalidSocket;
};

// A BIO over `Socket`, so BoringSSL's reads and writes go through
// `Socket::Read` and `Socket::Write` (and their SIGPIPE protection) rather
// than BoringSSL's own socket BIO, which uses write(2).
int SocketBioWrite(BIO* bio, const char* data, int length) {
  auto* socket = static_cast<Socket*>(BIO_get_data(bio));
  ptrdiff_t n = socket->Write(reinterpret_cast<const uint8_t*>(data),
                              static_cast<size_t>(length));
  return n < 0 ? -1 : static_cast<int>(n);
}

int SocketBioRead(BIO* bio, char* buffer, int length) {
  auto* socket = static_cast<Socket*>(BIO_get_data(bio));
  ptrdiff_t n =
      socket->Read(reinterpret_cast<uint8_t*>(buffer), static_cast<size_t>(length));
  return n < 0 ? -1 : static_cast<int>(n);
}

long SocketBioCtrl(BIO* bio, int command, long, void*) {
  return command == BIO_CTRL_FLUSH ? 1 : 0;
}

const BIO_METHOD* SocketBioMethod() {
  static const BIO_METHOD* method = [] {
    BIO_METHOD* m = BIO_meth_new(BIO_TYPE_SOCKET, "shorebird socket");
    if (m != nullptr) {
      BIO_meth_set_write(m, SocketBioWrite);
      BIO_meth_set_read(m, SocketBioRead);
      BIO_meth_set_ctrl(m, SocketBioCtrl);
    }
    return m;
  }();
  return method;
}

// The SSL_CTX for platform trust, built once: loading roots can mean
// reading the system store, which is too slow to repeat per request.
SSL_CTX* PlatformContext() {
  static SSL_CTX* context = [] {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
      return static_cast<SSL_CTX*>(nullptr);
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (!ConfigurePlatformTrust(ctx)) {
      FML_LOG(ERROR) << "Shorebird updater found no trusted root certificates";
      SSL_CTX_free(ctx);
      return static_cast<SSL_CTX*>(nullptr);
    }
    return ctx;
  }();
  return context;
}

bssl::UniquePtr<SSL_CTX> ContextWithRoots(const std::string& pem) {
  bssl::UniquePtr<SSL_CTX> ctx(SSL_CTX_new(TLS_client_method()));
  if (!ctx) {
    return nullptr;
  }
  SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
  bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(pem.data(), pem.size()));
  X509_STORE* store = SSL_CTX_get_cert_store(ctx.get());
  int added = 0;
  while (true) {
    bssl::UniquePtr<X509> cert(
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
    if (!cert) {
      break;
    }
    if (X509_STORE_add_cert(store, cert.get())) {
      added++;
    }
  }
  ERR_clear_error();
  if (added == 0) {
    return nullptr;
  }
  SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
  return ctx;
}

// Response bytes from a plain or TLS connection.
class Connection : public ByteSource {
 public:
  ~Connection() override {
    if (ssl_) {
      // Best effort; we are about to close the socket either way.
      SSL_shutdown(ssl_.get());
    }
    ERR_clear_error();
  }

  HttpResult Open(const Url& url, const HttpClientOptions& options) {
    HttpResult result = socket_.Connect(url, options);
    if (result != HttpResult::kOk || !url.https) {
      return result;
    }
    return StartTls(url, options);
  }

  bool WriteAll(const std::string& data) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    if (!ssl_) {
      return socket_.WriteAll(bytes, data.size());
    }
    size_t offset = 0;
    while (offset < data.size()) {
      int chunk =
          static_cast<int>(std::min<size_t>(data.size() - offset, 1 << 30));
      int n = SSL_write(ssl_.get(), bytes + offset, chunk);
      if (n <= 0) {
        ERR_clear_error();
        return false;
      }
      offset += static_cast<size_t>(n);
    }
    return true;
  }

  ptrdiff_t Read(uint8_t* buffer, size_t length) override {
    if (!ssl_) {
      return socket_.Read(buffer, length);
    }
    int chunk = static_cast<int>(std::min<size_t>(length, 1 << 30));
    int n = SSL_read(ssl_.get(), buffer, chunk);
    if (n > 0) {
      return n;
    }
    int error = SSL_get_error(ssl_.get(), n);
    ERR_clear_error();
    if (error == SSL_ERROR_ZERO_RETURN) {
      return 0;
    }
    // A peer that closes without close_notify. Many servers do; framing
    // (Content-Length or chunked) still catches a truncated body.
    if (error == SSL_ERROR_SYSCALL && n == 0) {
      return 0;
    }
    return -1;
  }

 private:
  HttpResult StartTls(const Url& url, const HttpClientOptions& options) {
    SSL_CTX* ctx;
    if (options.trusted_roots_pem.has_value()) {
      custom_context_ = ContextWithRoots(*options.trusted_roots_pem);
      ctx = custom_context_.get();
    } else {
      ctx = PlatformContext();
    }
    const BIO_METHOD* method = SocketBioMethod();
    if (ctx == nullptr || method == nullptr) {
      return HttpResult::kTlsError;
    }
    ssl_.reset(SSL_new(ctx));
    BIO* bio = BIO_new(method);
    if (!ssl_ || bio == nullptr) {
      BIO_free(bio);
      return HttpResult::kTlsError;
    }
    BIO_set_data(bio, &socket_);
    BIO_set_init(bio, 1);
    // SSL takes ownership of the BIO.
    SSL_set_bio(ssl_.get(), bio, bio);

    host_ = url.host;
    X509_VERIFY_PARAM* param = SSL_get0_param(ssl_.get());
    X509_VERIFY_PARAM_set_hostflags(param,
                                    X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    bool host_set = url.host_is_ip_literal
                        ? X509_VERIFY_PARAM_set1_ip_asc(param, host_.c_str())
                        : X509_VERIFY_PARAM_set1_host(param, host_.data(),
                                                      host_.size());
    if (!host_set) {
      return HttpResult::kTlsError;
    }
    if (!url.host_is_ip_literal &&
        !SSL_set_tlsext_host_name(ssl_.get(), host_.c_str())) {
      return HttpResult::kTlsError;
    }
    // Verification is always on. Apple replaces BoringSSL's chain check
    // with the system's trust evaluation, which checks the host itself.
    SSL_set_verify(ssl_.get(), SSL_VERIFY_PEER, nullptr);
    if (!options.trusted_roots_pem.has_value()) {
      ConfigurePlatformVerification(ssl_.get(), &host_);
    }

    ERR_clear_error();
    int result = SSL_connect(ssl_.get());
    if (result == 1) {
      return HttpResult::kOk;
    }
    int error = SSL_get_error(ssl_.get(), result);
    long verify = SSL_get_verify_result(ssl_.get());
    // The certificate's problem if verification ran and failed, otherwise
    // BoringSSL's (a protocol version or cipher the server insists on).
    const char* reason = verify != X509_V_OK
                             ? X509_verify_cert_error_string(verify)
                             : ERR_reason_error_string(ERR_peek_last_error());
    std::string reason_text = reason != nullptr ? reason : "unknown";
    ERR_clear_error();
    // Don't send close_notify on a connection that never opened.
    ssl_.reset();
    if (error == SSL_ERROR_SYSCALL && verify == X509_V_OK) {
      // The socket failed or timed out mid-handshake.
      return HttpResult::kTransportError;
    }
    FML_LOG(ERROR) << "Shorebird updater TLS handshake with " << url.host
                   << " failed: " << reason_text;
    return HttpResult::kTlsError;
  }

  Socket socket_;
  bssl::UniquePtr<SSL_CTX> custom_context_;
  bssl::UniquePtr<SSL> ssl_;
  std::string host_;
};

bool IsReservedHeader(const std::string& name) {
  static const char* kReserved[] = {"host", "content-length",
                                    "transfer-encoding", "connection"};
  for (const char* reserved : kReserved) {
    if (http_internal::EqualsIgnoreCase(name, reserved)) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> BuildRequest(const HttpRequest& request,
                                        const Url& url) {
  if (request.method != "GET" && request.method != "POST") {
    return std::nullopt;
  }
  std::string head = request.method + " " + url.target + " HTTP/1.1\r\n";
  head += "Host: " + http_internal::Authority(url) + "\r\n";
  bool has_user_agent = false;
  bool has_accept = false;
  for (const auto& header : request.headers) {
    if (!http_internal::IsToken(header.name) ||
        !http_internal::IsFieldValue(header.value) ||
        IsReservedHeader(header.name)) {
      return std::nullopt;
    }
    has_user_agent |= http_internal::EqualsIgnoreCase(header.name, "user-agent");
    has_accept |= http_internal::EqualsIgnoreCase(header.name, "accept");
    head += header.name + ": " + header.value + "\r\n";
  }
  if (!has_user_agent) {
    head += std::string("User-Agent: ") + kUserAgent + "\r\n";
  }
  if (!has_accept) {
    head += "Accept: */*\r\n";
  }
  if (request.method == "POST" || !request.body.empty()) {
    head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  }
  head += "Connection: close\r\n\r\n";
  return head + request.body;
}

bool IsRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 ||
         status == 308;
}

}  // namespace

HttpResult SendHttpRequest(const HttpRequest& request,
                           HttpResponseHandler& handler,
                           const HttpClientOptions& options) {
  std::string url_text = request.url;
  for (int redirects = 0;; redirects++) {
    std::optional<Url> url = http_internal::ParseUrl(url_text);
    if (!url.has_value()) {
      return HttpResult::kInvalidRequest;
    }
    std::optional<std::string> wire = BuildRequest(request, *url);
    if (!wire.has_value()) {
      return HttpResult::kInvalidRequest;
    }

    Connection connection;
    HttpResult result = connection.Open(*url, options);
    if (result != HttpResult::kOk) {
      return result;
    }
    if (!connection.WriteAll(*wire)) {
      return HttpResult::kTransportError;
    }

    ResponseReader reader(connection);
    ResponseHead head;
    result = reader.ReadHead(head);
    if (result != HttpResult::kOk) {
      return result;
    }
    // Redirects are followed for GET only. Nothing the updater POSTs is
    // expected to redirect, and replaying a POST elsewhere is not safe.
    const std::string* location =
        http_internal::FindHeader(head.headers, "location");
    if (request.method == "GET" && IsRedirect(head.status) &&
        location != nullptr) {
      if (redirects >= options.max_redirects) {
        return HttpResult::kProtocolError;
      }
      std::optional<std::string> next =
          http_internal::ResolveRedirect(*url, *location);
      if (!next.has_value()) {
        return HttpResult::kProtocolError;
      }
      // Never from https to plain http: the request (and the headers it
      // carries) would leave TLS.
      std::optional<Url> next_url = http_internal::ParseUrl(*next);
      if (url->https && next_url.has_value() && !next_url->https) {
        return HttpResult::kProtocolError;
      }
      url_text = *next;
      continue;
    }

    if (!handler.OnResponse(head.status, head.headers)) {
      return HttpResult::kAborted;
    }
    return reader.ReadBody(head, handler);
  }
}

}  // namespace shorebird
}  // namespace flutter
