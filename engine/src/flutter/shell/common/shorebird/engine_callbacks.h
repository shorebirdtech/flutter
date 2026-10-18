// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_ENGINE_CALLBACKS_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_ENGINE_CALLBACKS_H_

#include <cstddef>
#include <cstdint>

#include "third_party/updater/library/include/updater_engine.h"

namespace flutter {
namespace shorebird {

// The services the engine gives the Rust updater: HTTP through
// `SendHttpRequest` and patch signature checks, both on the engine's
// BoringSSL. `RealUpdater` registers these before `shorebird_init`.
const ShorebirdEngineCallbacks& UpdaterEngineCallbacks();

// The C entry points, exposed for tests.
int32_t UpdaterHttpRequest(const ShorebirdHttpRequest* request,
                           const ShorebirdHttpResponseCallbacks* callbacks);
bool UpdaterVerifySignature(const uint8_t* public_key,
                            uintptr_t public_key_length,
                            const uint8_t* message,
                            uintptr_t message_length,
                            const uint8_t* signature,
                            uintptr_t signature_length);

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_ENGINE_CALLBACKS_H_
