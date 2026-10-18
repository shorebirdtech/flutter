// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_PARSER_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "flutter/shell/common/shorebird/http_client.h"

// The parts of the HTTP client that touch no socket and no TLS: URLs,
// redirects and the response parser. They build on their own (no fml, no
// BoringSSL), so the fuzzer and the llhttp differential test can compile
// them anywhere.

namespace flutter {
namespace shorebird {
namespace http_internal {

// HTTP token and field-value grammar (RFC 9110 5.6.2, 5.5).
bool IsToken(const std::string& s);
bool IsFieldValue(const std::string& s);
bool EqualsIgnoreCase(const std::string& a, const char* b);
// The first header named `name`, case-insensitively, or null.
const std::string* FindHeader(const std::vector<HttpHeader>& headers,
                              const char* name);

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

// The Host header value for `url`: the host, bracketed if IPv6, and the
// port unless it is the scheme's default.
std::string Authority(const Url& url);

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

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_PARSER_H_
