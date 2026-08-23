#pragma once

// Shim for NimBLE's host/ble_gap.h.
//
// A firmware may include this header directly for one function:
// ble_gap_conn_rssi(). No GAP wrapper exists on NimBLEServer or
// NimBLEConnInfo, so reading RSSI means reaching past the C++ API to the C
// host.
//
// The host error codes and the advertising interval defaults live here too.
// Real NimBLE spreads them over host/ble_hs.h and host/ble_gap.h; the shim
// mirrors the API surface, not the file layout, and NimBLEDevice.h includes
// this header so either include path sees every constant.
//
// Values match NimBLE's own so a firmware comparison means the same thing
// here as on a device.

#include <cstdint>

// 0xffff: "no connection". NimBLE host/ble_hs.h.
#define BLE_HS_CONN_HANDLE_NONE 0xffff

// Host return codes, in NimBLE's order. Only the three the firmware names are
// defined; adding more is a contract change, not a convenience.
#define BLE_HS_ENOTCONN 7
#define BLE_HS_ETIMEOUT 13
// The onStatus code meaning "the peer confirmed this indication".
#define BLE_HS_EDONE 14

// Advertising interval defaults, in 0.625 ms units. 0x0030 = 30 ms,
// 0x0060 = 60 ms. The host substitutes this pair when a peripheral asks for
// 0/0 bounds, which is what the firmware's disconnect path relies on.
#define BLE_GAP_ADV_FAST_INTERVAL1_MIN 0x0030
#define BLE_GAP_ADV_FAST_INTERVAL1_MAX 0x0060
#define BLE_GAP_ADV_FAST_INTERVAL1 BLE_GAP_ADV_FAST_INTERVAL1_MIN

#ifdef __cplusplus
extern "C" {
#endif

// Reads the RSSI of a live connection. Returns 0 on success and writes
// out_rssi; returns non-zero and leaves out_rssi alone otherwise.
//
// There is no radio here. The value is whatever the client last set with the
// `rssi` op (docs/ble-shim.md, "Client to simulator").
int ble_gap_conn_rssi(uint16_t conn_handle, int8_t *out_rssi);

#ifdef __cplusplus
}
#endif
