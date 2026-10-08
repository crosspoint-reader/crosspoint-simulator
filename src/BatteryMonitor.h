#pragma once
class BatteryMonitor {
public:
  BatteryMonitor(int pin, int factor = -1) {}
  void begin() {}
  static bool loadDesignCapacity() { return false; }
  int getVoltage() { return 4200; }
  int getPercentage() { return 100; }
};
