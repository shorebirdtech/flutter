// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Fetches each URL on the command line through the HTTP callback the engine
// gives the updater, and prints one line per URL: the URL, the result, the
// status, the body's size and SHA-256, and the platform's error, if any.
// Used to build the TLS accept/reject table on each platform and compare it
// with dart:io (http_probe.dart prints the first three columns the same
// way). A plain executable runs on macOS and the iOS simulator, not on an
// iOS device or with Android's client, which needs the app's Java VM.

#include <cstdio>
#include <string>

#include "flutter/shell/common/shorebird/engine_callbacks.h"
#include "openssl/sha.h"

namespace {

struct Probe {
  SHA256_CTX sha;
  int status = 0;
  size_t size = 0;
  std::string error;

  std::string Digest() {
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &sha);
    std::string hex;
    char byte[3];
    for (uint8_t b : digest) {
      snprintf(byte, sizeof(byte), "%02x", b);
      hex += byte;
    }
    return hex;
  }
};

bool OnResponse(void* context,
                uint16_t status,
                const ShorebirdHttpHeader*,
                uintptr_t) {
  static_cast<Probe*>(context)->status = status;
  return true;
}

bool OnBody(void* context, const uint8_t* data, uintptr_t length) {
  auto* probe = static_cast<Probe*>(context);
  SHA256_Update(&probe->sha, data, length);
  probe->size += length;
  return true;
}

void OnError(void* context, const char* message) {
  static_cast<Probe*>(context)->error = message;
}

const char* Name(int32_t code) {
  switch (code) {
    case SHOREBIRD_HTTP_OK:
      return "accept";
    case SHOREBIRD_HTTP_TRANSPORT_ERROR:
      return "transport";
    case SHOREBIRD_HTTP_TLS_ERROR:
      return "tls";
    case SHOREBIRD_HTTP_PROTOCOL_ERROR:
      return "protocol";
    case SHOREBIRD_HTTP_ABORTED:
      return "aborted";
    case SHOREBIRD_HTTP_INVALID_REQUEST:
      return "invalid";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  auto http_request = flutter::shorebird::UpdaterEngineCallbacks().http_request;
  if (http_request == nullptr) {
    fprintf(stderr, "The engine has no HTTP client on this platform.\n");
    return 1;
  }
  const ShorebirdHttpHeader headers[] = {
      {"User-Agent", "Shorebird-Updater"},
      {"Accept-Encoding", "identity"},
  };
  for (int i = 1; i < argc; i++) {
    ShorebirdHttpRequest request = {};
    request.method = "GET";
    request.url = argv[i];
    request.headers = headers;
    request.header_count = sizeof(headers) / sizeof(headers[0]);
    request.timeout_ms = 60000;
    Probe probe;
    SHA256_Init(&probe.sha);
    ShorebirdHttpResponseCallbacks callbacks = {&probe, OnResponse, OnBody,
                                                OnError};
    int32_t code = http_request(&request, &callbacks);
    bool ok = code == SHOREBIRD_HTTP_OK;
    printf("%s\t%s\t%d\t%zu\t%s\t%s\n", argv[i], Name(code), probe.status,
           probe.size, ok ? probe.Digest().c_str() : "-", probe.error.c_str());
    fflush(stdout);
  }
  return 0;
}
