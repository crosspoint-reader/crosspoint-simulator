#pragma once

#include <Arduino.h>

// Host model of device frontlight state. The framebuffer is
// unchanged; this class preserves the firmware-visible brightness, warmth, and
// on/off state so the quick panel and persisted settings can be exercised.
class HalFrontlight {
public:
  static HalFrontlight &getInstance();

  void begin(uint8_t brightness, uint8_t warmth, bool on);

  bool present() const;
  bool hasColorTemperature() const;

  void setBrightness(uint8_t percent);
  void setWarmth(uint8_t warmPercent);
  void setOn(bool on);

  uint8_t brightness() const;
  uint8_t warmth() const;
  bool isOn() const;

private:
  uint8_t lastBrightness = 60;
  uint8_t lastWarmth = 50;
  bool lit = false;
};

#define Frontlight HalFrontlight::getInstance()
