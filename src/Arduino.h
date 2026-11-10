#pragma once
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "freertos/FreeRTOS.h"
#include "esp_system.h"

#ifdef _WIN32
#include <cstring>
// newlib (the device), macOS and glibc 2.38+ provide BSD strlcpy; MinGW does
// not. Same contract: copy at most size-1 bytes, always terminate, return the
// length of src.
inline size_t strlcpy(char *dst, const char *src, size_t size) {
  const size_t len = strlen(src);
  if (size > 0) {
    const size_t n = len < size - 1 ? len : size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
  }
  return len;
}

#include <sys/time.h>
// MinGW has no settimeofday(). The firmware sets the clock through it; a
// simulator user process may not, which Linux and macOS report as EPERM too.
inline int settimeofday(const struct timeval *, const void *) {
  errno = EPERM;
  return -1;
}
#endif

#define PROGMEM
#define ICACHE_RODATA_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_NOINIT_ATTR
#define SET_LOOP_TASK_STACK_SIZE(bytes)
#define PGM_P const char *
#define PSTR(s) (s)
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#define memcpy_P(dest, src, num) memcpy((dest), (src), (num))

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
inline void configTzTime(const char *, const char *) {}

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

  uint32_t getFreeHeap() { return heapValue("CROSSPOINT_SIM_FREE_HEAP"); }
  void restart() {}
  uint32_t getHeapSize() { return HEAP_SIZE; }
  uint32_t getMinFreeHeap() { return getFreeHeap(); }
  uint32_t getMaxAllocHeap() {
    return std::min(heapValue("CROSSPOINT_SIM_MAX_ALLOC_HEAP"), getFreeHeap());
  }
  const char *getChipModel() const { return "simulator"; }
  uint32_t getChipRevision() const { return 0; }
  uint32_t getFlashChipSize() const { return 16U * 1024U * 1024U; }
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
