#pragma once
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(CROSSPOINT_SIM_HEAP_ARENA)
#include "SimulatorHeap.h"
#endif

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
  static constexpr uint32_t HEAP_SIZE = 1024 * 1024;

  static uint32_t heapValue(const char *name) {
    const char *value = std::getenv(name);
    if (!value || *value == '\0')
      return HEAP_SIZE;

    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno == ERANGE || *end != '\0' || parsed > HEAP_SIZE)
      return HEAP_SIZE;
    return static_cast<uint32_t>(parsed);
  }

  uint32_t getFreeHeap() {
#if defined(CROSSPOINT_SIM_HEAP_ARENA)
    if (SimulatorHeap::isActive())
      return static_cast<uint32_t>(SimulatorHeap::freeBytes());
#endif
    return heapValue("CROSSPOINT_SIM_FREE_HEAP");
  }
  void restart() {}
  uint32_t getHeapSize() {
#if defined(CROSSPOINT_SIM_HEAP_ARENA)
    if (SimulatorHeap::isActive())
      return static_cast<uint32_t>(SimulatorHeap::totalBytes());
#endif
    return HEAP_SIZE;
  }
  uint32_t getMinFreeHeap() {
#if defined(CROSSPOINT_SIM_HEAP_ARENA)
    if (SimulatorHeap::isActive())
      return static_cast<uint32_t>(SimulatorHeap::minFreeBytes());
#endif
    return getFreeHeap();
  }
  uint32_t getMaxAllocHeap() {
#if defined(CROSSPOINT_SIM_HEAP_ARENA)
    if (SimulatorHeap::isActive())
      return static_cast<uint32_t>(SimulatorHeap::largestFreeBlockBytes());
#endif
    return std::min(heapValue("CROSSPOINT_SIM_MAX_ALLOC_HEAP"), getFreeHeap());
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
