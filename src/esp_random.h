#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>

inline uint32_t esp_random() { return std::random_device{}(); }

inline void esp_fill_random(void* buffer, size_t length) {
  auto* out = static_cast<uint8_t*>(buffer);
  while (length) {
    const uint32_t value = esp_random();
    const size_t count = length < sizeof(value) ? length : sizeof(value);
    std::memcpy(out, &value, count);
    out += count;
    length -= count;
  }
}
