#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  explicit RenderLock();
  // A non-blocking lock is useful for input handlers: they can retain an
  // intent and return to GPIO polling when rendering is busy.
  explicit RenderLock(bool waitForLock);
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  bool ownsLock() const { return isLocked; }
  static bool peek();
};
