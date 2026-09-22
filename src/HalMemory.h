#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

class HalMemory {
 public:
  struct HeapStats {
    size_t freeBytes;
    size_t totalBytes;
    size_t minFreeBytes;
    size_t largestBlockBytes;
  };

  struct PsramDeleter {
    void operator()(uint8_t* buffer) const;
  };
  using PsramBuffer = std::unique_ptr<uint8_t[], PsramDeleter>;
  // The simulator has no PSRAM; always null, matching the firmware's
  // documented behavior for devices without it.
  static PsramBuffer allocatePsram(size_t bytes);

  static HeapStats getDefaultHeap();
  static HeapStats getInternalHeap();
  static HeapStats getPsramHeap();
};
