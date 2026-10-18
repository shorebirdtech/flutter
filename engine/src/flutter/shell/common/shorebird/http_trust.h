// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_TRUST_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_TRUST_H_

#include <string>

#include "openssl/ssl.h"

namespace flutter {
namespace shorebird {

// How the HTTP client trusts servers, per platform. Each platform finds its
// trusted roots the way dart:io does (runtime/bin/security_context_*.cc in
// the Dart SDK), so the updater trusts what the app's own HTTPS traffic
// trusts.

// Sets up `context` to verify servers against the platform's trusted roots.
// Returns false if the platform's roots could not be found.
bool ConfigurePlatformTrust(SSL_CTX* context);

// Sets up one connection, for platforms that verify certificates outside
// BoringSSL (Apple's trust evaluation). `host` must outlive the handshake.
void ConfigurePlatformVerification(SSL* ssl, const std::string* host);

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_TRUST_H_
