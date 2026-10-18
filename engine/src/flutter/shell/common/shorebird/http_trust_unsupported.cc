// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Platforms the updater does not support have no trusted roots, so every
// HTTPS request fails.

#include "flutter/shell/common/shorebird/http_trust.h"

namespace flutter {
namespace shorebird {

bool ConfigurePlatformTrust(SSL_CTX* context) {
  return false;
}

void ConfigurePlatformVerification(SSL* ssl, const std::string* host) {}

}  // namespace shorebird
}  // namespace flutter
