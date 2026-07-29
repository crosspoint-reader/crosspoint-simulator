#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

#define MALLOC_CAP_EXEC (1 << 0)
#define MALLOC_CAP_32BIT (1 << 1)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT (1 << 12)

inline size_t heap_caps_get_largest_free_block(uint32_t) {
  return static_cast<size_t>(ESP.getMaxAllocHeap());
}

inline size_t heap_caps_get_free_size(uint32_t) {
  return static_cast<size_t>(ESP.getFreeHeap());
}
