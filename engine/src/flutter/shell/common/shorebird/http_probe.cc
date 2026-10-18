// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Fetches each URL on the command line with the updater's HTTP client and
// prints one line per URL: the URL, the result, the status and the body's
// size and SHA-256. Used to build the TLS accept/reject table on each
// platform and compare it with dart:io (tool/http_probe.dart in the updater
// repo prints the same lines).

#include <cstdio>
#include <string>

#include "flutter/shell/common/shorebird/http_client.h"
#include "openssl/sha.h"

namespace {

using flutter::shorebird::HttpHeader;
using flutter::shorebird::HttpRequest;
using flutter::shorebird::HttpResponseHandler;
using flutter::shorebird::HttpResult;

class Hasher : public HttpResponseHandler {
 public:
  Hasher() { SHA256_Init(&sha_); }
  bool OnResponse(int status, const std::vector<HttpHeader>&) override {
    status_ = status;
    return true;
  }
  bool OnBody(const uint8_t* data, size_t length) override {
    SHA256_Update(&sha_, data, length);
    size_ += length;
    return true;
  }
  std::string Digest() {
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &sha_);
    std::string hex;
    char byte[3];
    for (uint8_t b : digest) {
      snprintf(byte, sizeof(byte), "%02x", b);
      hex += byte;
    }
    return hex;
  }
  int status_ = 0;
  size_t size_ = 0;

 private:
  SHA256_CTX sha_;
};

const char* Name(HttpResult result) {
  switch (result) {
    case HttpResult::kOk:
      return "ok";
    case HttpResult::kTransportError:
      return "transport";
    case HttpResult::kTlsError:
      return "tls";
    case HttpResult::kProtocolError:
      return "protocol";
    case HttpResult::kAborted:
      return "aborted";
    case HttpResult::kInvalidRequest:
      return "invalid";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    HttpRequest request;
    request.url = argv[i];
    Hasher hasher;
    HttpResult result = flutter::shorebird::SendHttpRequest(request, hasher);
    bool ok = result == HttpResult::kOk;
    printf("%s\t%s\t%d\t%zu\t%s\n", argv[i], ok ? "accept" : Name(result),
           hasher.status_, hasher.size_, ok ? hasher.Digest().c_str() : "-");
    fflush(stdout);
  }
  return 0;
}
