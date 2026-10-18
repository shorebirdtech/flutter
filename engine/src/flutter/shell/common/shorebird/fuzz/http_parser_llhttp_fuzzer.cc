// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Differential fuzzer: parses each input as an HTTP/1.1 response with both
// the updater's ResponseReader and llhttp (Node's parser). Where both accept
// the response, they must agree on the status, the headers and every body
// byte; a disagreement there means one of them frames messages differently,
// the class of bug behind response smuggling. Inputs only one of them
// accepts are expected (the two are strict in different places) and are not
// failures.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "flutter/shell/common/shorebird/http_parser.h"
#include "llhttp.h"

namespace {

using flutter::shorebird::HttpHeader;
using flutter::shorebird::HttpResponseHandler;
using flutter::shorebird::HttpResult;
using flutter::shorebird::http_internal::ByteSource;
using flutter::shorebird::http_internal::ResponseHead;
using flutter::shorebird::http_internal::ResponseReader;

struct Parsed {
  int status = 0;
  std::vector<HttpHeader> headers;
  std::string body;
};

class StringSource : public ByteSource {
 public:
  StringSource(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  ptrdiff_t Read(uint8_t* buffer, size_t length) override {
    size_t n = std::min(length, size_ - offset_);
    memcpy(buffer, data_ + offset_, n);
    offset_ += n;
    return static_cast<ptrdiff_t>(n);
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t offset_ = 0;
};

class BodyCollector : public HttpResponseHandler {
 public:
  explicit BodyCollector(std::string& body) : body_(body) {}
  bool OnResponse(int, const std::vector<HttpHeader>&) override { return true; }
  bool OnBody(const uint8_t* data, size_t length) override {
    body_.append(reinterpret_cast<const char*>(data), length);
    return true;
  }

 private:
  std::string& body_;
};

bool ParseWithReader(const uint8_t* data, size_t size, Parsed& out) {
  StringSource source(data, size);
  ResponseReader reader(source);
  ResponseHead head;
  if (reader.ReadHead(head) != HttpResult::kOk) {
    return false;
  }
  out.status = head.status;
  out.headers = head.headers;
  BodyCollector collector(out.body);
  return reader.ReadBody(head, collector) == HttpResult::kOk;
}

std::string TrimOws(const std::string& s) {
  size_t begin = s.find_first_not_of(" \t");
  if (begin == std::string::npos) {
    return "";
  }
  return s.substr(begin, s.find_last_not_of(" \t") - begin + 1);
}

bool IsFramingHeader(const std::string& name) {
  auto lower = [](std::string s) {
    for (char& c : s) {
      c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return s;
  };
  std::string n = lower(name);
  return n == "transfer-encoding" || n == "content-length";
}

// llhttp, driven the way the reader reads: interim 1xx responses are
// skipped, the first final response is the one compared, and a body with no
// framing runs to the end of input.
struct LlhttpState {
  Parsed parsed;
  std::string field;
  std::string value;
  bool in_value = false;
  // Trailers arrive through the same callbacks; the reader drops them.
  bool head_complete = false;
  // An interim (1xx) response declared a body.
  bool interim_with_framing = false;
  bool done = false;
};

LlhttpState* State(llhttp_t* parser) {
  return static_cast<LlhttpState*>(parser->data);
}

void FlushHeader(LlhttpState* state) {
  if (state->in_value || !state->field.empty()) {
    state->parsed.headers.push_back({state->field, state->value});
  }
  state->field.clear();
  state->value.clear();
  state->in_value = false;
}

int OnMessageBegin(llhttp_t* parser) {
  LlhttpState* state = State(parser);
  state->parsed = Parsed();
  state->field.clear();
  state->value.clear();
  state->in_value = false;
  state->head_complete = false;
  return 0;
}

int OnHeaderField(llhttp_t* parser, const char* at, size_t length) {
  LlhttpState* state = State(parser);
  if (state->head_complete) {
    return 0;
  }
  if (state->in_value) {
    FlushHeader(state);
  }
  state->field.append(at, length);
  return 0;
}

int OnHeaderValue(llhttp_t* parser, const char* at, size_t length) {
  LlhttpState* state = State(parser);
  if (state->head_complete) {
    return 0;
  }
  state->in_value = true;
  state->value.append(at, length);
  return 0;
}

int OnHeadersComplete(llhttp_t* parser) {
  LlhttpState* state = State(parser);
  FlushHeader(state);
  state->head_complete = true;
  state->parsed.status = parser->status_code;
  for (const HttpHeader& header : state->parsed.headers) {
    if (parser->status_code < 200 && IsFramingHeader(header.name)) {
      state->interim_with_framing = true;
    }
  }
  return 0;
}

int OnBody(llhttp_t* parser, const char* at, size_t length) {
  State(parser)->parsed.body.append(at, length);
  return 0;
}

int OnMessageComplete(llhttp_t* parser) {
  LlhttpState* state = State(parser);
  int status = parser->status_code;
  if (status >= 100 && status < 200 && status != 101) {
    return 0;  // Interim; the final response follows.
  }
  state->done = true;
  return HPE_PAUSED;
}

bool ParseWithLlhttp(const uint8_t* data,
                     size_t size,
                     Parsed& out,
                     bool& interim_with_framing) {
  llhttp_settings_t settings;
  llhttp_settings_init(&settings);
  settings.on_message_begin = OnMessageBegin;
  settings.on_header_field = OnHeaderField;
  settings.on_header_value = OnHeaderValue;
  settings.on_headers_complete = OnHeadersComplete;
  settings.on_body = OnBody;
  settings.on_message_complete = OnMessageComplete;
  llhttp_t parser;
  llhttp_init(&parser, HTTP_RESPONSE, &settings);
  LlhttpState state;
  parser.data = &state;
  llhttp_errno_t error =
      llhttp_execute(&parser, reinterpret_cast<const char*>(data), size);
  if (error == HPE_OK && !state.done) {
    error = llhttp_finish(&parser);
  }
  if (!state.done || (error != HPE_OK && error != HPE_PAUSED)) {
    return false;
  }
  out = state.parsed;
  interim_with_framing = state.interim_with_framing;
  return true;
}

// Where the two parsers are known to differ, and the reader is right.
bool KnownDivergence(const Parsed& llhttp, bool interim_with_framing) {
  // RFC 9112 6.3: a 1xx response ends at its blank line whatever its
  // headers say; llhttp reads a body for one with Content-Length.
  if (interim_with_framing) {
    return true;
  }
  for (const HttpHeader& header : llhttp.headers) {
    // RFC 9110 5.5: whitespace around a field value is not part of it, so
    // "chunked<TAB>" is chunked. llhttp only recognizes the coding (and
    // the length) without trailing whitespace, and reads to close.
    if (IsFramingHeader(header.name) && TrimOws(header.value) != header.value) {
      return true;
    }
  }
  return false;
}

[[noreturn]] void Disagree(const char* what,
                           const Parsed& reader,
                           const Parsed& llhttp) {
  fprintf(stderr,
          "ResponseReader and llhttp disagree on the %s: status %d vs %d, "
          "%zu vs %zu headers, %zu vs %zu body bytes\n",
          what, reader.status, llhttp.status, reader.headers.size(),
          llhttp.headers.size(), reader.body.size(), llhttp.body.size());
  auto dump = [](const char* who, const std::string& body) {
    fprintf(stderr, "%s body:", who);
    for (unsigned char c : body.substr(0, 64)) {
      fprintf(stderr, " %02x", c);
    }
    fprintf(stderr, "\n");
  };
  dump("reader", reader.body);
  dump("llhttp", llhttp.body);
  abort();
}

}  // namespace

// The reader accepts a bare LF as a line ending (RFC 9112 2.2 allows it);
// llhttp's lenient mode for that splits lines differently, so compare only
// inputs whose every LF follows a CR, with llhttp strict. The crash fuzzer
// still covers bare LFs.
bool HasBareLf(const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; i++) {
    if (data[i] == '\n' && (i == 0 || data[i - 1] != '\r')) {
      return true;
    }
  }
  return false;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (HasBareLf(data, size)) {
    return 0;
  }
  Parsed reader;
  Parsed llhttp;
  bool interim_with_framing = false;
  if (!ParseWithReader(data, size, reader) ||
      !ParseWithLlhttp(data, size, llhttp, interim_with_framing) ||
      KnownDivergence(llhttp, interim_with_framing)) {
    return 0;
  }
  if (reader.status != llhttp.status) {
    Disagree("status", reader, llhttp);
  }
  if (reader.headers.size() != llhttp.headers.size()) {
    Disagree("header count", reader, llhttp);
  }
  for (size_t i = 0; i < reader.headers.size(); i++) {
    if (reader.headers[i].name != llhttp.headers[i].name ||
        reader.headers[i].value != TrimOws(llhttp.headers[i].value)) {
      Disagree("headers", reader, llhttp);
    }
  }
  if (reader.body != llhttp.body) {
    Disagree("body", reader, llhttp);
  }
  return 0;
}
