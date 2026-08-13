#pragma once
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>

#define PROGMEM
#define ICACHE_RODATA_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_NOINIT_ATTR
#define PGM_P const char *
#define PSTR(s) (s)

inline unsigned long millis() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return duration_cast<milliseconds>(steady_clock::now() - start).count();
}

inline unsigned long micros() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return duration_cast<microseconds>(steady_clock::now() - start).count();
}

inline void delay(unsigned long ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
inline void yield() { std::this_thread::yield(); }

// Native builds have no GPIO pins. Treat every input as released, matching the
// idle pull-up state used by the button diagnostics in firmware startup.
inline int digitalRead(int /*pin*/) { return 1; }

#include "HardwareSerial.h"
#include "Print.h"
#include "WString.h"

struct ESPMock {
  static uint32_t heapValue(const char *name) {
    const char *value = std::getenv(name);
    if (!value || *value == '\0')
      return 1024 * 1024;

    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (*end != '\0' || parsed > std::numeric_limits<uint32_t>::max())
      return 1024 * 1024;
    return static_cast<uint32_t>(parsed);
  }

  uint32_t getFreeHeap() { return heapValue("CROSSPOINT_SIM_FREE_HEAP"); }
  void restart() {}
  uint32_t getHeapSize() { return 1024 * 1024; }
  uint32_t getMinFreeHeap() { return getFreeHeap(); }
  uint32_t getMaxAllocHeap() {
    return heapValue("CROSSPOINT_SIM_MAX_ALLOC_HEAP");
  }
};
extern ESPMock ESP;

inline long random(long max) { return std::rand() % max; }

template <typename A, typename B>
constexpr auto max(A a, B b) -> decltype(a > b ? a : b) {
  return a > b ? a : b;
}
template <typename A, typename B>
constexpr auto min(A a, B b) -> decltype(a < b ? a : b) {
  return a < b ? a : b;
}
