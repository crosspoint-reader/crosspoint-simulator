#pragma once

// SimBleGatt -- the GATT model and the host thread behind the NimBLE shim.
//
// The NimBLE* classes are thin wrappers; every decision lives here. Three
// threads meet in this object and the split matters:
//
//   - The **reader thread** (SimBleLink's) delivers decoded client ops to
//     onReaderEvent(). It only enqueues. It never calls firmware code.
//   - The **host thread** (this class's) drains that queue and dispatches
//     every firmware callback. It is the stand-in for the NimBLE host task.
//   - The **activity thread** (the firmware's own) calls indicate(),
//     advertisingStart() and the rest of the API. Those return without ever
//     running a callback.
//
// One mutex guards all state, including the fields inside the NimBLE* wrapper
// objects (this class is their friend). The mutex is **never held while a
// firmware callback runs**, because the callbacks call straight back in --
// the firmware's disconnect handler calls advertising->start().
//
// docs/ble-shim.md is the contract this implements.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "NimBLEAttValue.h"
#include "NimBLECharacteristic.h"
#include "NimBLEConnInfo.h"
#include "SimBleLink.h"

class NimBLEAdvertising;
class NimBLEServer;
class NimBLEServerCallbacks;
class NimBLEService;

class SimBleGatt {
 public:
  // The pessimistic ATT default. A client that says nothing gets this, and the
  // firmware's chunk arithmetic is then the arithmetic a fresh link runs.
  static constexpr uint16_t kDefaultMtu = 23;
  // 1.25 ms units: 30 ms, a plausible negotiated interval.
  static constexpr uint16_t kDefaultInterval = 24;
  static constexpr uint32_t kDefaultAutoConfirmDelayMs = 10;

  static SimBleGatt &get();

  // --- stack lifecycle, called from the activity thread -------------------
  bool init(const char *deviceName);
  void deinit(bool clearAll);
  bool initialized() const;
  void setSecurityAuth(bool bonding, bool mitm, bool sc);

  // --- the objects the firmware builds its table out of -------------------
  NimBLEServer *server();
  NimBLEAdvertising *advertising();
  NimBLEService *createService(const char *uuid);
  NimBLECharacteristic *createCharacteristic(NimBLEService *service,
                                             const char *uuid,
                                             uint32_t properties);
  void setServerCallbacks(NimBLEServerCallbacks *callbacks);
  void setCharacteristicCallbacks(NimBLECharacteristic *characteristic,
                                  NimBLECharacteristicCallbacks *callbacks);
  bool startServer();

  // Everything a wrapper object stores goes through here, because the same
  // mutex guards it. Advertising bounds in particular are written from two
  // threads: the activity thread sets them on the slow-interval switch, and
  // the firmware's onDisconnect resets them on the host thread.
  void setAdvertisingName(const char *name);
  void setAdvertisingServiceUuid(const char *uuid);
  void setAdvertisingScanResponse(bool enable);
  void setAdvertisingMinInterval(uint16_t interval);
  void setAdvertisingMaxInterval(uint16_t interval);
  NimBLEAttValue characteristicValue(NimBLECharacteristic *characteristic);

  // --- things the firmware does to a live link ----------------------------
  void requestConnParams(uint16_t handle, uint16_t minInterval,
                         uint16_t maxInterval, uint16_t latency,
                         uint16_t timeout);
  bool indicate(NimBLECharacteristic *characteristic, const uint8_t *data,
                size_t len);
  bool advertisingStart();
  void advertisingStop();
  int connRssi(uint16_t handle, int8_t *out) const;

  // Blocks until the host thread's queue is empty and it is not mid-dispatch,
  // including events not yet due (an auto-confirm delay is waited out).
  //
  // Not a NimBLE API. It exists so a test can assert that a callback did *not*
  // fire without racing the host thread, and so a test can wait for one that
  // should. The firmware never calls it.
  void waitIdle();

 private:
  // One unit of work for the host thread. Client ops arrive as these, and so
  // does the shim's own delayed auto-confirm.
  struct HostEvent {
    enum class Kind {
      Connect,
      Disconnect,
      Write,
      Subscribe,
      Confirm,
      Mtu,
      ConnParams,
      Rssi,
      AutoConfirm,
      Unknown
    };
    Kind kind = Kind::Unknown;
    std::string uuid;
    std::string op;  // Unknown only, for the error message
    std::vector<uint8_t> data;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    bool flag = false;
    // Confirm only: which pending payload this confirm is for. A confirm the
    // shim generated for a payload that has since been clobbered is dropped
    // silently rather than confirming its replacement.
    uint32_t seq = 0;
    std::chrono::steady_clock::time_point due{};
  };

  SimBleGatt() = default;
  SimBleGatt(const SimBleGatt &) = delete;
  SimBleGatt &operator=(const SimBleGatt &) = delete;

  static void sinkTrampoline(void *ctx, const SimBleEvent &event);
  void onReaderEvent(const SimBleEvent &event);
  void enqueue(HostEvent event);
  void hostThreadMain();
  void dispatch(HostEvent &event);

  // All of these want m_mutex held.
  NimBLECharacteristic *findCharLocked(const std::string &uuid) const;
  NimBLEConnInfo connInfoLocked() const;
  void clearConnectionLocked();
  void emitAdvertisingLocked() const;
  void emitErrorLocked(const std::string &message) const;

  mutable std::mutex m_mutex;
  std::condition_variable m_queueCv;
  std::condition_variable m_idleCv;
  std::deque<HostEvent> m_queue;
  std::thread m_hostThread;
  std::thread::id m_hostThreadId{};
  bool m_hostRunning = false;
  bool m_dispatching = false;

  bool m_initialized = false;
  std::string m_deviceName;

  NimBLEServer *m_server = nullptr;
  NimBLEAdvertising *m_advertising = nullptr;
  std::vector<NimBLEService *> m_services;
  std::vector<NimBLECharacteristic *> m_characteristics;
  NimBLEServerCallbacks *m_serverCallbacks = nullptr;
  bool m_advertisingUp = false;

  // Connection state. Owned by the client: it sets the MTU and the timing.
  bool m_connected = false;
  uint16_t m_connHandle = BLE_HS_CONN_HANDLE_NONE;
  uint16_t m_nextConnHandle = 1;
  uint16_t m_mtu = kDefaultMtu;
  uint16_t m_interval = 0;
  uint16_t m_latency = 0;
  uint16_t m_timeout = 0;
  int8_t m_rssi = 0;

  // The connection's single indication slot, shared by every characteristic.
  bool m_pending = false;
  std::string m_pendingUuid;
  std::vector<uint8_t> m_pendingPayload;
  uint32_t m_pendingSeq = 0;

  bool m_autoConfirm = true;
  uint32_t m_autoConfirmDelayMs = kDefaultAutoConfirmDelayMs;
};
