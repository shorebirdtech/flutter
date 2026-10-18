// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_CLIENT_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_CLIENT_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flutter {
namespace shorebird {

// A small blocking HTTP/1.1 client on the engine's BoringSSL, used by the
// Shorebird updater for patch checks, patch downloads and event reports.
//
// One request per connection (`Connection: close`), GET and POST, redirects
// followed for GET, bodies framed by Content-Length, chunked encoding or
// connection close. Certificates are checked against the platform's trusted
// roots, found the way dart:io finds them (see http_trust_*.cc).

struct HttpHeader {
  std::string name;
  std::string value;
};

struct HttpRequest {
  // "GET" or "POST".
  std::string method = "GET";
  // An absolute http:// or https:// URL.
  std::string url;
  // Extra request headers. The client owns the framing headers (Host,
  // Content-Length, Transfer-Encoding, Connection), and a request naming any
  // of them is invalid.
  std::vector<HttpHeader> headers;
  std::string body;
};

// Receives a response. Both methods run on the thread that called
// `SendHttpRequest`, before it returns. Returning false stops the request,
// and `SendHttpRequest` returns `HttpResult::kAborted`.
class HttpResponseHandler {
 public:
  virtual ~HttpResponseHandler() = default;
  // The status and headers of the final response, after redirects, before
  // any body bytes.
  virtual bool OnResponse(int status,
                          const std::vector<HttpHeader>& headers) = 0;
  // The next run of decoded body bytes.
  virtual bool OnBody(const uint8_t* data, size_t length) = 0;
};

enum class HttpResult {
  // The whole response was delivered, whatever its status.
  kOk,
  // DNS, connect, read or write failure, or a timeout.
  kTransportError,
  // The TLS handshake failed, including an untrusted certificate.
  kTlsError,
  // The response was not valid HTTP/1.1, or redirected too many times.
  kProtocolError,
  // The handler returned false.
  kAborted,
  // The request was unusable: a bad URL, method or header.
  kInvalidRequest,
};

struct HttpClientOptions {
  std::chrono::milliseconds connect_timeout{30000};
  // The longest a single read or write may wait for the peer.
  std::chrono::milliseconds io_timeout{60000};
  int max_redirects = 10;
  // PEM roots to trust instead of the platform's. For tests.
  std::optional<std::string> trusted_roots_pem;
};

HttpResult SendHttpRequest(const HttpRequest& request,
                           HttpResponseHandler& handler,
                           const HttpClientOptions& options = {});

// Exposed for tests.
namespace http_internal {

struct Url {
  bool https = false;
  // Without brackets for an IPv6 literal.
  std::string host;
  uint16_t port = 0;
  // Path and query, at least "/".
  std::string target;
  bool host_is_ip_literal = false;
};

std::optional<Url> ParseUrl(const std::string& url);

// Resolves a Location header value against the URL that returned it.
std::optional<std::string> ResolveRedirect(const Url& base,
                                           const std::string& location);

// A source of response bytes, so the response parser can be tested and
// fuzzed without a socket.
class ByteSource {
 public:
  virtual ~ByteSource() = default;
  // Reads up to `length` bytes. Returns the count, 0 at end of stream, or
  // -1 on a transport error.
  virtual ptrdiff_t Read(uint8_t* buffer, size_t length) = 0;
};

struct ResponseHead {
  int status = 0;
  std::vector<HttpHeader> headers;
};

// Parses one HTTP/1.1 response to a GET or POST from a `ByteSource`: first
// the head, skipping interim 1xx responses, then the body.
class ResponseReader {
 public:
  explicit ResponseReader(ByteSource& source) : source_(source) {}

  HttpResult ReadHead(ResponseHead& head);
  // Delivers the body of `head` to `handler.OnBody`.
  HttpResult ReadBody(const ResponseHead& head, HttpResponseHandler& handler);

 private:
  enum class LineResult { kOk, kEndOfStream, kTransportError, kTooLong };

  // Reads a line ending in LF (CR before it is dropped) of at most
  // `max_length` bytes, counting against `budget`.
  LineResult ReadLine(std::string& line, size_t& budget);
  // Reads up to `length` buffered or fresh bytes into `out`.
  ptrdiff_t ReadSome(uint8_t* out, size_t length);
  bool Fill();
  static HttpResult LineError(LineResult result);

  HttpResult ReadFixedBody(uint64_t length, HttpResponseHandler& handler);
  HttpResult ReadChunkedBody(HttpResponseHandler& handler);
  HttpResult ReadBodyUntilClose(HttpResponseHandler& handler);

  ByteSource& source_;
  uint8_t buffer_[16 * 1024];
  size_t start_ = 0;
  size_t end_ = 0;
  bool transport_error_ = false;
};

}  // namespace http_internal

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_CLIENT_H_
