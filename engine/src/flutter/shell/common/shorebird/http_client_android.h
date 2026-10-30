// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_CLIENT_ANDROID_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_CLIENT_ANDROID_H_

#include <jni.h>

namespace flutter {
namespace shorebird {

// Finds the embedding's ShorebirdHttpClient for `SendHttpRequest`. Call from
// JNI_OnLoad, where the app's class loader is in reach. Returns false if the
// class is missing, and requests then fail as transport errors.
bool RegisterHttpClient(JNIEnv* env);

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_HTTP_CLIENT_ANDROID_H_
