// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// iOS and macOS: the system's trust evaluation decides, as in the Dart SDK's
// runtime/bin/security_context_macos.cc (`CertificateVerificationCallback`):
// BoringSSL hands over the server's chain, and Security.framework checks it
// against the system's trusted roots (including any the user or a profile
// installed) and checks the host name. dart:io evaluates on another thread
// so as not to block its isolate; the updater's requests already run off
// the UI thread, so this evaluates in place.

#include "flutter/shell/common/shorebird/http_trust.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include "flutter/fml/platform/darwin/cf_utils.h"

namespace flutter {
namespace shorebird {

namespace {

int HostIndex() {
  static int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  return index;
}

ssl_verify_result_t VerifyWithSystemTrust(SSL* ssl, uint8_t* out_alert) {
  *out_alert = SSL_AD_CERTIFICATE_UNKNOWN;
  const auto* host = static_cast<const std::string*>(
      SSL_get_ex_data(ssl, HostIndex()));
  const STACK_OF(CRYPTO_BUFFER)* chain = SSL_get0_peer_certificates(ssl);
  if (host == nullptr || chain == nullptr || sk_CRYPTO_BUFFER_num(chain) == 0) {
    return ssl_verify_invalid;
  }

  fml::CFRef<CFMutableArrayRef> certificates(
      CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks));
  if (!certificates) {
    return ssl_verify_invalid;
  }
  for (const CRYPTO_BUFFER* buffer : chain) {
    fml::CFRef<CFDataRef> der(CFDataCreate(
        nullptr, CRYPTO_BUFFER_data(buffer), CRYPTO_BUFFER_len(buffer)));
    if (!der) {
      return ssl_verify_invalid;
    }
    fml::CFRef<SecCertificateRef> certificate(
        SecCertificateCreateWithData(nullptr, der));
    if (!certificate) {
      return ssl_verify_invalid;
    }
    CFArrayAppendValue(certificates, certificate);
  }

  fml::CFRef<CFStringRef> hostname(CFStringCreateWithCString(
      nullptr, host->c_str(), kCFStringEncodingUTF8));
  if (!hostname) {
    return ssl_verify_invalid;
  }
  fml::CFRef<SecPolicyRef> policy(SecPolicyCreateSSL(true, hostname));
  SecTrustRef raw_trust = nullptr;
  if (!policy || SecTrustCreateWithCertificates(certificates, policy,
                                                &raw_trust) != errSecSuccess) {
    return ssl_verify_invalid;
  }
  fml::CFRef<SecTrustRef> trust(raw_trust);
  if (!SecTrustEvaluateWithError(trust, nullptr)) {
    return ssl_verify_invalid;
  }
  return ssl_verify_ok;
}

}  // namespace

bool ConfigurePlatformTrust(SSL_CTX* context) {
  // The roots stay in the system; nothing to load.
  return HostIndex() >= 0;
}

void ConfigurePlatformVerification(SSL* ssl, const std::string* host) {
  SSL_set_ex_data(ssl, HostIndex(), const_cast<std::string*>(host));
  SSL_set_custom_verify(ssl, SSL_VERIFY_PEER, VerifyWithSystemTrust);
}

}  // namespace shorebird
}  // namespace flutter
