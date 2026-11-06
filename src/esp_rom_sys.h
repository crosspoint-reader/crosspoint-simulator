#pragma once

#include <cstdarg>
#include <cstdio>

#include "SimLog.h"

inline int esp_rom_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  const int written = vfprintf(stderr, format, args);
  va_end(args);
  char line[256];
  const int length = vsnprintf(line, sizeof(line), format, copy);
  va_end(copy);
  if (length > 0)
    sim_log::write(line, static_cast<size_t>(length) < sizeof(line)
                             ? static_cast<size_t>(length)
                             : sizeof(line) - 1);
  return written;
}
