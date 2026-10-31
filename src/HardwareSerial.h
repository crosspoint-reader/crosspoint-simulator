#pragma once
#include <cstdio>
#include <cstring>
#include <iostream>

#include "Arduino.h"
#include "Print.h"
#include "SimLog.h"
#include "Stream.h"
#include "WString.h"
class HWCDC : public Stream {
public:
  void begin(unsigned long baud) {}
  void setTxTimeoutMs(uint32_t timeoutMs) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *buffer, size_t size) override {
    std::cerr.write((const char *)buffer, size);
    sim_log::write((const char *)buffer, size);
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  template <typename... Args> void printf(const char *format, Args... args) {
    if constexpr (sizeof...(Args) == 0) {
      write((const uint8_t *)format, strlen(format));
    } else {
      char buf[256];
      snprintf(buf, sizeof(buf), format, args...);
      write((const uint8_t *)buf, strlen(buf));
    }
  }
  operator bool() const { return true; }
};

// CrossPoint uses HardwareSerial when ARDUINO_USB_CDC_ON_BOOT is not defined.
// The simulator has a single stderr-backed serial endpoint, so both Arduino
// serial types intentionally resolve to the same host implementation.
using HardwareSerial = HWCDC;

extern HWCDC Serial;
