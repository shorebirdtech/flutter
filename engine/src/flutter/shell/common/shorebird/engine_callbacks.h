// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_ENGINE_CALLBACKS_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_ENGINE_CALLBACKS_H_

#include <cstddef>
#include <cstdint>

#include "flutter/shell/common/shorebird/http_client.h"
#include "third_party/updater/library/include/updater_engine.h"

namespace flutter {
namespace shorebird {

// The services the engine gives the Rust updater: patch signature checks on
// the engine's BoringSSL, everywhere, and HTTP through the platform's client
// (http_client.h) where one is built; elsewhere `http_request` is null.
// `RealUpdater` registers these before `shorebird_init`.
const ShorebirdEngineCallbacks& UpdaterEngineCallbacks();

// Sends an updater request through `send` and delivers the response to
// `callbacks`, as `ShorebirdEngineCallbacks::http_request` does with the
// platform's client. Exposed for tests.
int32_t ForwardHttpRequest(const ShorebirdHttpRequest* request,
                           const ShorebirdHttpResponseCallbacks* callbacks,
                           HttpSender send);

// `ShorebirdEngineCallbacks::verify_signature`. Exposed for tests.
bool UpdaterVerifySignature(const uint8_t* public_key,
                            uintptr_t public_key_length,
                            const uint8_t* message,
                            uintptr_t message_length,
                            const uint8_t* signature,
                            uintptr_t signature_length);

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_ENGINE_CALLBACKS_H_
