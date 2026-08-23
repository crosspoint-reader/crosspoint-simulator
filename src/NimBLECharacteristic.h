#pragma once

// Shim for NimBLE's NimBLECharacteristic and its callback interface.
//
// The characteristic is a value plus a subscription plus a callback pointer.
// All three live in this object; SimBleGatt is a friend and owns the mutex
// that guards them, because the reader thread, the host thread and the
// activity thread all touch them.
//
// **Callback dispatch is never inline.** onWrite, onStatus and onSubscribe are
// called by SimBleGatt's host thread, never by the client op that caused them
// and never by indicate(). See docs/ble-shim.md, "Threading model".

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "NimBLEAttValue.h"
#include "NimBLEConnInfo.h"

// Real NimBLE spells these as a namespace of constants, so a firmware
// expression like `WRITE | NOTIFY | INDICATE` is an integer. Values are
// NimBLE's own, so a props number in an emitted `gatt` event means the same
// thing here as in a NimBLE header.
namespace NIMBLE_PROPERTY {
static constexpr uint32_t READ = 0x0001;
static constexpr uint32_t WRITE = 0x0008;
static constexpr uint32_t NOTIFY = 0x0010;
static constexpr uint32_t INDICATE = 0x0020;
}  // namespace NIMBLE_PROPERTY

class NimBLECharacteristic;
class SimBleGatt;

class NimBLECharacteristicCallbacks {
 public:
  virtual ~NimBLECharacteristicCallbacks() = default;

  // A central wrote this characteristic. Read the bytes with getValue().
  virtual void onWrite(NimBLECharacteristic *, NimBLEConnInfo &) {}

  // One indication finished. `code` is BLE_HS_EDONE when the peer confirmed.
  // Fires once per accepted indicate(), out of band, on the host thread.
  virtual void onStatus(NimBLECharacteristic *, NimBLEConnInfo &, int) {}

  // A central changed its subscription. bit0 is notify, bit1 is indicate.
  // Never fired for a disconnect -- NimBLE does not, so neither does this.
  virtual void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &,
                           uint16_t) {}
};

class NimBLECharacteristic {
 public:
  NimBLECharacteristic(const char *uuid, uint32_t properties)
      : m_uuid(uuid != nullptr ? uuid : ""), m_properties(properties) {}

  void setCallbacks(NimBLECharacteristicCallbacks *callbacks);

  // A copy of the last value a central wrote. Returned by value, same as the
  // real API.
  NimBLEAttValue getValue();

  // Puts `len` bytes into the connection's single pending indication slot.
  //
  // **True means the slot accepted the payload, not that the peer got it.**
  // The confirm arrives later through onStatus. A second call before that
  // confirm overwrites the first and still returns true -- measured on real
  // hardware, and the shim reproduces it rather than queueing politely. The
  // overwrite emits a `clobber` event so it is observable instead of silent.
  //
  // False means the real stack would have refused: stack down, no central
  // connected, nobody subscribed to this characteristic, this characteristic
  // cannot notify or indicate, or an empty payload.
  bool indicate(const uint8_t *data, size_t len);

  // Shim-only accessors. Not part of the NimBLE API; the emitted `gatt` event
  // and the self-test read them.
  const std::string &shimUuid() const { return m_uuid; }
  uint32_t shimProperties() const { return m_properties; }

 private:
  friend class SimBleGatt;

  std::string m_uuid;
  uint32_t m_properties = 0;
  NimBLECharacteristicCallbacks *m_callbacks = nullptr;
  // Guarded by SimBleGatt's mutex.
  std::vector<uint8_t> m_value;
  uint16_t m_subValue = 0;
};
