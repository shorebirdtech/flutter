#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_PROTECTED_DATA_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_PROTECTED_DATA_H_

#include <functional>
#include <memory>

namespace flutter {
namespace shorebird {

/// Abstraction over "wait until the platform is ready to let the Shorebird
/// updater write its state files, then start the update."
///
/// On iOS, files under `Library/Application Support/` inherit the default
/// `NSFileProtectionCompleteUntilFirstUserAuthentication` class. Before the
/// user has unlocked the device for the first time since boot, the OS
/// refuses writes under that directory with EPERM/EACCES and any update
/// the updater starts during that window will fail at the state-write
/// step. The iOS implementation of this interface defers the start until
/// `UIApplication.protectedDataAvailable` is true (either immediately, if
/// it already is, or via a one-shot observer registered with
/// `NSNotificationCenter` for
/// `UIApplicationProtectedDataDidBecomeAvailableNotification`).
///
/// Every other platform uses the default implementation, which starts
/// immediately. There is nothing to wait for on Android, desktop, or
/// embedded.
///
/// Ownership: an instance of `ProtectedDataGate` is owned by the
/// `Updater` singleton. Installing a new gate via
/// `Updater::SetProtectedDataGate` cancels any pending start on the
/// previous gate.
///
/// Thread safety: implementations must be safe to call from any thread.
/// `start_fn` may be invoked on a different thread than the one that
/// called `StartWhenAvailable` (the iOS impl dispatches to the main
/// queue).
///
/// `start_fn` warnings:
/// - Must be non-blocking — it is expected to hand real work off to a
///   background worker, as `Updater::StartUpdateThread()` does.
/// - Must not capture anything with shorter-than-process lifetime. The
///   gate may hold `start_fn` indefinitely (until the user unlocks the
///   device for the first time since boot). Capturing a short-lived
///   pointer or reference in the lambda is a dangling-reference bug
///   waiting to happen.
class ProtectedDataGate {
 public:
  virtual ~ProtectedDataGate() = default;

  /// Arrange for `start_fn` to be invoked when the platform is ready.
  /// Implementations may invoke it synchronously. If a previous
  /// `start_fn` on this gate is still pending, the implementation must
  /// cancel it before accepting the new one — there is only ever one
  /// pending start per gate.
  virtual void StartWhenAvailable(std::function<void()> start_fn) = 0;

  /// Cancel any pending invocation from a prior `StartWhenAvailable`
  /// call. No-op if none is pending. Safe to call during destruction.
  virtual void CancelPending() = 0;
};

/// Creates the default `ProtectedDataGate` used on every platform that
/// does not install its own. Invokes `start_fn` synchronously;
/// `CancelPending` is a no-op.
std::unique_ptr<ProtectedDataGate> MakeImmediateProtectedDataGate();

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_PROTECTED_DATA_H_
