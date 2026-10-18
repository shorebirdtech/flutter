// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/http_parser.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace flutter {
namespace shorebird {

namespace http_internal {

namespace {

// Status line plus headers.
constexpr size_t kMaxHeadBytes = 64 * 1024;
constexpr size_t kMaxHeaders = 128;
constexpr size_t kMaxTrailerBytes = 64 * 1024;
constexpr size_t kMaxChunkSizeLine = 1024;
constexpr int kMaxInterimResponses = 16;

bool IsTokenChar(char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9')) {
    return true;
  }
  return std::strchr("!#$%&'*+-.^_`|~", c) != nullptr && c != '\0';
}

}  // namespace

bool IsToken(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), IsTokenChar);
}

// Field values may hold visible ASCII, spaces, tabs and obs-text, but no
// control characters: a CR or LF would let a value inject headers.
bool IsFieldValue(const std::string& s) {
  return std::none_of(s.begin(), s.end(), [](char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return (u < 0x20 && u != '\t') || u == 0x7f;
  });
}

bool EqualsIgnoreCase(const std::string& a, const char* b) {
  size_t n = std::strlen(b);
  if (a.size() != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') {
      x = static_cast<char>(x - 'A' + 'a');
    }
    if (y >= 'A' && y <= 'Z') {
      y = static_cast<char>(y - 'A' + 'a');
    }
    if (x != y) {
      return false;
    }
  }
  return true;
}

namespace {

std::string ToLower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

std::string TrimWhitespace(const std::string& s) {
  size_t begin = s.find_first_not_of(" \t");
  if (begin == std::string::npos) {
    return "";
  }
  size_t end = s.find_last_not_of(" \t");
  return s.substr(begin, end - begin + 1);
}

}  // namespace

const std::string* FindHeader(const std::vector<HttpHeader>& headers,
                              const char* name) {
  for (const auto& header : headers) {
    if (EqualsIgnoreCase(header.name, name)) {
      return &header.value;
    }
  }
  return nullptr;
}

namespace {

bool IsDecimal(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(),
                                   [](char c) { return c >= '0' && c <= '9'; });
}

// Parses up to 18 decimal digits, so the value fits in 63 bits. (No
// std::stoull: the engine builds without exceptions.)
std::optional<uint64_t> ParseDecimal(const std::string& s) {
  if (!IsDecimal(s) || s.size() > 18) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (char c : s) {
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  return value;
}

bool ParsePort(const std::string& s, uint16_t& port) {
  std::optional<uint64_t> value = ParseDecimal(s);
  if (!value.has_value() || *value == 0 || *value > 65535) {
    return false;
  }
  port = static_cast<uint16_t>(*value);
  return true;
}

bool IsIpv4Literal(const std::string& host) {
  in_addr addr;
  return inet_pton(AF_INET, host.c_str(), &addr) == 1;
}

bool IsIpv6Literal(const std::string& host) {
  in6_addr addr;
  return inet_pton(AF_INET6, host.c_str(), &addr) == 1;
}

}  // namespace

std::optional<Url> ParseUrl(const std::string& url) {
  // Spaces and control characters would corrupt the request line.
  for (char c : url) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7f) {
      return std::nullopt;
    }
  }
  size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    return std::nullopt;
  }
  Url result;
  std::string scheme = ToLower(url.substr(0, scheme_end));
  if (scheme == "https") {
    result.https = true;
    result.port = 443;
  } else if (scheme == "http") {
    result.port = 80;
  } else {
    return std::nullopt;
  }

  size_t authority_start = scheme_end + 3;
  size_t authority_end = url.find_first_of("/?#", authority_start);
  if (authority_end == std::string::npos) {
    authority_end = url.size();
  }
  std::string authority =
      url.substr(authority_start, authority_end - authority_start);
  // Credentials in URLs are never ours to send.
  if (authority.empty() || authority.find('@') != std::string::npos) {
    return std::nullopt;
  }

  std::string port_text;
  if (authority[0] == '[') {
    size_t close = authority.find(']');
    if (close == std::string::npos) {
      return std::nullopt;
    }
    result.host = authority.substr(1, close - 1);
    if (!IsIpv6Literal(result.host)) {
      return std::nullopt;
    }
    result.host_is_ip_literal = true;
    std::string rest = authority.substr(close + 1);
    if (!rest.empty()) {
      if (rest[0] != ':') {
        return std::nullopt;
      }
      port_text = rest.substr(1);
      if (port_text.empty()) {
        return std::nullopt;
      }
    }
  } else {
    size_t colon = authority.find(':');
    result.host = authority.substr(0, colon);
    if (colon != std::string::npos) {
      port_text = authority.substr(colon + 1);
      if (port_text.empty()) {
        return std::nullopt;
      }
    }
    if (result.host.empty()) {
      return std::nullopt;
    }
    for (char c : result.host) {
      bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
      if (!ok) {
        return std::nullopt;
      }
    }
    result.host_is_ip_literal = IsIpv4Literal(result.host);
  }
  if (!port_text.empty() && !ParsePort(port_text, result.port)) {
    return std::nullopt;
  }

  size_t fragment = url.find('#', authority_end);
  std::string target = url.substr(
      authority_end, fragment == std::string::npos ? std::string::npos
                                                   : fragment - authority_end);
  if (target.empty() || target[0] == '?') {
    target = "/" + target;
  }
  result.target = target;
  return result;
}

std::string Authority(const Url& url) {
  std::string host =
      url.host.find(':') != std::string::npos ? "[" + url.host + "]" : url.host;
  bool default_port = url.port == (url.https ? 443 : 80);
  return default_port ? host : host + ":" + std::to_string(url.port);
}

std::optional<std::string> ResolveRedirect(const Url& base,
                                           const std::string& location) {
  if (location.empty()) {
    return std::nullopt;
  }
  std::string scheme = base.https ? "https" : "http";
  if (location.find("://") != std::string::npos &&
      location.find("://") < location.find_first_of("/?#")) {
    return location;
  }
  if (location.rfind("//", 0) == 0) {
    return scheme + ":" + location;
  }
  std::string prefix = scheme + "://" + Authority(base);
  if (location[0] == '/') {
    return prefix + location;
  }
  // A relative reference: resolve against the base path's directory.
  std::string path = base.target.substr(0, base.target.find('?'));
  size_t slash = path.rfind('/');
  std::string directory = path.substr(0, slash + 1);
  if (location[0] == '?') {
    return prefix + path + location;
  }
  return prefix + directory + location;
}

bool ResponseReader::Fill() {
  if (start_ == end_) {
    start_ = 0;
    end_ = 0;
  }
  if (end_ == sizeof(buffer_)) {
    return false;
  }
  ptrdiff_t n = source_.Read(buffer_ + end_, sizeof(buffer_) - end_);
  if (n < 0) {
    transport_error_ = true;
    return false;
  }
  if (n == 0) {
    return false;
  }
  end_ += static_cast<size_t>(n);
  return true;
}

ptrdiff_t ResponseReader::ReadSome(uint8_t* out, size_t length) {
  if (start_ == end_ && !Fill()) {
    return transport_error_ ? -1 : 0;
  }
  size_t n = std::min(length, end_ - start_);
  std::memcpy(out, buffer_ + start_, n);
  start_ += n;
  return static_cast<ptrdiff_t>(n);
}

ResponseReader::LineResult ResponseReader::ReadLine(std::string& line,
                                                    size_t& budget) {
  line.clear();
  while (true) {
    if (start_ == end_ && !Fill()) {
      return transport_error_ ? LineResult::kTransportError
                              : LineResult::kEndOfStream;
    }
    const uint8_t* begin = buffer_ + start_;
    size_t available = end_ - start_;
    const void* newline = std::memchr(begin, '\n', available);
    size_t take = newline != nullptr
                      ? static_cast<const uint8_t*>(newline) - begin + 1
                      : available;
    if (take > budget) {
      return LineResult::kTooLong;
    }
    budget -= take;
    line.append(reinterpret_cast<const char*>(begin), take);
    start_ += take;
    if (newline != nullptr) {
      line.pop_back();
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return LineResult::kOk;
    }
  }
}

HttpResult ResponseReader::LineError(LineResult result) {
  // A line that runs past its limit is malformed. A stream that ends before
  // the line does means the connection dropped.
  return result == LineResult::kTooLong ? HttpResult::kProtocolError
                                        : HttpResult::kTransportError;
}

namespace {

bool ParseStatusLine(const std::string& line, int& status) {
  // HTTP/1.x SP 3DIGIT [SP reason]
  if (line.size() < 12 || line.compare(0, 7, "HTTP/1.") != 0 ||
      (line[7] != '0' && line[7] != '1') || line[8] != ' ') {
    return false;
  }
  std::optional<uint64_t> code = ParseDecimal(line.substr(9, 3));
  if (!code.has_value() || line.substr(9, 3).size() != 3 ||
      (line.size() > 12 && line[12] != ' ')) {
    return false;
  }
  status = static_cast<int>(*code);
  return status >= 100 && status <= 599;
}

}  // namespace

HttpResult ResponseReader::ReadHead(ResponseHead& head) {
  for (int interim = 0; interim <= kMaxInterimResponses; interim++) {
    size_t budget = kMaxHeadBytes;
    std::string line;
    LineResult result = ReadLine(line, budget);
    if (result != LineResult::kOk) {
      return LineError(result);
    }
    head.headers.clear();
    if (!ParseStatusLine(line, head.status)) {
      return HttpResult::kProtocolError;
    }
    while (true) {
      result = ReadLine(line, budget);
      if (result != LineResult::kOk) {
        return LineError(result);
      }
      if (line.empty()) {
        break;
      }
      // Obsolete line folding is allowed to be rejected (RFC 9112 5.2).
      if (line[0] == ' ' || line[0] == '\t') {
        return HttpResult::kProtocolError;
      }
      size_t colon = line.find(':');
      if (colon == std::string::npos) {
        return HttpResult::kProtocolError;
      }
      HttpHeader header{line.substr(0, colon),
                        TrimWhitespace(line.substr(colon + 1))};
      if (!IsToken(header.name) || !IsFieldValue(header.value) ||
          head.headers.size() == kMaxHeaders) {
        return HttpResult::kProtocolError;
      }
      head.headers.push_back(std::move(header));
    }
    if (head.status >= 200) {
      return HttpResult::kOk;
    }
    // We never ask to switch protocols.
    if (head.status == 101) {
      return HttpResult::kProtocolError;
    }
    // Any other 1xx is interim: the real response follows.
  }
  return HttpResult::kProtocolError;
}

HttpResult ResponseReader::ReadBody(const ResponseHead& head,
                                    HttpResponseHandler& handler) {
  // RFC 9112 6.3: these never have a body.
  if (head.status == 204 || head.status == 304) {
    return HttpResult::kOk;
  }
  // Repeated Transfer-Encoding fields are one comma-separated list (RFC
  // 9110 5.3), and only the last coding decides the framing.
  std::optional<std::string> te;
  for (const auto& header : head.headers) {
    if (EqualsIgnoreCase(header.name, "transfer-encoding")) {
      te = te.has_value() ? *te + "," + header.value : header.value;
    }
  }
  if (te.has_value()) {
    std::string codings = ToLower(*te);
    size_t comma = codings.rfind(',');
    std::string last = TrimWhitespace(
        comma == std::string::npos ? codings : codings.substr(comma + 1));
    // We send no TE header, so chunked is the only coding a server may
    // apply; a body with any other final coding runs to close.
    if (last == "chunked") {
      return ReadChunkedBody(handler);
    }
    return ReadBodyUntilClose(handler);
  }
  std::optional<uint64_t> length;
  for (const auto& header : head.headers) {
    if (!EqualsIgnoreCase(header.name, "content-length")) {
      continue;
    }
    std::optional<uint64_t> value = ParseDecimal(header.value);
    if (!value.has_value() || (length.has_value() && *length != *value)) {
      return HttpResult::kProtocolError;
    }
    length = value;
  }
  if (length.has_value()) {
    return ReadFixedBody(*length, handler);
  }
  return ReadBodyUntilClose(handler);
}

HttpResult ResponseReader::ReadFixedBody(uint64_t length,
                                         HttpResponseHandler& handler) {
  uint8_t chunk[16 * 1024];
  while (length > 0) {
    size_t want = static_cast<size_t>(
        std::min<uint64_t>(length, static_cast<uint64_t>(sizeof(chunk))));
    ptrdiff_t n = ReadSome(chunk, want);
    if (n <= 0) {
      // Short of Content-Length: the connection dropped mid-body.
      return HttpResult::kTransportError;
    }
    if (!handler.OnBody(chunk, static_cast<size_t>(n))) {
      return HttpResult::kAborted;
    }
    length -= static_cast<uint64_t>(n);
  }
  return HttpResult::kOk;
}

HttpResult ResponseReader::ReadChunkedBody(HttpResponseHandler& handler) {
  while (true) {
    std::string line;
    size_t budget = kMaxChunkSizeLine;
    LineResult result = ReadLine(line, budget);
    if (result != LineResult::kOk) {
      return LineError(result);
    }
    std::string size_text = TrimWhitespace(line.substr(0, line.find(';')));
    if (size_text.empty() || size_text.size() > 15) {
      return HttpResult::kProtocolError;
    }
    uint64_t size = 0;
    for (char c : size_text) {
      int digit;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        return HttpResult::kProtocolError;
      }
      size = size * 16 + static_cast<uint64_t>(digit);
    }
    if (size == 0) {
      // Trailer fields, ignored, then the blank line that ends the message.
      size_t trailer_budget = kMaxTrailerBytes;
      while (true) {
        result = ReadLine(line, trailer_budget);
        if (result != LineResult::kOk) {
          return LineError(result);
        }
        if (line.empty()) {
          return HttpResult::kOk;
        }
      }
    }
    HttpResult body_result = ReadFixedBody(size, handler);
    if (body_result != HttpResult::kOk) {
      return body_result;
    }
    budget = 2;
    result = ReadLine(line, budget);
    if (result != LineResult::kOk) {
      return LineError(result);
    }
    if (!line.empty()) {
      return HttpResult::kProtocolError;
    }
  }
}

HttpResult ResponseReader::ReadBodyUntilClose(HttpResponseHandler& handler) {
  uint8_t chunk[16 * 1024];
  while (true) {
    ptrdiff_t n = ReadSome(chunk, sizeof(chunk));
    if (n < 0) {
      return HttpResult::kTransportError;
    }
    if (n == 0) {
      return HttpResult::kOk;
    }
    if (!handler.OnBody(chunk, static_cast<size_t>(n))) {
      return HttpResult::kAborted;
    }
  }
}

}  // namespace http_internal

}  // namespace shorebird
}  // namespace flutter
