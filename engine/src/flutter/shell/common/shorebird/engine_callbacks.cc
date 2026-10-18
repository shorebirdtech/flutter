// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/engine_callbacks.h"

#include <string>
#include <vector>

#include "flutter/shell/common/shorebird/http_client.h"
#include "openssl/err.h"
#include "openssl/nid.h"
#include "openssl/rsa.h"
#include "openssl/sha.h"

namespace flutter {
namespace shorebird {

namespace {

// Forwards a response to the updater's C callbacks.
class CallbackHandler : public HttpResponseHandler {
 public:
  explicit CallbackHandler(const ShorebirdHttpResponseCallbacks& callbacks)
      : callbacks_(callbacks) {}

  bool OnResponse(int status,
                  const std::vector<HttpHeader>& headers) override {
    std::vector<ShorebirdHttpHeader> c_headers;
    c_headers.reserve(headers.size());
    for (const auto& header : headers) {
      c_headers.push_back({header.name.c_str(), header.value.c_str()});
    }
    return callbacks_.on_response(callbacks_.context,
                                  static_cast<uint16_t>(status),
                                  c_headers.data(), c_headers.size());
  }

  bool OnBody(const uint8_t* data, size_t length) override {
    return callbacks_.on_body(callbacks_.context, data, length);
  }

 private:
  const ShorebirdHttpResponseCallbacks& callbacks_;
};

int32_t ToCode(HttpResult result) {
  switch (result) {
    case HttpResult::kOk:
      return SHOREBIRD_HTTP_OK;
    case HttpResult::kTransportError:
      return SHOREBIRD_HTTP_TRANSPORT_ERROR;
    case HttpResult::kTlsError:
      return SHOREBIRD_HTTP_TLS_ERROR;
    case HttpResult::kProtocolError:
      return SHOREBIRD_HTTP_PROTOCOL_ERROR;
    case HttpResult::kAborted:
      return SHOREBIRD_HTTP_ABORTED;
    case HttpResult::kInvalidRequest:
      return SHOREBIRD_HTTP_INVALID_REQUEST;
  }
  return SHOREBIRD_HTTP_TRANSPORT_ERROR;
}

}  // namespace

int32_t UpdaterHttpRequest(const ShorebirdHttpRequest* request,
                           const ShorebirdHttpResponseCallbacks* callbacks) {
  if (request == nullptr || callbacks == nullptr ||
      request->method == nullptr || request->url == nullptr ||
      callbacks->on_response == nullptr || callbacks->on_body == nullptr ||
      (request->header_count > 0 && request->headers == nullptr) ||
      (request->body_length > 0 && request->body == nullptr)) {
    return SHOREBIRD_HTTP_INVALID_REQUEST;
  }
  HttpRequest http_request;
  http_request.method = request->method;
  http_request.url = request->url;
  for (size_t i = 0; i < request->header_count; i++) {
    const ShorebirdHttpHeader& header = request->headers[i];
    if (header.name == nullptr || header.value == nullptr) {
      return SHOREBIRD_HTTP_INVALID_REQUEST;
    }
    http_request.headers.push_back({header.name, header.value});
  }
  if (request->body_length > 0) {
    http_request.body.assign(reinterpret_cast<const char*>(request->body),
                             request->body_length);
  }
  CallbackHandler handler(*callbacks);
  return ToCode(SendHttpRequest(http_request, handler));
}

bool UpdaterVerifySignature(const uint8_t* public_key,
                            uintptr_t public_key_length,
                            const uint8_t* message,
                            uintptr_t message_length,
                            const uint8_t* signature,
                            uintptr_t signature_length) {
  if ((public_key == nullptr && public_key_length > 0) ||
      (message == nullptr && message_length > 0) ||
      (signature == nullptr && signature_length > 0)) {
    return false;
  }
  // The same check the Rust updater made with ring's
  // RSA_PKCS1_2048_8192_SHA256: a PKCS#1 RSAPublicKey with a 2048 to 8192
  // bit modulus, and an RSASSA-PKCS1-v1_5 SHA-256 signature.
  bssl::UniquePtr<RSA> rsa(
      RSA_public_key_from_bytes(public_key, public_key_length));
  bool valid = false;
  if (rsa) {
    unsigned bits = RSA_bits(rsa.get());
    if (bits >= 2048 && bits <= 8192) {
      uint8_t digest[SHA256_DIGEST_LENGTH];
      SHA256(message, message_length, digest);
      valid = RSA_verify(NID_sha256, digest, sizeof(digest), signature,
                         signature_length, rsa.get()) == 1;
    }
  }
  ERR_clear_error();
  return valid;
}

const ShorebirdEngineCallbacks& UpdaterEngineCallbacks() {
  static const ShorebirdEngineCallbacks callbacks = {
      UpdaterHttpRequest,
      UpdaterVerifySignature,
  };
  return callbacks;
}

}  // namespace shorebird
}  // namespace flutter
