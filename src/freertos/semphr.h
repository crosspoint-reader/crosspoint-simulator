#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "FreeRTOS.h"
#include "task.h"

// FreeRTOS hands out mutexes and binary semaphores through the same
// SemaphoreHandle_t. They are different objects with different rules, so the
// handle carries a tag and the shared entry points below dispatch on it.
enum class SimSemaphoreKind { Mutex, Binary };

struct SimSemaphoreBase {
  const SimSemaphoreKind kind;
  explicit SimSemaphoreBase(SimSemaphoreKind k) : kind(k) {}
};

// Use a real recursive mutex for the rendering semaphore.
struct SimMutex : SimSemaphoreBase {
  SimMutex() : SimSemaphoreBase(SimSemaphoreKind::Mutex) {}
  std::recursive_mutex mtx;
  // Track "holder" for xSemaphoreGetMutexHolder compatibility (not thread-safe,
  // good enough for simulator)
  TaskHandle_t holder = nullptr;
  uint16_t holdCount = 0;
};

// A real binary semaphore: one token, created empty. A take with no token
// waits, and reports failure when ticksToWait runs out. A give may come from
// any thread, including one that never took it.
struct SimBinarySemaphore : SimSemaphoreBase {
  SimBinarySemaphore() : SimSemaphoreBase(SimSemaphoreKind::Binary) {}
  std::mutex mtx;
  std::condition_variable cv;
  bool available = false;
};

typedef SimSemaphoreBase *SemaphoreHandle_t;

namespace sim_semaphore_detail {

// Single non-virtual base, so a tag-checked downcast is exact. Nothing frees a
// handle in this shim (there is no vSemaphoreDelete), so no base-pointer delete
// happens and the base needs no virtual destructor.
inline SimMutex *asMutex(SemaphoreHandle_t sem) {
  return static_cast<SimMutex *>(sem);
}
inline SimBinarySemaphore *asBinary(SemaphoreHandle_t sem) {
  return static_cast<SimBinarySemaphore *>(sem);
}

// FreeRTOS timeouts are in ticks. portTICK_PERIOD_MS (FreeRTOS.h) is the one
// tick-rate definition this shim has, so convert through it rather than
// assuming a tick is a millisecond.
inline std::chrono::milliseconds ticksToDuration(uint32_t ticks) {
  return std::chrono::milliseconds(static_cast<int64_t>(ticks) *
                                   portTICK_PERIOD_MS);
}

} // namespace sim_semaphore_detail

inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new SimMutex(); }

inline SemaphoreHandle_t xSemaphoreCreateBinary() {
  return new SimBinarySemaphore();
}

// Returns pdTRUE (true) on success, pdFALSE (false) when ticksToWait expired.
// Only the binary semaphore can fail: a mutex take waits as long as it must,
// which is what every caller of the recursive mutex already assumes.
inline bool xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticksToWait) {
  if (!sem)
    return true;
  if (sem->kind == SimSemaphoreKind::Binary) {
    auto *bin = sim_semaphore_detail::asBinary(sem);
    std::unique_lock<std::mutex> lk(bin->mtx);
    const auto hasToken = [bin] { return bin->available; };
    if (ticksToWait == portMAX_DELAY) {
      bin->cv.wait(lk, hasToken);
    } else if (!bin->cv.wait_for(
                   lk, sim_semaphore_detail::ticksToDuration(ticksToWait),
                   hasToken)) {
      // ticksToWait == 0 lands here too: wait_for tests the predicate once and
      // returns, which is the non-blocking "drain a stale give" poll.
      return false;
    }
    bin->available = false;
    return true;
  }
  auto *mtx = sim_semaphore_detail::asMutex(sem);
  mtx->mtx.lock();
  mtx->holder = xTaskGetCurrentTaskHandle();
  mtx->holdCount++;
  return true;
}

inline bool xSemaphoreGive(SemaphoreHandle_t sem) {
  if (!sem)
    return true;
  if (sem->kind == SimSemaphoreKind::Binary) {
    auto *bin = sim_semaphore_detail::asBinary(sem);
    bool wasEmpty;
    {
      std::lock_guard<std::mutex> lk(bin->mtx);
      wasEmpty = !bin->available;
      bin->available = true;
    }
    // Notify outside the lock: the woken thread would only block on it again.
    bin->cv.notify_one();
    // FreeRTOS reports pdFALSE for a give that overflows a full semaphore.
    return wasEmpty;
  }
  auto *mtx = sim_semaphore_detail::asMutex(sem);
  if (mtx->holdCount > 0) {
    mtx->holdCount--;
  }
  if (mtx->holdCount == 0) {
    mtx->holder = nullptr;
  }
  mtx->mtx.unlock();
  return true;
}

inline TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t sem) {
  if (!sem || sem->kind != SimSemaphoreKind::Mutex)
    return nullptr;
  return sim_semaphore_detail::asMutex(sem)->holder;
}

// xQueuePeek on a mutex: returns pdTRUE if the mutex is available (not taken).
// On a binary semaphore, "available" means a token is waiting to be taken.
inline int xQueuePeek(SemaphoreHandle_t sem, void *, uint32_t) {
  if (!sem)
    return pdTRUE;
  if (sem->kind == SimSemaphoreKind::Binary) {
    auto *bin = sim_semaphore_detail::asBinary(sem);
    std::lock_guard<std::mutex> lk(bin->mtx);
    return bin->available ? pdTRUE : pdFALSE;
  }
  auto *mtx = sim_semaphore_detail::asMutex(sem);
  bool locked = mtx->mtx.try_lock();
  if (locked) {
    mtx->mtx.unlock();
    return pdTRUE;
  }
  return pdFALSE;
}
