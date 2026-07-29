#pragma once
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <thread>

#include "esp_random.h"
#include "freertos/task.h"

#define PROGMEM
#define pgm_read_byte_near(address) (*(const unsigned char *)(address))
#define ICACHE_RODATA_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_NOINIT_ATTR
#define PGM_P const char *
#define PSTR(s) (s)

using boolean = bool;

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

#include "HardwareSerial.h"
#include "Print.h"
#include "WString.h"

struct ESPMock {
  uint32_t getFreeHeap() { return 1024 * 1024; }
  void restart() {}
  uint32_t getHeapSize() { return 1024 * 1024; }
  uint32_t getMinFreeHeap() { return 1024 * 1024; }
  uint32_t getMaxAllocHeap() { return 1024 * 1024; }
};
extern ESPMock ESP;

namespace simulator_arduino_detail {
struct RandomEngine {
  using result_type = uint64_t;
  static constexpr result_type min() { return 0; }
  static constexpr result_type max() {
    return std::numeric_limits<result_type>::max();
  }
  result_type operator()() const {
    result_type value = 0;
    esp_fill_random(&value, sizeof(value));
    return value;
  }
};
} // namespace simulator_arduino_detail

inline long random(long max) {
  if (max <= 0)
    return 0;
  simulator_arduino_detail::RandomEngine engine;
  return std::uniform_int_distribution<long>(0, max - 1)(engine);
}
inline long random(long min, long max) {
  return min < max ? min + random(max - min) : min;
}

template <typename A, typename B>
constexpr auto max(A a, B b) -> decltype(a > b ? a : b) {
  return a > b ? a : b;
}
template <typename A, typename B>
constexpr auto min(A a, B b) -> decltype(a < b ? a : b) {
  return a < b ? a : b;
}
