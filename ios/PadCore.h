#pragma once

#include <vector>

// The pure decision logic behind the on-screen button pad: finger events in,
// press/release actions out. Deliberately SDL-free and CLOCK-FREE.
//
// THE CONTRACT IS PURE PASSTHROUGH. A finger down inside a slot presses that
// slot's button; the matching finger up (or dragging off the slot, or a reset
// on backgrounding) releases it. Nothing else exists: no timers, no gesture
// widening, no per-slot special cases. The API has no way to express time, so
// time-based inventions (the old POWER tap-stretch, which held the injected
// button 600 ms past the finger and made the control look stuck) cannot come
// back without changing this header -- which is the point.
//
// Tested by tests/pad_core_test.cpp; the SDL adapter in CrossPointIOSShim.cpp
// translates finger events to these calls and actions to HalGPIO injections,
// and holds no button state of its own.
class PadCore {
 public:
  static constexpr int kNoSlot = -1;

  struct Action {
    enum Type { Press, Release };
    Type type;
    int slot;
  };

  explicit PadCore(int slotCount)
      : down_(static_cast<size_t>(slotCount), false),
        finger_(static_cast<size_t>(slotCount), 0) {}

  // Finger landed; `slot` is the hit-tested pad slot (kNoSlot = outside).
  // Presses the slot unless it is already held by another finger.
  std::vector<Action> fingerDown(int slot, long long fingerId);

  // Finger lifted; releases the slot this finger holds, if any.
  std::vector<Action> fingerUp(long long fingerId);

  // Finger dragged outside the slot it pressed; cancels that press. The
  // finger's later lift then releases nothing.
  std::vector<Action> fingerLeftSlot(long long fingerId);

  // Backgrounding / focus loss: release everything. Idempotent.
  std::vector<Action> reset();

  bool isDown(int slot) const {
    return slot >= 0 && slot < static_cast<int>(down_.size()) &&
           down_[static_cast<size_t>(slot)];
  }

  // The slot this finger is holding, or kNoSlot. For the adapter's
  // drag-off-the-control check; a pure read.
  int heldSlot(long long fingerId) const { return slotHeldBy(fingerId); }

 private:
  int slotHeldBy(long long fingerId) const;

  std::vector<bool> down_;
  std::vector<long long> finger_;
};
