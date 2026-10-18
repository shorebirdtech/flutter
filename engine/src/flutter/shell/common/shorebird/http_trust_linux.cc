// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Android and Linux: BoringSSL verifies against the system's certificate
// files, found as in the Dart SDK's runtime/bin/security_context_linux.cc
// (`SSLCertContext::TrustBuiltinRoots`).

#include "flutter/shell/common/shorebird/http_trust.h"

#include <sys/stat.h>

#include "flutter/fml/build_config.h"
#include "openssl/err.h"

namespace flutter {
namespace shorebird {

namespace {

#if !defined(FML_OS_ANDROID)
bool IsFile(const char* path) {
  struct stat info;
  return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}
#endif

bool IsDirectory(const char* path) {
  struct stat info;
  return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

#if !defined(FML_OS_ANDROID)
bool LoadFile(SSL_CTX* context, const char* file) {
  bool loaded = SSL_CTX_load_verify_locations(context, file, nullptr) == 1;
  ERR_clear_error();
  return loaded;
}
#endif

bool LoadDirectory(SSL_CTX* context, const char* directory) {
  bool loaded =
      SSL_CTX_load_verify_locations(context, nullptr, directory) == 1;
  ERR_clear_error();
  return loaded;
}

}  // namespace

bool ConfigurePlatformTrust(SSL_CTX* context) {
  SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
#if defined(FML_OS_ANDROID)
  // As dart:io does: the device's own directory of trusted certificates,
  // which BoringSSL reads lazily during each handshake.
  const char* kAndroidCaCerts = "/system/etc/security/cacerts";
  return IsDirectory(kAndroidCaCerts) &&
         LoadDirectory(context, kAndroidCaCerts);
#else
  // The first of these that exists, as dart:io does (its list is copied
  // from gRPC). dart:io falls back to roots compiled into the Dart SDK,
  // which the engine does not carry.
  const char* kCertFiles[] = {
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/pki/tls/certs/ca-bundle.crt",
      "/etc/ssl/ca-bundle.pem",
      "/etc/pki/tls/cacert.pem",
      "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
  };
  const char* kCertDirectories[] = {
      "/etc/ssl/certs",         "/system/etc/security/cacerts",
      "/usr/local/share/certs", "/etc/pki/tls/certs",
      "/etc/openssl/certs",
  };
  for (const char* file : kCertFiles) {
    if (IsFile(file)) {
      return LoadFile(context, file);
    }
  }
  for (const char* directory : kCertDirectories) {
    if (IsDirectory(directory)) {
      return LoadDirectory(context, directory);
    }
  }
  return false;
#endif
}

void ConfigurePlatformVerification(SSL* ssl, const std::string* host) {}

}  // namespace shorebird
}  // namespace flutter
