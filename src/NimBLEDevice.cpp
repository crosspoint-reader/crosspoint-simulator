#include "NimBLEDevice.h"

#include "SimBleGatt.h"

// Every method here is a forwarder. The model, the threads and the wire
// protocol live in SimBleGatt; this file only exists so the firmware's
// #include <NimBLEDevice.h> compiles and links.

// --- NimBLEDevice -----------------------------------------------------------

bool NimBLEDevice::init(const char *deviceName) {
  return SimBleGatt::get().init(deviceName);
}

void NimBLEDevice::deinit(bool clearAll) {
  SimBleGatt::get().deinit(clearAll);
}

bool NimBLEDevice::isInitialized() { return SimBleGatt::get().initialized(); }

NimBLEServer *NimBLEDevice::createServer() { return SimBleGatt::get().server(); }

NimBLEAdvertising *NimBLEDevice::getAdvertising() {
  return SimBleGatt::get().advertising();
}

void NimBLEDevice::setSecurityAuth(bool bonding, bool mitm, bool sc) {
  SimBleGatt::get().setSecurityAuth(bonding, mitm, sc);
}

// --- NimBLEServer -----------------------------------------------------------

NimBLEService *NimBLEServer::createService(const char *uuid) {
  return SimBleGatt::get().createService(uuid);
}

void NimBLEServer::setCallbacks(NimBLEServerCallbacks *callbacks, bool) {
  // The delete flag is ignored: the shim never owns the pointer. The firmware
  // passes false and registers a static object, so nothing is lost.
  SimBleGatt::get().setServerCallbacks(callbacks);
}

bool NimBLEServer::start() { return SimBleGatt::get().startServer(); }

void NimBLEServer::updateConnParams(uint16_t handle, uint16_t minInterval,
                                    uint16_t maxInterval, uint16_t latency,
                                    uint16_t timeout) {
  SimBleGatt::get().requestConnParams(handle, minInterval, maxInterval, latency,
                                      timeout);
}

// --- NimBLEService ----------------------------------------------------------

NimBLECharacteristic *NimBLEService::createCharacteristic(const char *uuid,
                                                          uint32_t properties) {
  return SimBleGatt::get().createCharacteristic(this, uuid, properties);
}

bool NimBLEService::start() { return true; }

// --- NimBLECharacteristic ---------------------------------------------------

void NimBLECharacteristic::setCallbacks(
    NimBLECharacteristicCallbacks *callbacks) {
  SimBleGatt::get().setCharacteristicCallbacks(this, callbacks);
}

NimBLEAttValue NimBLECharacteristic::getValue() {
  return SimBleGatt::get().characteristicValue(this);
}

bool NimBLECharacteristic::indicate(const uint8_t *data, size_t len) {
  return SimBleGatt::get().indicate(this, data, len);
}

// --- NimBLEAdvertising ------------------------------------------------------

bool NimBLEAdvertising::start() { return SimBleGatt::get().advertisingStart(); }

void NimBLEAdvertising::stop() { SimBleGatt::get().advertisingStop(); }

void NimBLEAdvertising::setName(const char *name) {
  SimBleGatt::get().setAdvertisingName(name);
}

void NimBLEAdvertising::addServiceUUID(const char *uuid) {
  SimBleGatt::get().setAdvertisingServiceUuid(uuid);
}

void NimBLEAdvertising::enableScanResponse(bool enable) {
  SimBleGatt::get().setAdvertisingScanResponse(enable);
}

void NimBLEAdvertising::setMinInterval(uint16_t interval) {
  SimBleGatt::get().setAdvertisingMinInterval(interval);
}

void NimBLEAdvertising::setMaxInterval(uint16_t interval) {
  SimBleGatt::get().setAdvertisingMaxInterval(interval);
}

// --- host/ble_gap.h ---------------------------------------------------------

extern "C" int ble_gap_conn_rssi(uint16_t conn_handle, int8_t *out_rssi) {
  return SimBleGatt::get().connRssi(conn_handle, out_rssi);
}
