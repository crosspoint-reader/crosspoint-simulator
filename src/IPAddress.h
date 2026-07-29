#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "WString.h"

class IPAddress {
  static size_t checkedIndex(int index) {
    if (index < 0 || index >= 4)
      std::abort();
    return static_cast<size_t>(index);
  }

  uint8_t bytes_[4] = {0, 0, 0, 0};

 public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
      : bytes_{a, b, c, d} {}

  String toString() const {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u",
                  static_cast<unsigned>(bytes_[0]),
                  static_cast<unsigned>(bytes_[1]),
                  static_cast<unsigned>(bytes_[2]),
                  static_cast<unsigned>(bytes_[3]));
    return String(buffer);
  }

  uint8_t operator[](int index) const { return bytes_[checkedIndex(index)]; }
  uint8_t &operator[](int index) { return bytes_[checkedIndex(index)]; }
  bool operator==(const IPAddress &other) const {
    return std::memcmp(bytes_, other.bytes_, sizeof(bytes_)) == 0;
  }
  bool operator!=(const IPAddress &other) const { return !(*this == other); }
};
