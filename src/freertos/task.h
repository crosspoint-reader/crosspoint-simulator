#pragma once
#include <cstdint>
#include <map>
#include <string>

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

// Registry of live tasks by name, so a task is created at most once per name.
//
// On real hardware a deep-sleep wake RESETS the chip, so setup() runs against a
// clean machine and exactly one render task ever exists. Where the simulator
// re-enters setup() in-process instead of restarting (iOS, which cannot exec),
// a second xTaskCreate for the same name would spawn a second render thread and
// orphan the first -- two threads writing one framebuffer. Deduping by name
// models the reset rather than working around the firmware, which is why this
// belongs in the shim and not in any caller.
inline std::map<std::string, SimTaskHandle *> &simTaskRegistry() {
  static std::map<std::string, SimTaskHandle *> registry;
  return registry;
}

// Create a real OS thread. The FreeRTOS task function signature is
// void(*)(void*).
inline BaseType_t xTaskCreate(void (*fn)(void *), const char *name,
                              uint32_t /*stackDepth*/, void *param,
                              BaseType_t /*priority*/, TaskHandle_t *handle) {
  const std::string key = name ? name : "sim-task";

  auto &registry = simTaskRegistry();
  const auto existing = registry.find(key);
  if (existing != registry.end()) {
    // Already running from a previous setup(). Hand back the live task; the
    // firmware's view ("my task exists") stays true.
    if (handle)
      *handle = existing->second;
    return 1; // pdPASS
  }

  auto *h = new SimTaskHandle();
  // name is a string literal at every call site; the registry owns the copy.
  h->name = name ? name : "sim-task";
  registry.emplace(key, h);
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
inline void vTaskDelay(int) {}
