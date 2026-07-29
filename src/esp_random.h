#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#if defined(__APPLE__)
#include <stdlib.h>
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#else
#error "Unsupported host OS for simulator random source"
#endif

inline void esp_fill_random(void *buffer, size_t length) {
  if (length == 0)
    return;
  if (!buffer)
    std::abort();
#if defined(__APPLE__)
  arc4random_buf(buffer, length);
#else
  auto *bytes = static_cast<uint8_t *>(buffer);
  while (length > 0) {
    const ssize_t count = getrandom(bytes, length, 0);
    if (count > 0) {
      bytes += count;
      length -= static_cast<size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      std::abort();
    }
  }
#endif
}

inline uint32_t esp_random() {
  uint32_t value = 0;
  esp_fill_random(&value, sizeof(value));
  return value;
}
