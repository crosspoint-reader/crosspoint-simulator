#include "HalFrontlight.h"

#include <BoardConfig.h>

#include <algorithm>
#include <iostream>

HalFrontlight &HalFrontlight::getInstance() {
  static HalFrontlight instance;
  return instance;
}

void HalFrontlight::begin(uint8_t brightness, uint8_t warmth, bool on) {
  if (!present())
    return;
  lastBrightness = std::min<uint8_t>(brightness, 100);
  lastWarmth = hasColorTemperature() ? std::min<uint8_t>(warmth, 100) : 0;
  lit = on;
  std::cerr << "[SIM] Frontlight: " << (lit ? "on" : "off")
            << ", brightness=" << static_cast<unsigned>(lastBrightness)
            << "%, warmth=" << static_cast<unsigned>(lastWarmth) << "%"
            << std::endl;
}

bool HalFrontlight::present() const {
  return BoardConfig::hasPwmFrontlight();
}

bool HalFrontlight::hasColorTemperature() const {
  return BoardConfig::isX4Pro();
}

void HalFrontlight::setBrightness(uint8_t percent) {
  lastBrightness = std::min<uint8_t>(percent, 100);
}

void HalFrontlight::setWarmth(uint8_t warmPercent) {
  if (hasColorTemperature())
    lastWarmth = std::min<uint8_t>(warmPercent, 100);
}

void HalFrontlight::setOn(bool on) { lit = present() && on; }

uint8_t HalFrontlight::brightness() const { return lastBrightness; }

uint8_t HalFrontlight::warmth() const { return lastWarmth; }

bool HalFrontlight::isOn() const { return lit; }
