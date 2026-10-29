// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/engine_callbacks.h"

#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "openssl/base64.h"
#include "openssl/bn.h"
#include "openssl/mem.h"
#include "openssl/nid.h"
#include "openssl/rsa.h"
#include "openssl/sha.h"

namespace flutter {
namespace shorebird {
namespace testing {

// --- Forwarding requests to the platform's client ---------------------------

namespace {

// What the updater's C callbacks saw.
struct Received {
  int responses = 0;
  uint16_t status = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::vector<std::string> errors;
  bool accept_response = true;
};

bool OnResponse(void* context,
                uint16_t status,
                const ShorebirdHttpHeader* headers,
                uintptr_t header_count) {
  auto* received = static_cast<Received*>(context);
  received->responses++;
  received->status = status;
  for (uintptr_t i = 0; i < header_count; i++) {
    received->headers.emplace_back(headers[i].name, headers[i].value);
  }
  return received->accept_response;
}

bool OnBody(void* context, const uint8_t* data, uintptr_t length) {
  static_cast<Received*>(context)->body.append(
      reinterpret_cast<const char*>(data), length);
  return true;
}

void OnError(void* context, const char* message) {
  static_cast<Received*>(context)->errors.push_back(message);
}

// What the fake client was asked for, and what it answers.
HttpRequest sent;
HttpResult fake_result = HttpResult::kOk;
std::string fake_error;

HttpResult FakeSend(const HttpRequest& request,
                    HttpResponseHandler& handler,
                    std::string* error) {
  sent = request;
  if (fake_result == HttpResult::kOk || fake_result == HttpResult::kAborted) {
    if (!handler.OnResponse(206, {{"Content-Length", "5"}})) {
      return HttpResult::kAborted;
    }
    handler.OnBody(reinterpret_cast<const uint8_t*>("he"), 2);
    handler.OnBody(reinterpret_cast<const uint8_t*>("llo"), 3);
    return HttpResult::kOk;
  }
  *error = fake_error;
  return fake_result;
}

}  // namespace

class ForwardHttpRequestTest : public ::testing::Test {
 protected:
  void SetUp() override {
    sent = {};
    fake_result = HttpResult::kOk;
    fake_error.clear();
    request_.method = "POST";
    request_.url = "https://example.com/api";
    request_.headers = headers_;
    request_.header_count = 2;
    request_.body = reinterpret_cast<const uint8_t*>("{}");
    request_.body_length = 2;
    request_.timeout_ms = 30000;
  }

  int32_t Forward() {
    return ForwardHttpRequest(&request_, &callbacks_, FakeSend);
  }

  ShorebirdHttpHeader headers_[2] = {{"Accept-Encoding", "identity"},
                                     {"Content-Type", "application/json"}};
  ShorebirdHttpRequest request_ = {};
  Received received_;
  ShorebirdHttpResponseCallbacks callbacks_ = {&received_, OnResponse, OnBody,
                                               OnError};
};

TEST_F(ForwardHttpRequestTest, PassesTheRequestThrough) {
  EXPECT_EQ(Forward(), SHOREBIRD_HTTP_OK);
  EXPECT_EQ(sent.method, "POST");
  EXPECT_EQ(sent.url, "https://example.com/api");
  ASSERT_EQ(sent.headers.size(), 2u);
  EXPECT_EQ(sent.headers[0].name, "Accept-Encoding");
  EXPECT_EQ(sent.headers[0].value, "identity");
  EXPECT_EQ(sent.headers[1].name, "Content-Type");
  EXPECT_EQ(sent.body, "{}");
  EXPECT_EQ(sent.timeout_ms, 30000u);
}

TEST_F(ForwardHttpRequestTest, DeliversTheResponse) {
  EXPECT_EQ(Forward(), SHOREBIRD_HTTP_OK);
  EXPECT_EQ(received_.responses, 1);
  EXPECT_EQ(received_.status, 206);
  ASSERT_EQ(received_.headers.size(), 1u);
  EXPECT_EQ(received_.headers[0].first, "Content-Length");
  EXPECT_EQ(received_.headers[0].second, "5");
  EXPECT_EQ(received_.body, "hello");
  EXPECT_TRUE(received_.errors.empty());
}

TEST_F(ForwardHttpRequestTest, ReportsEachFailureWithThePlatformsReason) {
  const std::pair<HttpResult, int32_t> cases[] = {
      {HttpResult::kTransportError, SHOREBIRD_HTTP_TRANSPORT_ERROR},
      {HttpResult::kTlsError, SHOREBIRD_HTTP_TLS_ERROR},
      {HttpResult::kProtocolError, SHOREBIRD_HTTP_PROTOCOL_ERROR},
      {HttpResult::kInvalidRequest, SHOREBIRD_HTTP_INVALID_REQUEST},
  };
  for (const auto& [result, code] : cases) {
    received_ = {};
    fake_result = result;
    fake_error = "The request timed out.";
    EXPECT_EQ(Forward(), code);
    EXPECT_EQ(received_.responses, 0);
    ASSERT_EQ(received_.errors.size(), 1u);
    EXPECT_EQ(received_.errors[0], "The request timed out.");
  }
}

TEST_F(ForwardHttpRequestTest, AbortIsNotAnError) {
  received_.accept_response = false;
  EXPECT_EQ(Forward(), SHOREBIRD_HTTP_ABORTED);
  EXPECT_TRUE(received_.errors.empty());
}

TEST_F(ForwardHttpRequestTest, ToleratesNoErrorCallbackOrMessage) {
  fake_result = HttpResult::kTransportError;
  EXPECT_EQ(Forward(), SHOREBIRD_HTTP_TRANSPORT_ERROR);
  EXPECT_TRUE(received_.errors.empty());

  callbacks_.on_error = nullptr;
  fake_error = "offline";
  EXPECT_EQ(Forward(), SHOREBIRD_HTTP_TRANSPORT_ERROR);
}

TEST_F(ForwardHttpRequestTest, RejectsMalformedRequests) {
  EXPECT_EQ(ForwardHttpRequest(nullptr, &callbacks_, FakeSend),
            SHOREBIRD_HTTP_INVALID_REQUEST);
  EXPECT_EQ(ForwardHttpRequest(&request_, nullptr, FakeSend),
            SHOREBIRD_HTTP_INVALID_REQUEST);

  ShorebirdHttpRequest no_url = request_;
  no_url.url = nullptr;
  EXPECT_EQ(ForwardHttpRequest(&no_url, &callbacks_, FakeSend),
            SHOREBIRD_HTTP_INVALID_REQUEST);

  ShorebirdHttpRequest no_body = request_;
  no_body.body = nullptr;
  EXPECT_EQ(ForwardHttpRequest(&no_body, &callbacks_, FakeSend),
            SHOREBIRD_HTTP_INVALID_REQUEST);

  ShorebirdHttpHeader null_value[] = {{"Accept", nullptr}};
  ShorebirdHttpRequest bad_header = request_;
  bad_header.headers = null_value;
  bad_header.header_count = 1;
  EXPECT_EQ(ForwardHttpRequest(&bad_header, &callbacks_, FakeSend),
            SHOREBIRD_HTTP_INVALID_REQUEST);

  ShorebirdHttpResponseCallbacks no_body_callback = callbacks_;
  no_body_callback.on_body = nullptr;
  EXPECT_EQ(ForwardHttpRequest(&request_, &no_body_callback, FakeSend),
            SHOREBIRD_HTTP_INVALID_REQUEST);

  EXPECT_EQ(received_.responses, 0);
  EXPECT_TRUE(received_.errors.empty());
}

TEST(UpdaterEngineCallbacksTest, AlwaysVerifiesSignatures) {
  // Without it, shorebird_init fails for every app with a patch public key.
  EXPECT_EQ(UpdaterEngineCallbacks().verify_signature, UpdaterVerifySignature);
}

// --- Signature verification -------------------------------------------------

// The vectors from the Rust updater's signing tests: an RSA-2048 key (DER
// PKCS#1, base64), a message, and its RSASSA-PKCS1-v1_5 SHA-256 signature.
constexpr char kPublicKey[] =
    "MIIBCgKCAQEA2wdpEGbuvlPsb9i0qYrfMefJnEw1BHTi8SYZTKrXOvJWmEpPE1hWfbkvYzXu5"
    "a96gV1yocF3DMwn04VmRlKhC4AhsD0NL0UNhYhotbKG91Kwi1vAXpHhCdz5gQEBw0K1uB4Jz+"
    "zK6WK+31PryYpwLwbyXNqXoY8IAAUQ4STsHYV5w+BMSi8pepWMRd7DR9RHcbNOZlJvdBQ5Nxv"
    "B4JN4dRMq8cC73ez1P9d7Dfwv3TWY+he9EmuXLT2UivZSlHIrGBa7MFfqyUe2ro0F7Te/B0si"
    "12itBbWIqycvqcXjeOPNn6WEpqN7IWjb9LUh162JyYaz5Lb/VeeJX8LKtElccwIDAQAB";
constexpr char kMessage[] =
    "404e5caa5b906f6d03c97657e8c4d604d759f9cfba1a8bba9d5b49a5ebc174f9";
constexpr char kSignature[] =
    "2ixSo5LpaWUSLg2GJEV+D+uyLeLjp0c3vNXnl0yb1iJjAdpn10BFlbcwCcjaJW9PNky2HU2hK"
    "OBe62PkFHOU8DDYOfxf2LGg/ToLGPHin85WrwFAceAUYDs7JpQr43dRTbrXcT8k5tuCQOTwXe"
    "cGwuWcOFFvh0GbXFnyAmi7fLfN9CtTsG2GIOle/LyYLwoviTrXn/fZTZEYrqxD/wZ4QzoWOWL"
    "WNvrPbILhqWELkBLhdZeK0+nC2CIxFRYd3bUeOi1AGtPyHKBfdwuf4VO3+HbwJVaAEiD7HU2B"
    "j+Zp1xeSdbznmYgBV86oizrLFd23D+lBfTlmDGgdfNE9J4Z2/g==";

std::vector<uint8_t> Base64Decode(const char* text) {
  size_t length = 0;
  EXPECT_TRUE(EVP_DecodedLength(&length, std::strlen(text)));
  std::vector<uint8_t> out(length);
  EXPECT_TRUE(EVP_DecodeBase64(out.data(), &length, out.size(),
                               reinterpret_cast<const uint8_t*>(text),
                               std::strlen(text)));
  out.resize(length);
  return out;
}

bool Verify(const std::vector<uint8_t>& key,
            const std::string& message,
            const std::vector<uint8_t>& signature) {
  return UpdaterVerifySignature(
      key.data(), key.size(), reinterpret_cast<const uint8_t*>(message.data()),
      message.size(), signature.data(), signature.size());
}

TEST(UpdaterVerifySignatureTest, AcceptsValidSignature) {
  EXPECT_TRUE(
      Verify(Base64Decode(kPublicKey), kMessage, Base64Decode(kSignature)));
}

TEST(UpdaterVerifySignatureTest, RejectsWrongMessageOrSignature) {
  auto key = Base64Decode(kPublicKey);
  auto signature = Base64Decode(kSignature);
  EXPECT_FALSE(Verify(key, std::string(kMessage) + "x", signature));
  signature[10] ^= 1;
  EXPECT_FALSE(Verify(key, kMessage, signature));
  EXPECT_FALSE(Verify(key, kMessage, {}));
  // As the Rust test does: the key bytes are not a signature.
  EXPECT_FALSE(Verify(key, kMessage, key));
}

TEST(UpdaterVerifySignatureTest, RejectsBadOrSmallKeys) {
  auto signature = Base64Decode(kSignature);
  EXPECT_FALSE(Verify({}, kMessage, signature));
  EXPECT_FALSE(Verify({1, 2, 3}, kMessage, signature));
  EXPECT_FALSE(UpdaterVerifySignature(nullptr, 5, nullptr, 0, nullptr, 0));

  // A valid 1024-bit key is below the 2048-bit floor.
  bssl::UniquePtr<RSA> small(RSA_new());
  bssl::UniquePtr<BIGNUM> e(BN_new());
  BN_set_word(e.get(), RSA_F4);
  ASSERT_TRUE(RSA_generate_key_ex(small.get(), 1024, e.get(), nullptr));
  uint8_t* der = nullptr;
  size_t der_length = 0;
  ASSERT_TRUE(RSA_public_key_to_bytes(&der, &der_length, small.get()));
  std::vector<uint8_t> small_key(der, der + der_length);
  OPENSSL_free(der);
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(kMessage), std::strlen(kMessage),
         digest);
  std::vector<uint8_t> small_signature(RSA_size(small.get()));
  unsigned signature_length = 0;
  ASSERT_TRUE(RSA_sign(NID_sha256, digest, sizeof(digest),
                       small_signature.data(), &signature_length, small.get()));
  small_signature.resize(signature_length);
  EXPECT_FALSE(Verify(small_key, kMessage, small_signature));
}

}  // namespace testing
}  // namespace shorebird
}  // namespace flutter
