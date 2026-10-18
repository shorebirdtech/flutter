// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// libFuzzer target for everything the HTTP client parses from the network:
// the response (status line, headers, Content-Length, chunked and
// read-to-close bodies, interim responses) and Location headers. Run under
// ASan and UBSan by run.sh. Beyond not crashing, it checks that a parsed
// response never yields more body than the bytes it came from.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "flutter/shell/common/shorebird/http_parser.h"

namespace {

using flutter::shorebird::HttpHeader;
using flutter::shorebird::HttpResponseHandler;
using flutter::shorebird::HttpResult;
using flutter::shorebird::http_internal::ByteSource;
using flutter::shorebird::http_internal::ParseUrl;
using flutter::shorebird::http_internal::ResolveRedirect;
using flutter::shorebird::http_internal::ResponseHead;
using flutter::shorebird::http_internal::ResponseReader;

// Hands out the input in reads of varying size, as a socket would, so
// boundaries land everywhere in the parser's buffer.
class FuzzSource : public ByteSource {
 public:
  FuzzSource(const uint8_t* data, size_t size, uint8_t seed)
      : data_(data), size_(size), seed_(seed) {}
  ptrdiff_t Read(uint8_t* buffer, size_t length) override {
    if (offset_ == size_) {
      return 0;
    }
    seed_ = static_cast<uint8_t>(seed_ * 33 + 7);
    size_t n = std::min<size_t>({length, size_ - offset_, 1u + seed_ % 97});
    memcpy(buffer, data_ + offset_, n);
    offset_ += n;
    return static_cast<ptrdiff_t>(n);
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t offset_ = 0;
  uint8_t seed_;
};

class CountingHandler : public HttpResponseHandler {
 public:
  bool OnResponse(int, const std::vector<HttpHeader>&) override { return true; }
  bool OnBody(const uint8_t*, size_t length) override {
    body_bytes += length;
    return true;
  }
  size_t body_bytes = 0;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  FuzzSource source(data + 1, size - 1, data[0]);
  ResponseReader reader(source);
  ResponseHead head;
  if (reader.ReadHead(head) == HttpResult::kOk) {
    if (head.status < 200 || head.status > 599) {
      abort();
    }
    CountingHandler handler;
    reader.ReadBody(head, handler);
    if (handler.body_bytes > size) {
      abort();
    }
    for (const HttpHeader& header : head.headers) {
      if (header.name.compare("location") == 0 ||
          header.name.compare("Location") == 0) {
        auto base = ParseUrl("https://api.shorebird.dev/api/v1/patches/check");
        auto next = ResolveRedirect(*base, header.value);
        if (next.has_value()) {
          ParseUrl(*next);
        }
      }
    }
  }
  std::string text(reinterpret_cast<const char*>(data), size);
  auto url = ParseUrl(text);
  if (url.has_value()) {
    if (url->target.empty() || url->target[0] != '/') {
      abort();
    }
    ResolveRedirect(*url, text);
  }
  return 0;
}
