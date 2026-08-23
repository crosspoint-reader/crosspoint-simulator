#pragma once

// Shim for NimBLE-Arduino's NimBLEDevice.h.
//
// **Shim** means header-compatible fake: the same C++ API the firmware
// compiles against, implemented over a TCP line protocol instead of a radio.
// No NimBLE source is compiled. docs/ble-shim.md has the wire protocol, the
// threading model and what this cannot answer.
//
// Only the peripheral (GATT server) side exists. There is no NimBLEClient, no
// NimBLEScan and no central role, because the firmware has none.
//
// Every firmware callback runs on SimBleGatt's host thread. That is the whole
// point: the firmware is written around "the disconnect callback runs on the
// NimBLE host task, and the sync event it waits for is dispatched on that same
// task", and inline dispatch would make that class of deadlock impossible to
// reproduce.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "NimBLEAttValue.h"
#include "NimBLECharacteristic.h"
#include "NimBLEConnInfo.h"
#include "host/ble_gap.h"

class NimBLEServer;
class SimBleGatt;

class NimBLEServerCallbacks {
 public:
  virtual ~NimBLEServerCallbacks() = default;

  // A central connected. Advertising is already down by the time this runs:
  // NimBLE stops it for the duration of the connection and does not resume it.
  virtual void onConnect(NimBLEServer *, NimBLEConnInfo &) {}

  // The link dropped. `reason` is the client's `disconnect` reason field.
  virtual void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) {}

  // The central changed the connection parameters, or answered a request the
  // firmware made with updateConnParams().
  virtual void onConnParamsUpdate(NimBLEConnInfo &) {}

  // The central set the MTU. It drives the firmware's payload arithmetic, so
  // the client owns this number; the default is the pessimistic 23.
  virtual void onMTUChange(uint16_t, NimBLEConnInfo &) {}
};

class NimBLEService {
 public:
  explicit NimBLEService(const char *uuid)
      : m_uuid(uuid != nullptr ? uuid : "") {}

  NimBLECharacteristic *createCharacteristic(const char *uuid,
                                             uint32_t properties);

  // Deprecated in NimBLE 2.x and unused by the firmware: services start when
  // the server starts. Kept because the shim's contract names it. Always true.
  bool start();

  const std::string &shimUuid() const { return m_uuid; }

 private:
  friend class SimBleGatt;

  std::string m_uuid;
  std::vector<NimBLECharacteristic *> m_characteristics;
};

class NimBLEServer {
 public:
  NimBLEService *createService(const char *uuid);

  // `deleteCallbacks` is honoured to the extent the shim can: it never deletes
  // the pointer. The firmware passes false and registers a static object.
  void setCallbacks(NimBLEServerCallbacks *callbacks,
                    bool deleteCallbacks = true);

  // Builds the GATT table and emits the `gatt` event. False when the stack is
  // down. Not in the shim's original contract list -- the firmware calls it
  // (BlePositionServer.cpp:314) in place of the deprecated
  // NimBLEService::start().
  bool start();

  // Asks the central for new connection parameters. A request, not a command:
  // it emits `connparams_request` and fires no callback. The central answers
  // with a `connparams` op, which is what fires onConnParamsUpdate.
  void updateConnParams(uint16_t handle, uint16_t minInterval,
                        uint16_t maxInterval, uint16_t latency,
                        uint16_t timeout);

 private:
  friend class SimBleGatt;
  NimBLEServer() = default;
};

class NimBLEAdvertising {
 public:
  // True once advertising is up. A start() while already advertising is a
  // no-op success and emits nothing -- real NimBLE behaves that way
  // (NimBLEAdvertising.cpp:194-197), which is why the firmware's slow-interval
  // switch does stop() before start().
  bool start();
  void stop();

  void setName(const char *name);
  void addServiceUUID(const char *uuid);
  void enableScanResponse(bool enable);

  // Both in 0.625 ms units. 0 on both means "let the host pick", and the host
  // picks BLE_GAP_ADV_FAST_INTERVAL1.
  void setMinInterval(uint16_t interval);
  void setMaxInterval(uint16_t interval);

 private:
  friend class SimBleGatt;
  NimBLEAdvertising() = default;

  // Guarded by SimBleGatt's mutex.
  std::string m_name;
  std::string m_serviceUuid;
  bool m_scanResponse = false;
  uint16_t m_minInterval = 0;
  uint16_t m_maxInterval = 0;
};

class NimBLEDevice {
 public:
  // Starts the host thread and the transport reader thread, and emits
  // `stack up`. True even with no client and no listener: a simulator run
  // without BLE must behave exactly as it did before the shim existed.
  static bool init(const char *deviceName);

  // Stops advertising, emits `stack down`, joins both threads. `clearAll`
  // deletes the server, services and characteristics, same as the real API --
  // which is why the firmware nulls its own pointers before calling this.
  static void deinit(bool clearAll = false);

  static bool isInitialized();

  // One server per stack. Repeated calls return the same object.
  static NimBLEServer *createServer();

  // One advertising object per stack, created on first use. It outlives a
  // start()/stop() pair, so interval bounds set on it persist -- the firmware
  // depends on that.
  static NimBLEAdvertising *getAdvertising();

  // Recorded and otherwise ignored. There is no pairing in the shim, and the
  // firmware asks for none.
  static void setSecurityAuth(bool bonding, bool mitm, bool sc);
};
