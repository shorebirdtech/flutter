// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Windows: BoringSSL verifies against the certificates in the Windows
// system stores, copied in as the Dart SDK's
// runtime/bin/security_context_win.cc (`AddCertificatesFromRootStore`) does.

#include "flutter/shell/common/shorebird/http_trust.h"

#include <windows.h>

#include <wincrypt.h>

#include "openssl/err.h"
#include "openssl/x509.h"

#pragma comment(lib, "crypt32.lib")

namespace flutter {
namespace shorebird {

namespace {

bool IsCurrentlyValid(X509* cert) {
  return X509_cmp_current_time(X509_get0_notBefore(cert)) < 0 &&
         X509_cmp_current_time(X509_get0_notAfter(cert)) > 0;
}

// Returns the number of certificates added from one named store.
int AddCertificatesFromStore(const wchar_t* name,
                             DWORD location,
                             X509_STORE* store) {
  HCERTSTORE cert_store =
      CertOpenStore(CERT_STORE_PROV_SYSTEM, 0, NULL,
                    location | CERT_STORE_READONLY_FLAG, name);
  if (cert_store == nullptr) {
    return 0;
  }
  int added = 0;
  PCCERT_CONTEXT cert_context = nullptr;
  while ((cert_context = CertEnumCertificatesInStore(cert_store,
                                                     cert_context)) !=
         nullptr) {
    const unsigned char* der = cert_context->pbCertEncoded;
    X509* cert = d2i_X509(nullptr, &der, cert_context->cbCertEncoded);
    if (cert == nullptr) {
      ERR_clear_error();
      continue;
    }
    // As dart:io does: an expired root can only get in the way.
    if (IsCurrentlyValid(cert) && X509_STORE_add_cert(store, cert)) {
      added++;
    }
    // Duplicates across stores fail to add; that is fine.
    ERR_clear_error();
    X509_free(cert);
  }
  CertCloseStore(cert_store, 0);
  return added;
}

}  // namespace

bool ConfigurePlatformTrust(SSL_CTX* context) {
  SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
  X509_STORE* store = SSL_CTX_get_cert_store(context);
  int added = 0;
  for (DWORD location :
       {CERT_SYSTEM_STORE_CURRENT_USER, CERT_SYSTEM_STORE_LOCAL_MACHINE}) {
    for (const wchar_t* name : {L"ROOT", L"CA", L"TRUST", L"MY"}) {
      added += AddCertificatesFromStore(name, location, store);
    }
  }
  return added > 0;
}

void ConfigurePlatformVerification(SSL* ssl, const std::string* host) {}

}  // namespace shorebird
}  // namespace flutter
