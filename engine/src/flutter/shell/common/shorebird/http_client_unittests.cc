// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/http_client.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "flutter/shell/common/shorebird/engine_callbacks.h"
#include "flutter/shell/common/shorebird/http_parser.h"
#include "third_party/updater/library/include/updater_dart.h"
#include "gtest/gtest.h"
#include "openssl/base64.h"
#include "openssl/bio.h"
#include "openssl/bn.h"
#include "openssl/ec_key.h"
#include "openssl/err.h"
#include "openssl/evp.h"
#include "openssl/nid.h"
#include "openssl/pem.h"
#include "openssl/rsa.h"
#include "openssl/sha.h"
#include "openssl/ssl.h"
#include "openssl/x509.h"
#include "openssl/x509v3.h"

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace flutter {
namespace shorebird {
namespace testing {

using http_internal::ByteSource;
using http_internal::ParseUrl;
using http_internal::ResolveRedirect;
using http_internal::ResponseHead;
using http_internal::ResponseReader;
using http_internal::Url;

// Records what a request delivered.
class RecordingHandler : public HttpResponseHandler {
 public:
  bool OnResponse(int status,
                  const std::vector<HttpHeader>& headers) override {
    response_count++;
    this->status = status;
    this->headers = headers;
    return !abort_on_response;
  }

  bool OnBody(const uint8_t* data, size_t length) override {
    body.append(reinterpret_cast<const char*>(data), length);
    return !(abort_after_bytes > 0 && body.size() >= abort_after_bytes);
  }

  std::string Header(const std::string& name) const {
    for (const auto& header : headers) {
      if (header.name == name) {
        return header.value;
      }
    }
    return "";
  }

  int response_count = 0;
  int status = 0;
  std::vector<HttpHeader> headers;
  std::string body;
  bool abort_on_response = false;
  size_t abort_after_bytes = 0;
};

// --- URLs -------------------------------------------------------------

TEST(HttpClientUrlTest, ParsesHttpsWithDefaultPort) {
  auto url = ParseUrl("https://api.shorebird.dev/api/v1/patches/check");
  ASSERT_TRUE(url.has_value());
  EXPECT_TRUE(url->https);
  EXPECT_EQ(url->host, "api.shorebird.dev");
  EXPECT_EQ(url->port, 443);
  EXPECT_EQ(url->target, "/api/v1/patches/check");
  EXPECT_FALSE(url->host_is_ip_literal);
}

TEST(HttpClientUrlTest, ParsesPortQueryAndDropsFragment) {
  auto url = ParseUrl("HTTP://127.0.0.1:8080/a?b=c#frag");
  ASSERT_TRUE(url.has_value());
  EXPECT_FALSE(url->https);
  EXPECT_EQ(url->host, "127.0.0.1");
  EXPECT_EQ(url->port, 8080);
  EXPECT_EQ(url->target, "/a?b=c");
  EXPECT_TRUE(url->host_is_ip_literal);
}

TEST(HttpClientUrlTest, DefaultsTargetToSlash) {
  EXPECT_EQ(ParseUrl("https://example.com")->target, "/");
  EXPECT_EQ(ParseUrl("https://example.com?x=1")->target, "/?x=1");
}

TEST(HttpClientUrlTest, ParsesIpv6Literal) {
  auto url = ParseUrl("http://[::1]:9000/x");
  ASSERT_TRUE(url.has_value());
  EXPECT_EQ(url->host, "::1");
  EXPECT_EQ(url->port, 9000);
  EXPECT_TRUE(url->host_is_ip_literal);
}

TEST(HttpClientUrlTest, RejectsBadUrls) {
  for (const char* bad : {
           "",
           "example.com/x",
           "ftp://example.com/",
           "https://",
           "https:///path",
           "https://user:pass@example.com/",
           "https://example.com:/",
           "https://example.com:0/",
           "https://example.com:65536/",
           "https://example.com:12a/",
           "https://exa mple.com/",
           "https://example.com/a b",
           "https://example.com/\r\nInjected: yes",
           "https://[::1/",
           "https://[not-ip]/",
           "https://[::1]x/",
           "https://ex%61mple.com/",
       }) {
    EXPECT_FALSE(ParseUrl(bad).has_value()) << bad;
  }
}

TEST(HttpClientUrlTest, ResolvesRedirects) {
  Url base = *ParseUrl("https://api.example.com:8443/dir/file?q=1");
  EXPECT_EQ(*ResolveRedirect(base, "https://cdn.example.com/p"),
            "https://cdn.example.com/p");
  EXPECT_EQ(*ResolveRedirect(base, "//cdn.example.com/p"),
            "https://cdn.example.com/p");
  EXPECT_EQ(*ResolveRedirect(base, "/other"),
            "https://api.example.com:8443/other");
  EXPECT_EQ(*ResolveRedirect(base, "sibling"),
            "https://api.example.com:8443/dir/sibling");
  EXPECT_EQ(*ResolveRedirect(base, "?r=2"),
            "https://api.example.com:8443/dir/file?r=2");
  EXPECT_FALSE(ResolveRedirect(base, "").has_value());
}

// --- Response parsing -----------------------------------------------------

// Serves `data` in pieces of at most `piece` bytes, then end of stream (or
// a transport error, if `fail_at_end`).
class StringSource : public ByteSource {
 public:
  explicit StringSource(std::string data, size_t piece = 1 << 20)
      : data_(std::move(data)), piece_(piece) {}

  ptrdiff_t Read(uint8_t* buffer, size_t length) override {
    if (offset_ == data_.size()) {
      return fail_at_end ? -1 : 0;
    }
    size_t n = std::min({length, piece_, data_.size() - offset_});
    std::memcpy(buffer, data_.data() + offset_, n);
    offset_ += n;
    return static_cast<ptrdiff_t>(n);
  }

  bool fail_at_end = false;

 private:
  std::string data_;
  size_t piece_;
  size_t offset_ = 0;
};

HttpResult Parse(ByteSource& source, RecordingHandler& handler) {
  ResponseReader reader(source);
  ResponseHead head;
  HttpResult result = reader.ReadHead(head);
  if (result != HttpResult::kOk) {
    return result;
  }
  if (!handler.OnResponse(head.status, head.headers)) {
    return HttpResult::kAborted;
  }
  return reader.ReadBody(head, handler);
}

HttpResult Parse(const std::string& response,
                 RecordingHandler& handler,
                 size_t piece = 1 << 20) {
  StringSource source(response, piece);
  return Parse(source, handler);
}

TEST(HttpClientParseTest, ContentLengthBody) {
  for (size_t piece : {1, 3, 1000}) {
    RecordingHandler handler;
    EXPECT_EQ(Parse("HTTP/1.1 200 OK\r\n"
                    "Content-Length: 5\r\n"
                    "X-Thing:   spaced value  \r\n"
                    "\r\n"
                    "helloEXTRA",
                    handler, piece),
              HttpResult::kOk);
    EXPECT_EQ(handler.status, 200);
    EXPECT_EQ(handler.body, "hello");
    EXPECT_EQ(handler.Header("X-Thing"), "spaced value");
  }
}

TEST(HttpClientParseTest, ChunkedBodyWithExtensionsAndTrailers) {
  for (size_t piece : {1, 7, 1000}) {
    RecordingHandler handler;
    EXPECT_EQ(Parse("HTTP/1.1 200 OK\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "\r\n"
                    "5;ext=1\r\nhello\r\n"
                    "B\r\n, world!!!!\r\n"
                    "0\r\n"
                    "Trailer: x\r\n"
                    "\r\n",
                    handler, piece),
              HttpResult::kOk);
    EXPECT_EQ(handler.body, "hello, world!!!!");
  }
}

TEST(HttpClientParseTest, ChunkedWinsOverContentLength) {
  RecordingHandler handler;
  EXPECT_EQ(Parse("HTTP/1.1 200 OK\r\n"
                  "Content-Length: 100\r\n"
                  "Transfer-Encoding: gzip, chunked\r\n"
                  "\r\n"
                  "2\r\nhi\r\n0\r\n\r\n",
                  handler),
            HttpResult::kOk);
  EXPECT_EQ(handler.body, "hi");
}

TEST(HttpClientParseTest, RepeatedTransferEncodingFieldsCombine) {
  // "chunked" then "gzip" is the list "chunked, gzip": the last coding is
  // not chunked, so the body runs to close (found by the llhttp
  // differential fuzzer).
  RecordingHandler handler;
  EXPECT_EQ(Parse("HTTP/1.1 200 OK\r\n"
                  "Transfer-Encoding: chunked\r\n"
                  "Transfer-Encoding: gzip\r\n"
                  "\r\n"
                  "2\r\nhi\r\n0\r\n\r\n",
                  handler),
            HttpResult::kOk);
  EXPECT_EQ(handler.body, "2\r\nhi\r\n0\r\n\r\n");
}

TEST(HttpClientParseTest, BodyUntilClose) {
  RecordingHandler handler;
  EXPECT_EQ(Parse("HTTP/1.0 200 OK\r\n\r\nall of it", handler),
            HttpResult::kOk);
  EXPECT_EQ(handler.body, "all of it");
}

TEST(HttpClientParseTest, AcceptsBareLineFeeds) {
  RecordingHandler handler;
  EXPECT_EQ(Parse("HTTP/1.1 404 Not Found\nContent-Length: 2\n\nno", handler),
            HttpResult::kOk);
  EXPECT_EQ(handler.status, 404);
  EXPECT_EQ(handler.body, "no");
}

TEST(HttpClientParseTest, SkipsInterimResponses) {
  RecordingHandler handler;
  EXPECT_EQ(Parse("HTTP/1.1 100 Continue\r\n\r\n"
                  "HTTP/1.1 103 Early Hints\r\nLink: x\r\n\r\n"
                  "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n",
                  handler),
            HttpResult::kOk);
  EXPECT_EQ(handler.status, 201);
  EXPECT_EQ(handler.Header("Link"), "");
  EXPECT_EQ(handler.response_count, 1);
}

TEST(HttpClientParseTest, NoBodyFor204And304) {
  for (const char* status : {"204 No Content", "304 Not Modified"}) {
    RecordingHandler handler;
    EXPECT_EQ(Parse(std::string("HTTP/1.1 ") + status +
                        "\r\n\r\nshould not be read",
                    handler),
              HttpResult::kOk);
    EXPECT_EQ(handler.body, "");
  }
}

TEST(HttpClientParseTest, MalformedResponsesAreProtocolErrors) {
  for (const char* bad : {
           "HTTP/2 200 OK\r\n\r\n",
           "HTTP/1.1 2000 OK\r\n\r\n",
           "HTTP/1.1 20 OK\r\n\r\n",
           "HTTP/1.1 600 Nope\r\n\r\n",
           "HTTP/1.1 099 Nope\r\n\r\n",
           "HTTP/1.1 200OK\r\n\r\n",
           "ICY 200 OK\r\n\r\n",
           "HTTP/1.1 101 Switching Protocols\r\n\r\n",
           "HTTP/1.1 200 OK\r\nNo colon here\r\n\r\n",
           "HTTP/1.1 200 OK\r\n: empty name\r\n\r\n",
           "HTTP/1.1 200 OK\r\nBad Name: x\r\n\r\n",
           "HTTP/1.1 200 OK\r\nA: b\r\n folded\r\n\r\n",
           "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
           "HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n\r\n",
           "HTTP/1.1 200 OK\r\nContent-Length: 1, 2\r\n\r\n",
           "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n",
           "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
           "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n",
           "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\n",
           "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
           "1000000000000000\r\n",
           "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
           "2\r\nhiXX\r\n0\r\n\r\n",
       }) {
    RecordingHandler handler;
    EXPECT_EQ(Parse(bad, handler), HttpResult::kProtocolError) << bad;
  }
}

TEST(HttpClientParseTest, ControlCharactersInHeaderValuesAreRejected) {
  RecordingHandler handler;
  std::string response = "HTTP/1.1 200 OK\r\nA: b";
  response.push_back('\0');
  response += "c\r\n\r\n";
  EXPECT_EQ(Parse(response, handler), HttpResult::kProtocolError);
}

TEST(HttpClientParseTest, OversizedHeadIsRejected) {
  RecordingHandler handler;
  std::string response = "HTTP/1.1 200 OK\r\nBig: " +
                         std::string(70 * 1024, 'a') + "\r\n\r\n";
  EXPECT_EQ(Parse(response, handler), HttpResult::kProtocolError);
}

TEST(HttpClientParseTest, TooManyHeadersAreRejected) {
  RecordingHandler handler;
  std::string response = "HTTP/1.1 200 OK\r\n";
  for (int i = 0; i < 200; i++) {
    response += "H" + std::to_string(i) + ": v\r\n";
  }
  response += "\r\n";
  EXPECT_EQ(Parse(response, handler), HttpResult::kProtocolError);
}

TEST(HttpClientParseTest, EndlessInterimResponsesAreRejected) {
  RecordingHandler handler;
  std::string response;
  for (int i = 0; i < 100; i++) {
    response += "HTTP/1.1 100 Continue\r\n\r\n";
  }
  EXPECT_EQ(Parse(response, handler), HttpResult::kProtocolError);
}

TEST(HttpClientParseTest, TruncatedResponsesAreTransportErrors) {
  for (const char* truncated : {
           "",
           "HTTP/1.1 200 OK\r\n",
           "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort",
           "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel",
           "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n",
       }) {
    RecordingHandler handler;
    EXPECT_EQ(Parse(truncated, handler), HttpResult::kTransportError)
        << truncated;
  }
}

TEST(HttpClientParseTest, ReadErrorMidBodyIsATransportError) {
  StringSource source("HTTP/1.1 200 OK\r\n\r\npartial");
  source.fail_at_end = true;
  RecordingHandler handler;
  EXPECT_EQ(Parse(source, handler), HttpResult::kTransportError);
  EXPECT_EQ(handler.body, "partial");
}

TEST(HttpClientParseTest, HandlerCanAbort) {
  RecordingHandler on_response;
  on_response.abort_on_response = true;
  EXPECT_EQ(Parse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi",
                  on_response),
            HttpResult::kAborted);

  RecordingHandler on_body;
  on_body.abort_after_bytes = 1;
  EXPECT_EQ(Parse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", on_body,
                  /*piece=*/1),
            HttpResult::kAborted);
  EXPECT_EQ(on_body.body, "h");
}

// The parser sees whatever a network hands it, so arbitrary bytes must
// never crash it, hang it, or deliver more than they contain. Runs under
// ASan in sanitizer builds.
TEST(HttpClientParseTest, SurvivesMutatedResponses) {
  const std::vector<std::string> seeds = {
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
      "HTTP/1.1 206 Partial\r\nContent-Range: bytes 0-1/2\r\n"
      "Transfer-Encoding: chunked\r\n\r\n2;x\r\nhi\r\n0\r\nT: v\r\n\r\n",
      "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.0 302 Found\r\nLocation: /x\r\n\r\n",
  };
  std::mt19937 random(12345);
  for (int i = 0; i < 20000; i++) {
    std::string input = seeds[random() % seeds.size()];
    int mutations = 1 + random() % 8;
    for (int m = 0; m < mutations && !input.empty(); m++) {
      size_t at = random() % input.size();
      switch (random() % 4) {
        case 0:
          input[at] = static_cast<char>(random());
          break;
        case 1:
          input.erase(at, 1 + random() % 4);
          break;
        case 2:
          input.insert(at, 1 + random() % 4, static_cast<char>(random()));
          break;
        case 3:
          input.insert(at, input.substr(0, random() % input.size()));
          break;
      }
    }
    RecordingHandler handler;
    Parse(input, handler, 1 + random() % 16);
    EXPECT_LE(handler.body.size(), input.size());
  }
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
  EXPECT_TRUE(Verify(Base64Decode(kPublicKey), kMessage,
                     Base64Decode(kSignature)));
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
                       small_signature.data(), &signature_length,
                       small.get()));
  small_signature.resize(signature_length);
  EXPECT_FALSE(Verify(small_key, kMessage, small_signature));
}

#if !defined(_WIN32)

// --- Live requests over loopback --------------------------------------------

// A self-signed certificate and key for a test server.
struct TestCertificate {
  bssl::UniquePtr<EVP_PKEY> key;
  bssl::UniquePtr<X509> cert;
  std::string pem;
};

TestCertificate MakeCertificate(const char* dns_name) {
  TestCertificate result;
  bssl::UniquePtr<EC_KEY> ec(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
  EC_KEY_generate_key(ec.get());
  result.key.reset(EVP_PKEY_new());
  EVP_PKEY_assign_EC_KEY(result.key.get(), ec.release());

  result.cert.reset(X509_new());
  X509* cert = result.cert.get();
  X509_set_version(cert, X509_VERSION_3);
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
  X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const uint8_t*>(dns_name), -1,
                             -1, 0);
  X509_set_issuer_name(cert, name);
  X509_set_pubkey(cert, result.key.get());
  std::string san = std::string("DNS:") + dns_name + ",IP:127.0.0.1";
  X509V3_CTX context;
  X509V3_set_ctx(&context, cert, cert, nullptr, nullptr, 0);
  bssl::UniquePtr<X509_EXTENSION> san_extension(X509V3_EXT_nconf_nid(
      nullptr, &context, NID_subject_alt_name, san.c_str()));
  X509_add_ext(cert, san_extension.get(), -1);
  bssl::UniquePtr<X509_EXTENSION> ca_extension(X509V3_EXT_nconf_nid(
      nullptr, &context, NID_basic_constraints, "critical,CA:TRUE"));
  X509_add_ext(cert, ca_extension.get(), -1);
  X509_sign(cert, result.key.get(), EVP_sha256());

  bssl::UniquePtr<BIO> bio(BIO_new(BIO_s_mem()));
  PEM_write_bio_X509(bio.get(), cert);
  const uint8_t* data;
  size_t length;
  BIO_mem_contents(bio.get(), &data, &length);
  result.pem.assign(reinterpret_cast<const char*>(data), length);
  return result;
}

// One scripted exchange: the server reads a request, records it, and
// writes `response` (unless `respond` is false, in which case it holds the
// connection open until the client gives up).
struct Exchange {
  std::string response;
  bool respond = true;
};

// A loopback server that serves `exchanges` in order, one per connection.
class TestServer {
 public:
  TestServer(std::vector<Exchange> exchanges,
             const TestCertificate* certificate = nullptr)
      : exchanges_(std::move(exchanges)), certificate_(certificate) {
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    listen(listener_, 8);
    if (certificate_ != nullptr) {
      context_.reset(SSL_CTX_new(TLS_server_method()));
      SSL_CTX_use_certificate(context_.get(), certificate_->cert.get());
      SSL_CTX_use_PrivateKey(context_.get(), certificate_->key.get());
    }
    thread_ = std::thread([this] { Serve(); });
  }

  ~TestServer() {
    stop_ = true;
    shutdown(listener_, SHUT_RDWR);
    close(listener_);
    thread_.join();
  }

  std::string Url(const std::string& path, const char* host = "127.0.0.1") {
    return std::string(certificate_ ? "https" : "http") + "://" + host + ":" +
           std::to_string(port_) + path;
  }

  const std::vector<std::string>& requests() const { return requests_; }

 private:
  void Serve() {
    for (const Exchange& exchange : exchanges_) {
      int connection = accept(listener_, nullptr, nullptr);
      if (connection < 0) {
        return;
      }
      bssl::UniquePtr<SSL> ssl;
      if (context_) {
        ssl.reset(SSL_new(context_.get()));
        SSL_set_fd(ssl.get(), connection);
        if (SSL_accept(ssl.get()) != 1) {
          ERR_clear_error();
          close(connection);
          continue;
        }
      }
      auto read = [&](char* buffer, int size) {
        return ssl ? SSL_read(ssl.get(), buffer, size)
                   : static_cast<int>(recv(connection, buffer, size, 0));
      };
      auto write = [&](const std::string& data) {
        if (ssl) {
          SSL_write(ssl.get(), data.data(), static_cast<int>(data.size()));
        } else {
          send(connection, data.data(), data.size(), 0);
        }
      };
      requests_.push_back(ReadRequest(read));
      if (exchange.respond) {
        write(exchange.response);
      } else {
        // Hold the connection until the client hangs up.
        char buffer[64];
        while (!stop_ && read(buffer, sizeof(buffer)) > 0) {
        }
      }
      if (ssl) {
        SSL_shutdown(ssl.get());
      }
      close(connection);
    }
  }

  static std::string ReadRequest(const std::function<int(char*, int)>& read) {
    std::string request;
    char buffer[4096];
    size_t head_end;
    while ((head_end = request.find("\r\n\r\n")) == std::string::npos) {
      int n = read(buffer, sizeof(buffer));
      if (n <= 0) {
        return request;
      }
      request.append(buffer, n);
    }
    size_t length_at = request.find("Content-Length: ");
    if (length_at != std::string::npos && length_at < head_end) {
      size_t length = std::stoul(request.substr(length_at + 16));
      while (request.size() < head_end + 4 + length) {
        int n = read(buffer, sizeof(buffer));
        if (n <= 0) {
          break;
        }
        request.append(buffer, n);
      }
    }
    return request;
  }

  std::vector<Exchange> exchanges_;
  const TestCertificate* certificate_;
  bssl::UniquePtr<SSL_CTX> context_;
  int listener_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::vector<std::string> requests_;
  std::thread thread_;
};

HttpResult Get(const std::string& url,
               RecordingHandler& handler,
               const HttpClientOptions& options = {},
               std::vector<HttpHeader> headers = {}) {
  HttpRequest request;
  request.url = url;
  request.headers = std::move(headers);
  return SendHttpRequest(request, handler, options);
}

TEST(HttpClientLiveTest, GetsAResponse) {
  RecordingHandler handler;
  std::vector<std::string> requests;
  {
    TestServer server({{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"}});
    EXPECT_EQ(Get(server.Url("/p?q=1"), handler), HttpResult::kOk);
    requests = server.requests();
  }
  EXPECT_EQ(handler.status, 200);
  EXPECT_EQ(handler.body, "ok");
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].rfind("GET /p?q=1 HTTP/1.1\r\n", 0), 0u)
      << requests[0];
  EXPECT_NE(requests[0].find("\r\nHost: 127.0.0.1:"), std::string::npos);
  EXPECT_NE(requests[0].find("\r\nUser-Agent: Shorebird-Updater\r\n"),
            std::string::npos);
  EXPECT_NE(requests[0].find("\r\nConnection: close\r\n"), std::string::npos);
  EXPECT_EQ(requests[0].find("Content-Length"), std::string::npos);
}

TEST(HttpClientLiveTest, PostsABody) {
  std::vector<std::string> requests;
  RecordingHandler handler;
  {
    TestServer server({{"HTTP/1.1 500 Oops\r\nContent-Length: 4\r\n\r\nfail"}});
    HttpRequest request;
    request.method = "POST";
    request.url = server.Url("/api/v1/patches/check");
    request.headers = {{"Content-Type", "application/json"}};
    request.body = R"({"a":1})";
    EXPECT_EQ(SendHttpRequest(request, handler), HttpResult::kOk);
    requests = server.requests();
  }
  // Error statuses are delivered; judging them is the caller's job.
  EXPECT_EQ(handler.status, 500);
  EXPECT_EQ(handler.body, "fail");
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].rfind("POST /api/v1/patches/check HTTP/1.1\r\n", 0),
            0u);
  EXPECT_NE(requests[0].find("\r\nContent-Type: application/json\r\n"),
            std::string::npos);
  EXPECT_NE(requests[0].find("\r\nContent-Length: 7\r\n"), std::string::npos);
  EXPECT_EQ(requests[0].substr(requests[0].size() - 7), R"({"a":1})");
}

TEST(HttpClientLiveTest, FollowsGetRedirectsKeepingHeaders) {
  RecordingHandler handler;
  std::vector<std::string> requests;
  {
    TestServer server({
        {"HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 3\r\n"
         "\r\nabc"},
        {"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 5-6/7\r\n"
         "Content-Length: 2\r\n\r\nhi"},
    });
    EXPECT_EQ(Get(server.Url("/start"), handler, {}, {{"Range", "bytes=5-"}}),
              HttpResult::kOk);
    requests = server.requests();
  }
  EXPECT_EQ(handler.status, 206);
  EXPECT_EQ(handler.body, "hi");
  EXPECT_EQ(handler.response_count, 1);
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(requests[1].rfind("GET /final HTTP/1.1\r\n", 0), 0u);
  EXPECT_NE(requests[1].find("\r\nRange: bytes=5-\r\n"), std::string::npos);
}

TEST(HttpClientLiveTest, StopsAfterTooManyRedirects) {
  std::vector<Exchange> loop(3, {"HTTP/1.1 301 Moved\r\nLocation: /again\r\n"
                                 "Content-Length: 0\r\n\r\n"});
  TestServer server(loop);
  RecordingHandler handler;
  HttpClientOptions options;
  options.max_redirects = 2;
  EXPECT_EQ(Get(server.Url("/"), handler, options),
            HttpResult::kProtocolError);
  EXPECT_EQ(handler.response_count, 0);
}

TEST(HttpClientLiveTest, DoesNotFollowPostRedirects) {
  TestServer server(
      {{"HTTP/1.1 302 Found\r\nLocation: /elsewhere\r\nContent-Length: 0\r\n"
        "\r\n"}});
  HttpRequest request;
  request.method = "POST";
  request.url = server.Url("/");
  RecordingHandler handler;
  EXPECT_EQ(SendHttpRequest(request, handler), HttpResult::kOk);
  EXPECT_EQ(handler.status, 302);
}

TEST(HttpClientLiveTest, ConnectionRefusedIsATransportError) {
  // Bind a port, then close it, so nothing is listening there.
  int s = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  socklen_t length = sizeof(address);
  getsockname(s, reinterpret_cast<sockaddr*>(&address), &length);
  close(s);
  RecordingHandler handler;
  EXPECT_EQ(Get("http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) +
                    "/",
                handler),
            HttpResult::kTransportError);
}

TEST(HttpClientLiveTest, UnresolvableHostIsATransportError) {
  RecordingHandler handler;
  EXPECT_EQ(Get("http://does-not-exist.invalid/", handler),
            HttpResult::kTransportError);
}

TEST(HttpClientLiveTest, SilentServerTimesOut) {
  TestServer server({{"", /*respond=*/false}});
  RecordingHandler handler;
  HttpClientOptions options;
  options.io_timeout = std::chrono::milliseconds(200);
  EXPECT_EQ(Get(server.Url("/"), handler, options),
            HttpResult::kTransportError);
}

TEST(HttpClientLiveTest, ServerHangingUpIsATransportError) {
  TestServer server({{""}});
  RecordingHandler handler;
  EXPECT_EQ(Get(server.Url("/"), handler), HttpResult::kTransportError);
}

TEST(HttpClientLiveTest, RejectsUnsafeRequests) {
  RecordingHandler handler;
  EXPECT_EQ(Get("http://127.0.0.1:1/", handler, {}, {{"X", "a\r\nInjected: 1"}}),
            HttpResult::kInvalidRequest);
  EXPECT_EQ(Get("http://127.0.0.1:1/", handler, {}, {{"Bad Name", "a"}}),
            HttpResult::kInvalidRequest);
  EXPECT_EQ(Get("http://127.0.0.1:1/", handler, {}, {{"Host", "evil"}}),
            HttpResult::kInvalidRequest);
  EXPECT_EQ(
      Get("http://127.0.0.1:1/", handler, {}, {{"content-length", "1"}}),
      HttpResult::kInvalidRequest);
  HttpRequest put;
  put.method = "PUT";
  put.url = "http://127.0.0.1:1/";
  EXPECT_EQ(SendHttpRequest(put, handler), HttpResult::kInvalidRequest);
  EXPECT_EQ(Get("not a url", handler), HttpResult::kInvalidRequest);
  EXPECT_EQ(handler.response_count, 0);
}

TEST(HttpClientTlsTest, TrustedServerSucceeds) {
  TestCertificate certificate = MakeCertificate("localhost");
  TestServer server({{"HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecure"}},
                    &certificate);
  HttpClientOptions options;
  options.trusted_roots_pem = certificate.pem;
  RecordingHandler handler;
  EXPECT_EQ(Get(server.Url("/", "localhost"), handler, options),
            HttpResult::kOk);
  EXPECT_EQ(handler.body, "secure");
}

TEST(HttpClientTlsTest, RefusesRedirectFromHttpsToHttp) {
  TestServer plain({{"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nleak"}});
  TestCertificate certificate = MakeCertificate("localhost");
  TestServer server({{"HTTP/1.1 302 Found\r\nLocation: " + plain.Url("/") +
                      "\r\nContent-Length: 0\r\n\r\n"}},
                    &certificate);
  HttpClientOptions options;
  options.trusted_roots_pem = certificate.pem;
  RecordingHandler handler;
  EXPECT_EQ(Get(server.Url("/", "localhost"), handler, options),
            HttpResult::kProtocolError);
  EXPECT_EQ(handler.response_count, 0);
  EXPECT_TRUE(plain.requests().empty());
}

TEST(HttpClientTlsTest, TrustedServerByIpSucceeds) {
  TestCertificate certificate = MakeCertificate("localhost");
  TestServer server({{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nip"}},
                    &certificate);
  HttpClientOptions options;
  options.trusted_roots_pem = certificate.pem;
  RecordingHandler handler;
  EXPECT_EQ(Get(server.Url("/", "127.0.0.1"), handler, options),
            HttpResult::kOk);
  EXPECT_EQ(handler.body, "ip");
}

TEST(HttpClientTlsTest, UntrustedServerFails) {
  TestCertificate server_certificate = MakeCertificate("localhost");
  TestCertificate other = MakeCertificate("localhost");
  TestServer server({{"HTTP/1.1 200 OK\r\n\r\n"}}, &server_certificate);
  HttpClientOptions options;
  options.trusted_roots_pem = other.pem;
  RecordingHandler handler;
  EXPECT_EQ(Get(server.Url("/", "localhost"), handler, options),
            HttpResult::kTlsError);
  EXPECT_EQ(handler.response_count, 0);
}

TEST(HttpClientTlsTest, WrongHostFails) {
  TestCertificate certificate = MakeCertificate("other.example");
  TestServer server({{"HTTP/1.1 200 OK\r\n\r\n"}}, &certificate);
  HttpClientOptions options;
  options.trusted_roots_pem = certificate.pem;
  RecordingHandler handler;
  EXPECT_EQ(Get(server.Url("/", "localhost"), handler, options),
            HttpResult::kTlsError);
}

TEST(HttpClientTlsTest, PlatformTrustRejectsSelfSignedServer) {
  TestCertificate certificate = MakeCertificate("localhost");
  TestServer server({{"HTTP/1.1 200 OK\r\n\r\n"}}, &certificate);
  RecordingHandler handler;
  EXPECT_EQ(Get(server.Url("/", "localhost"), handler), HttpResult::kTlsError);
}

TEST(HttpClientTlsTest, PlainHttpServerIsNotTls) {
  TestServer server({{"HTTP/1.1 200 OK\r\n\r\n"}});
  RecordingHandler handler;
  std::string url = server.Url("/");
  url.replace(0, 4, "https");
  HttpClientOptions options;
  options.io_timeout = std::chrono::milliseconds(500);
  HttpResult result = Get(url, handler, options);
  EXPECT_TRUE(result == HttpResult::kTlsError ||
              result == HttpResult::kTransportError);
}

// Reaches the real Shorebird API through the platform's trusted roots. Not
// run by default (it needs the network); run with
// --gtest_also_run_disabled_tests.
TEST(HttpClientTlsTest, DISABLED_ReachesShorebirdApiWithPlatformRoots) {
  for (const char* url : {"https://api.shorebird.dev/",
                          "https://cdn.shorebird.cloud/",
                          "https://storage.googleapis.com/"}) {
    RecordingHandler handler;
    EXPECT_EQ(Get(url, handler), HttpResult::kOk) << url;
    EXPECT_GT(handler.status, 0) << url;
  }
}

// --- The C entry point the Rust updater calls --------------------------------

struct CRecorder {
  int status = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

bool COnResponse(void* context,
                 uint16_t status,
                 const ShorebirdHttpHeader* headers,
                 uintptr_t header_count) {
  auto* recorder = static_cast<CRecorder*>(context);
  recorder->status = status;
  for (uintptr_t i = 0; i < header_count; i++) {
    recorder->headers.emplace_back(headers[i].name, headers[i].value);
  }
  return true;
}

bool COnBody(void* context, const uint8_t* data, uintptr_t length) {
  static_cast<CRecorder*>(context)->body.append(
      reinterpret_cast<const char*>(data), length);
  return true;
}

TEST(UpdaterHttpRequestTest, SendsThroughTheCInterface) {
  CRecorder recorder;
  std::vector<std::string> requests;
  {
    TestServer server(
        {{"HTTP/1.1 201 Created\r\nX-Reply: yes\r\nContent-Length: 4\r\n\r\n"
          "done"}});
    std::string url = server.Url("/events");
    ShorebirdHttpHeader header = {"Content-Type", "application/json"};
    const char body[] = "{}";
    ShorebirdHttpRequest request = {"POST", url.c_str(), &header, 1,
                                    reinterpret_cast<const uint8_t*>(body), 2};
    ShorebirdHttpResponseCallbacks callbacks = {&recorder, COnResponse,
                                                COnBody};
    EXPECT_EQ(UpdaterHttpRequest(&request, &callbacks), SHOREBIRD_HTTP_OK);
    requests = server.requests();
  }
  EXPECT_EQ(recorder.status, 201);
  EXPECT_EQ(recorder.body, "done");
  EXPECT_NE(std::find(recorder.headers.begin(), recorder.headers.end(),
                      std::make_pair(std::string("X-Reply"),
                                     std::string("yes"))),
            recorder.headers.end());
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].substr(requests[0].size() - 2), "{}");
}

TEST(UpdaterHttpRequestTest, RejectsNullArguments) {
  CRecorder recorder;
  ShorebirdHttpResponseCallbacks callbacks = {&recorder, COnResponse, COnBody};
  ShorebirdHttpRequest request = {"GET", "http://127.0.0.1:1/", nullptr, 0,
                                  nullptr, 0};
  EXPECT_EQ(UpdaterHttpRequest(nullptr, &callbacks),
            SHOREBIRD_HTTP_INVALID_REQUEST);
  EXPECT_EQ(UpdaterHttpRequest(&request, nullptr),
            SHOREBIRD_HTTP_INVALID_REQUEST);
  ShorebirdHttpRequest missing_headers = request;
  missing_headers.header_count = 2;
  EXPECT_EQ(UpdaterHttpRequest(&missing_headers, &callbacks),
            SHOREBIRD_HTTP_INVALID_REQUEST);
  ShorebirdHttpRequest missing_body = request;
  missing_body.body_length = 2;
  EXPECT_EQ(UpdaterHttpRequest(&missing_body, &callbacks),
            SHOREBIRD_HTTP_INVALID_REQUEST);
  ShorebirdHttpHeader null_header = {"X", nullptr};
  ShorebirdHttpRequest bad_header = request;
  bad_header.headers = &null_header;
  bad_header.header_count = 1;
  EXPECT_EQ(UpdaterHttpRequest(&bad_header, &callbacks),
            SHOREBIRD_HTTP_INVALID_REQUEST);
  ShorebirdHttpResponseCallbacks no_callbacks = {&recorder, nullptr, nullptr};
  EXPECT_EQ(UpdaterHttpRequest(&request, &no_callbacks),
            SHOREBIRD_HTTP_INVALID_REQUEST);
  EXPECT_EQ(recorder.status, 0);
}

// The whole path the app takes: the Rust updater, given the engine's
// callbacks, sends a real patch check through this client and parses the
// answer.
void* StubOpen() {
  return nullptr;
}
uintptr_t StubRead(void*, uint8_t*, uintptr_t) {
  return 0;
}
int64_t StubSeek(void*, int64_t, int32_t) {
  return 0;
}
void StubClose(void*) {}

TEST(UpdaterHttpRequestTest, RustUpdaterChecksForPatchesThroughThisClient) {
  char storage_template[] = "/tmp/shorebird_http_unittests_XXXXXX";
  ASSERT_NE(mkdtemp(storage_template), nullptr);
  std::string storage = storage_template;
  std::string libapp = storage + "/libapp.so";
  FILE* file = fopen(libapp.c_str(), "w");
  ASSERT_NE(file, nullptr);
  fputs("base", file);
  fclose(file);

  std::vector<std::string> requests;
  bool available;
  {
    // Chunked, split mid-token, as a server streaming JSON may send it.
    std::string json =
        R"({"patch_available":true,"patch":{"number":1,"hash":"abc",)"
        R"("download_url":"http://127.0.0.1:1/patch"}})";
    std::string first = json.substr(0, 20);
    std::string second = json.substr(20);
    auto hex = [](size_t n) {
      char buffer[16];
      snprintf(buffer, sizeof(buffer), "%zx", n);
      return std::string(buffer);
    };
    TestServer server({{"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n" +
                        hex(first.size()) + "\r\n" + first + "\r\n" +
                        hex(second.size()) + "\r\n" + second +
                        "\r\n0\r\n\r\n"}});
    shorebird_set_engine_callbacks(&UpdaterEngineCallbacks());
    const char* paths[] = {libapp.c_str()};
    AppParameters params = {"1.0.0+1", paths, 1, storage.c_str(),
                            storage.c_str()};
    ::FileCallbacks callbacks = {StubOpen, StubRead, StubSeek, StubClose};
    std::string yaml = "app_id: test-app-id\nbase_url: " + server.Url("");
    ASSERT_TRUE(shorebird_init(&params, callbacks, yaml.c_str()));
    available = shorebird_check_for_downloadable_update(nullptr);
    requests = server.requests();
  }
  EXPECT_TRUE(available);
  ASSERT_EQ(requests.size(), 1u);
  const std::string& request = requests[0];
  EXPECT_EQ(request.rfind("POST /api/v1/patches/check HTTP/1.1\r\n", 0), 0u)
      << request;
  EXPECT_NE(
      request.find("\r\nContent-Type: application/json; charset=utf-8\r\n"),
      std::string::npos);
  EXPECT_NE(request.find("\"app_id\":\"test-app-id\""), std::string::npos);
  EXPECT_NE(request.find("\"release_version\":\"1.0.0+1\""),
            std::string::npos);
}

#endif  // !defined(_WIN32)

}  // namespace testing
}  // namespace shorebird
}  // namespace flutter
