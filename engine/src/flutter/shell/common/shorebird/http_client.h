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

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_CLIENT_H_
