#ifndef FLUTTER_SHELL_PLATFORM_DARWIN_IOS_FRAMEWORK_SOURCE_FLUTTERSHOREBIRDPROTECTEDDATAGATE_H_
#define FLUTTER_SHELL_PLATFORM_DARWIN_IOS_FRAMEWORK_SOURCE_FLUTTERSHOREBIRDPROTECTEDDATAGATE_H_

#include <memory>

#include "flutter/shell/common/shorebird/protected_data.h"

namespace flutter {
namespace shorebird {

/// Creates an iOS `ProtectedDataGate` that defers a pending
/// `StartWhenAvailable` callback until
/// `UIApplication.protectedDataAvailable` is true.
///
/// Behavior:
///  - Dispatches to the main queue (required for `UIApplication`
///    access).
///  - If protected data is already available, invokes `start_fn`
///    synchronously on the main queue.
///  - Otherwise registers a one-shot observer for
///    `UIApplicationProtectedDataDidBecomeAvailableNotification`.
///    When the notification fires (typically the first time the user
///    unlocks the device after boot), the observer unregisters itself
///    and calls `start_fn` on the main queue.
///  - If `StartWhenAvailable` is called again while a prior callback
///    is still pending, the prior observer is removed before the new
///    one is registered.
///  - `CancelPending()` removes any currently-registered observer.
///  - Destruction calls `CancelPending()` so the gate never leaks an
///    observer past its lifetime.
///
/// Falls back to invoking `start_fn` immediately if
/// `UIApplication.sharedApplication` is nil, which happens inside app
/// extensions where there is no full `UIApplication` lifecycle.
std::unique_ptr<ProtectedDataGate> MakeIOSProtectedDataGate();

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_DARWIN_IOS_FRAMEWORK_SOURCE_FLUTTERSHOREBIRDPROTECTEDDATAGATE_H_
