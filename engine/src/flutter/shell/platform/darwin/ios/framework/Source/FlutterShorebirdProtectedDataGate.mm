#import "flutter/shell/platform/darwin/ios/framework/Source/FlutterShorebirdProtectedDataGate.h"

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <utility>

#include "flutter/fml/logging.h"

namespace flutter {
namespace shorebird {
namespace {

// Concrete iOS gate. Holds at most one pending
// `NSNotificationCenter` observer; installing a new pending start
// cancels the prior one first, and destruction releases the observer
// before deallocation.
//
// Thread-safety: `StartWhenAvailable` and `CancelPending` may be
// called from any thread. Both hop to the main queue before touching
// the observer handle or `UIApplication`, so all internal state
// mutation happens on the main queue.
class IOSProtectedDataGate : public ProtectedDataGate {
 public:
  IOSProtectedDataGate() = default;

  ~IOSProtectedDataGate() override {
    // Tear down synchronously on the main queue if needed. If the gate
    // is destroyed from the main thread this runs inline; otherwise it
    // dispatches sync to the main queue, which is safe because the
    // destructor blocks here anyway.
    if ([NSThread isMainThread]) {
      RemoveObserverOnMainQueue();
    } else {
      __block id observer_to_remove = observer_;
      observer_ = nil;
      dispatch_sync(dispatch_get_main_queue(), ^{
        if (observer_to_remove != nil) {
          [[NSNotificationCenter defaultCenter] removeObserver:observer_to_remove];
        }
      });
    }
  }

  void StartWhenAvailable(std::function<void()> start_fn) override {
    // Copy into a block-captured value; the __block lets the notification
    // block capture start_fn without a const copy.
    __block std::function<void()> captured_start_fn = std::move(start_fn);
    dispatch_async(dispatch_get_main_queue(), ^{
      // Cancel any prior pending start before queuing the new one.
      RemoveObserverOnMainQueue();

      UIApplication* app = [UIApplication sharedApplication];
      if (app == nil) {
        // Inside app extensions. No UIApplication lifecycle to observe,
        // and Data Protection is managed differently; fall back to
        // starting immediately so we do not silently block updates
        // forever.
        FML_LOG(INFO) << "Shorebird: UIApplication unavailable (likely app "
                         "extension); starting updater without data "
                         "protection gating.";
        captured_start_fn();
        return;
      }

      if (app.protectedDataAvailable) {
        captured_start_fn();
        return;
      }

      FML_LOG(INFO) << "Shorebird: protected data unavailable; deferring "
                       "updater kickoff until first unlock.";

      // Register a one-shot observer. The block retains captured_start_fn
      // until it fires, at which point we unregister and invoke it.
      observer_ = [[NSNotificationCenter defaultCenter]
          addObserverForName:UIApplicationProtectedDataDidBecomeAvailableNotification
                      object:nil
                       queue:[NSOperationQueue mainQueue]
                  usingBlock:^(NSNotification* _Nonnull note) {
                    // Move start_fn out before unregistering so releasing
                    // the block (which owns the closure containing
                    // captured_start_fn's storage) cannot destroy the
                    // function while we are still about to call it.
                    std::function<void()> to_invoke =
                        std::move(captured_start_fn);
                    RemoveObserverOnMainQueue();
                    FML_LOG(INFO)
                        << "Shorebird: protected data became available; "
                           "starting updater.";
                    to_invoke();
                  }];
    });
  }

  void CancelPending() override {
    if ([NSThread isMainThread]) {
      RemoveObserverOnMainQueue();
    } else {
      dispatch_async(dispatch_get_main_queue(), ^{
        RemoveObserverOnMainQueue();
      });
    }
  }

 private:
  // Main-queue-only: unregister any currently-held observer and clear
  // the handle. Safe to call when no observer is held.
  void RemoveObserverOnMainQueue() {
    if (observer_ != nil) {
      [[NSNotificationCenter defaultCenter] removeObserver:observer_];
      observer_ = nil;
    }
  }

  // __strong Obj-C pointer member inside a C++ class is valid in ARC
  // .mm files; ARC manages the retain/release across ctor/dtor of the
  // enclosing class. All reads/writes happen on the main queue.
  __strong id observer_ = nil;
};

}  // namespace

std::unique_ptr<ProtectedDataGate> MakeIOSProtectedDataGate() {
  return std::make_unique<IOSProtectedDataGate>();
}

}  // namespace shorebird
}  // namespace flutter
