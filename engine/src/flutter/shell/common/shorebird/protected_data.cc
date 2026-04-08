#include "flutter/shell/common/shorebird/protected_data.h"

#include <utility>

namespace flutter {
namespace shorebird {

namespace {

// Default gate implementation. Invokes `start_fn` synchronously on the
// caller's thread. Used on every platform that does not install its own
// gate: desktop, Android, embedded, and tests.
class ImmediateProtectedDataGate : public ProtectedDataGate {
 public:
  void StartWhenAvailable(std::function<void()> start_fn) override {
    start_fn();
  }
  void CancelPending() override {}
};

}  // namespace

std::unique_ptr<ProtectedDataGate> MakeImmediateProtectedDataGate() {
  return std::make_unique<ImmediateProtectedDataGate>();
}

}  // namespace shorebird
}  // namespace flutter
