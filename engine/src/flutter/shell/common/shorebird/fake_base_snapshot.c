// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Stands in for the App.framework a released Flutter app carries: a library
// nothing links, exporting the symbols DartSnapshot resolves out of
// settings.application_library_paths. Only the symbol addresses are read, so
// the bytes can be anything.

#include <stdint.h>

__attribute__((visibility("default"))) const uint8_t kDartSnapshotData[1] = {0};
__attribute__((visibility("default"))) const uint8_t kDartSnapshotText[1] = {0};
