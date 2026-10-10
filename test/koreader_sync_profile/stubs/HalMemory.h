#pragma once
#include <Arduino.h>

#include <cstddef>
class HalMemory {
 public:
  struct Heap {
    size_t freeBytes;
    size_t largestBlockBytes;
  };
  static Heap getDefaultHeap() { return {1024 * 1024, 1024 * 1024}; }
};
