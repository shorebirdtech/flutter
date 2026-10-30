// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_CLIENT_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_CLIENT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flutter {
namespace shorebird {

// The HTTP client the Shorebird updater uses on iOS, macOS and Android, for
// patch checks, patch downloads and event reports: the platform's own HTTP
// stack, so its trust store, revocation and proxy settings apply. Each
// platform implements `SendHttpRequest` and adds
// SHOREBIRD_PLATFORM_HTTP_CLIENT to the defines of :engine_callbacks in
// BUILD.gn; engine_callbacks.cc hands it to the updater. On other platforms
// the updater sends requests itself.
//
// An implementation follows redirects for GET only and never from https to
// http, and delivers body bytes exactly as received: the updater sends
// `Accept-Encoding: identity` and checks the bytes against Content-Length.

struct HttpHeader {
  std::string name;
  std::string value;
};

struct HttpRequest {
  // "GET" or "POST".
  std::string method = "GET";
  // An absolute http:// or https:// URL.
  std::string url;
  std::vector<HttpHeader> headers;
  std::string body;
  // How long the whole request may take, in milliseconds, before it fails
  // with `HttpResult::kTransportError`; 0 for no limit beyond the client's
  // own.
  uint32_t timeout_ms = 0;
};

// Receives a response. Both methods run on the thread that called
// `SendHttpRequest`, before it returns. Returning false stops the request,
// and `SendHttpRequest` returns `HttpResult::kAborted`.
class HttpResponseHandler {
 public:
  virtual ~HttpResponseHandler() = default;
  // The status and headers of the final response, after redirects, before
  // any body bytes. Called exactly once for a delivered response.
  virtual bool OnResponse(int status,
                          const std::vector<HttpHeader>& headers) = 0;
  // The next run of body bytes, undecoded.
  virtual bool OnBody(const uint8_t* data, size_t length) = 0;
};

enum class HttpResult {
  // The whole response was delivered, whatever its status.
  kOk,
  // DNS, connect, read or write failure, or a timeout.
  kTransportError,
  // The TLS handshake failed, including an untrusted certificate.
  kTlsError,
  // The response was not valid HTTP, or redirected too many times.
  kProtocolError,
  // The handler returned false.
  kAborted,
  // The request was unusable: a bad URL, method or header.
  kInvalidRequest,
};

// Sends `request`, blocking until the response is delivered to `handler` or
// the request fails. Safe to call from any thread. On a result other than
// kOk or kAborted, sets `*error` to why, in the platform's words.
using HttpSender = HttpResult (*)(const HttpRequest& request,
                                  HttpResponseHandler& handler,
                                  std::string* error);

// The platform's client, where SHOREBIRD_PLATFORM_HTTP_CLIENT is defined.
HttpResult SendHttpRequest(const HttpRequest& request,
                           HttpResponseHandler& handler,
                           std::string* error);

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_CLIENT_H_
