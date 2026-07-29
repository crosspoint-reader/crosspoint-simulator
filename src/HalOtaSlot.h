#pragma once

#include <cstddef>
#include <cstdint>

class HalOtaSlot {
 public:
  enum class RunningImageState : uint8_t {
    PendingVerify,
    Confirmed,
    Untracked,
    Unsafe,
  };

  static constexpr size_t ERASE_SIZE = 4096;

  static HalOtaSlot inactive();
  static RunningImageState runningImageState();
  static bool confirmRunningImage();

  bool valid() const { return false; }
  size_t size() const { return 0; }
  bool safeForScratchWrite() const;
  bool read(size_t offset, void* data, size_t length) const;
  bool erase(size_t offset, size_t length) const;
  bool write(size_t offset, const void* data, size_t length) const;
};
