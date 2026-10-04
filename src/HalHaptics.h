#pragma once

#include <cstdint>

// Match the firmware feedback API; the host has no vibration motor.
class HalHaptics {
public:
  static void longPress(bool /*enabled*/, uint8_t /*intensity*/) {}
  static void feedback(bool /*enabled*/, bool /*pressed*/,
                       uint8_t /*intensity*/) {}
};
