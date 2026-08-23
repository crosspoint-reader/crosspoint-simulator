#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFF
#define eIncrement 1
#define portTICK_PERIOD_MS 1

// Real FreeRTOS defines this in projdefs.h, reached through FreeRTOS.h, as
//   ( ( TickType_t ) ( xTimeInMs ) * configTICK_RATE_HZ ) / 1000
// with integer division, so it rounds down and a sub-tick delay becomes 0
// ticks. There is no configTICK_RATE_HZ here, and portTICK_PERIOD_MS above is
// the shim's one tick-rate definition. The port defines
// portTICK_PERIOD_MS == 1000 / configTICK_RATE_HZ, so substituting it reduces
// that expression exactly to a divide by it -- same rounding, and no
// multiply to overflow. At portTICK_PERIOD_MS == 1 it is the identity:
// pdMS_TO_TICKS(n) == n.
#define pdMS_TO_TICKS(xTimeInMs)                                               \
  ((uint32_t)((uint32_t)(xTimeInMs) / (uint32_t)portTICK_PERIOD_MS))

using BaseType_t = int;

// ESP-IDF's portMUX_TYPE is a spinlock used with taskENTER_CRITICAL /
// taskEXIT_CRITICAL to guard data shared between tasks (and, on multi-core
// targets, cores). The simulator has no real critical-section primitive, so
// back it with a real mutex to preserve the same mutual-exclusion semantics
// across host threads.
struct SimPortMux {
  std::recursive_mutex mtx;
};
typedef SimPortMux portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED                                           \
  {                                                                            \
  }

inline void taskENTER_CRITICAL(portMUX_TYPE *mux) { mux->mtx.lock(); }
inline void taskEXIT_CRITICAL(portMUX_TYPE *mux) { mux->mtx.unlock(); }
#define portENTER_CRITICAL(mux) taskENTER_CRITICAL(mux)
#define portEXIT_CRITICAL(mux) taskEXIT_CRITICAL(mux)

// TaskHandle wraps a real thread + a notification counter protected by a
// condvar.
struct SimTaskHandle {
  std::thread thread;
  std::mutex mtx;
  std::condition_variable cv;
  uint32_t notifyCount = 0;
  std::thread::id id;
  const char *name = "sim-task";
};
typedef SimTaskHandle *TaskHandle_t;
