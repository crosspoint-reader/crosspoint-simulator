#pragma once
#include <chrono>
#include <cstdint>

#include "FreeRTOS.h"

// Thread-local pointer so ulTaskNotifyTake can find the current task's handle.
inline thread_local SimTaskHandle *tl_currentTaskHandle = nullptr;

inline SimTaskHandle *simMainTaskHandle() {
  static SimTaskHandle mainTask;
  static std::once_flag initFlag;
  std::call_once(initFlag, [] {
    mainTask.name = "main";
    mainTask.id = std::this_thread::get_id();
  });
  return &mainTask;
}

inline TaskHandle_t xTaskGetCurrentTaskHandle() {
  return tl_currentTaskHandle ? tl_currentTaskHandle : simMainTaskHandle();
}

// Create a real OS thread. The FreeRTOS task function signature is
// void(*)(void*).
inline BaseType_t xTaskCreate(void (*fn)(void *), const char *name,
                              uint32_t /*stackDepth*/, void *param,
                              BaseType_t /*priority*/, TaskHandle_t *handle) {
  auto *h = new SimTaskHandle();
  h->name = name ? name : "sim-task";
  h->thread = std::thread([fn, param, h]() {
    tl_currentTaskHandle = h;
    h->id = std::this_thread::get_id();
    fn(param);
  });
  if (handle)
    *handle = h;
  return 1; // pdPASS
}

// Core pinning has no meaning on the host; delegate to xTaskCreate and
// ignore the core ID.
inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                          uint32_t stackDepth, void *param,
                                          BaseType_t priority,
                                          TaskHandle_t *handle,
                                          BaseType_t /*coreId*/) {
  return xTaskCreate(fn, name, stackDepth, param, priority, handle);
}

// Block until notified (simulates ulTaskNotifyTake with clear-on-exit).
inline uint32_t ulTaskNotifyTake(int /*clearOnExit*/,
                                 uint32_t /*ticksToWait*/) {
  auto *h = xTaskGetCurrentTaskHandle();
  std::unique_lock<std::mutex> lk(h->mtx);
  h->cv.wait(lk, [h] { return h->notifyCount > 0; });
  h->notifyCount--;
  return 1;
}

// Wake a task by incrementing its notification counter and signalling its
// condvar.
inline void xTaskNotify(TaskHandle_t handle, uint32_t /*value*/,
                        int /*action*/) {
  if (!handle)
    return;
  {
    std::lock_guard<std::mutex> lk(handle->mtx);
    handle->notifyCount++;
  }
  handle->cv.notify_one();
}

inline const char *pcTaskGetName(TaskHandle_t h) {
  if (!h)
    h = xTaskGetCurrentTaskHandle();
  return h ? h->name : "main";
}
inline void vTaskDelete(TaskHandle_t h) {
  if (h) {
    if (h->thread.joinable())
      h->thread.detach();
    delete h;
  }
}
inline unsigned int uxTaskGetStackHighWaterMark(TaskHandle_t) { return 2048; }
inline void vTaskList(char *) {}
// vTaskDelay's argument is in FreeRTOS ticks, and portTICK_PERIOD_MS
// (FreeRTOS.h) is this shim's one tick-rate definition. Convert through it, do
// not assume one tick is one millisecond. A no-op here made every firmware
// retry loop and every yield-every-N-rows call run in zero time.
inline void vTaskDelay(uint32_t ticks) {
  if (ticks == 0) {
    // FreeRTOS treats a zero delay as a yield, not a sleep.
    std::this_thread::yield();
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(
      static_cast<int64_t>(ticks) * portTICK_PERIOD_MS));
}
