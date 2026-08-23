#pragma once

// Shim for NimBLE's NimBLEAttValue -- the value of one GATT attribute.
//
// The real class is a small heap buffer with a capacity policy and a pile of
// conversion helpers. The firmware uses three things from it: it takes one by
// value out of getValue(), then reads data() and size(). So that is what this
// is: a vector with those names on it.
//
// Returning by value is deliberate, not an oversight of the shim. The real
// getValue() copies too, and the firmware's transfer path is written around
// that copy costing one malloc/memcpy/free per chunk.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

class NimBLEAttValue {
 public:
  NimBLEAttValue() = default;
  NimBLEAttValue(const uint8_t *data, size_t len) {
    if (data != nullptr && len > 0) m_value.assign(data, data + len);
  }
  explicit NimBLEAttValue(std::vector<uint8_t> value)
      : m_value(std::move(value)) {}

  const uint8_t *data() const { return m_value.data(); }
  size_t size() const { return m_value.size(); }
  size_t length() const { return m_value.size(); }
  bool empty() const { return m_value.empty(); }

  const uint8_t *begin() const { return m_value.data(); }
  const uint8_t *end() const { return m_value.data() + m_value.size(); }
  uint8_t operator[](size_t i) const { return m_value[i]; }

 private:
  std::vector<uint8_t> m_value;
};
