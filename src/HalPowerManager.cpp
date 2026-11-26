#include "HalPowerManager.h"

#include "HalGPIO.h"
#include "SimulatorLifecycle.h"

HalPowerManager powerManager;

void HalPowerManager::begin() {}
void HalPowerManager::startDeepSleep(HalGPIO &gpio,
                                     bool wakeOnHeldButton) const {
  if (wakeOnHeldButton)
    SimulatorLifecycle::rebootAsPowerWake();
  gpio.startDeepSleep();
}
#if FREEINK_DEVICE_PICCO
bool HalPowerManager::holdWhileUsbPowered(
    HalGPIO & /*gpio*/, void (* /*onStatus*/)(bool, uint16_t)) const {
  // No charger model: keep the existing host sleep/wake path available.
  return false;
}
#endif
void HalPowerManager::setPowerSaving(bool enable) {}
uint16_t HalPowerManager::getBatteryPercentage() const { return 100; }
bool HalPowerManager::readBatteryPercentage(uint16_t &percent) const {
  percent = getBatteryPercentage();
  return true;
}

HalPowerManager::Lock::Lock() {}
HalPowerManager::Lock::~Lock() {}
