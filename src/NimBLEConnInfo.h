#pragma once

// Shim for NimBLE's NimBLEConnInfo -- what one connection negotiated.
//
// Every firmware callback in the shim's contract takes one of these by
// reference. The real class wraps ble_gap_conn_desc and exposes peer address,
// bonding state and more; the firmware reads four numbers off it, so those
// four are what exist here.
//
// The interval, latency and timeout are the central's choices, not the
// peripheral's: the client sets them with the `connect` and `connparams` ops
// (docs/ble-shim.md, "Client to simulator"). Interval is in 1.25 ms units,
// timeout in 10 ms units -- the same units the firmware's logs assume.

#include <cstdint>

#include "host/ble_gap.h"

class NimBLEConnInfo {
 public:
  NimBLEConnInfo() = default;
  NimBLEConnInfo(uint16_t handle, uint16_t interval, uint16_t latency,
                 uint16_t timeout)
      : m_handle(handle), m_interval(interval), m_latency(latency),
        m_timeout(timeout) {}

  uint16_t getConnHandle() const { return m_handle; }
  uint16_t getConnInterval() const { return m_interval; }
  uint16_t getConnLatency() const { return m_latency; }
  uint16_t getConnTimeout() const { return m_timeout; }

 private:
  uint16_t m_handle = BLE_HS_CONN_HANDLE_NONE;
  uint16_t m_interval = 0;
  uint16_t m_latency = 0;
  uint16_t m_timeout = 0;
};
