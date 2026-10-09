#pragma once

#include <atomic>

// The render task publishes successful content display; only the loop/exit
// owner consumes the one-shot request and writes persistent reader state.
class ReaderResumeGate {
 public:
  void markPageRendered() { rendered.store(true, std::memory_order_release); }

  bool takeRememberRequest() {
    if (remembered || !rendered.load(std::memory_order_acquire)) return false;
    remembered = true;
    return true;
  }

 private:
  std::atomic<bool> rendered{false};
  bool remembered = false;
};
