#include "SimBleGatt.h"

#include <cstdio>

#include "NimBLEDevice.h"
#include "SimBleProtocol.h"

// JSON is hand-rolled on purpose: the shim emits seven event shapes and adding
// a JSON dependency to the simulator to write them would cost more than it
// saves. Every field is a number, a bool, a hex string or a UUID.
namespace {

std::string jsonEscape(const std::string &in) {
  std::string out;
  out.reserve(in.size() + 2);
  for (const char c : in) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
        out += buf;
      } else {
        out += c;
      }
    }
  }
  return out;
}

std::string toHex(const std::vector<uint8_t> &bytes) {
  static const char *digits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const uint8_t b : bytes) {
    out += digits[b >> 4];
    out += digits[b & 0x0f];
  }
  return out;
}

void emitLine(const std::string &json) { SimBleLink::get().emit(json.c_str()); }

}  // namespace

SimBleGatt &SimBleGatt::get() {
  static SimBleGatt instance;
  return instance;
}

// --- stack lifecycle --------------------------------------------------------

bool SimBleGatt::init(const char *deviceName) {
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Real NimBLE returns true for a second init. The firmware self-heals a
    // partial teardown by deiniting first anyway, so this path is a safety net,
    // not the normal route.
    if (m_initialized) return true;

    m_deviceName = deviceName != nullptr ? deviceName : "";
    m_initialized = true;
    m_advertisingUp = false;
    m_tableBuilt = false;
    clearConnectionLocked();
    m_rssi = 0;
    m_autoConfirm = true;
    m_autoConfirmDelayMs = kDefaultAutoConfirmDelayMs;
    m_queue.clear();
    m_hostRunning = true;
    m_hostThread = std::thread(&SimBleGatt::hostThreadMain, this);
  }

  // Sink first, listener second: a client op must not arrive before there is
  // somewhere to put it.
  SimBleLink::get().setSink(&SimBleGatt::sinkTrampoline, this);
  // A false return means the feature is off (no port set, or the bind failed).
  // That is not an init failure: a simulator run with no BLE client must behave
  // exactly as it did before the shim existed.
  // The port comes from CROSSPOINT_SIM_BLE_PORT, decoded by the transport's own
  // helper. One reader of that variable, not two.
  SimBleLink::get().start(crosspoint_simulator::ble::portFromEnv());

  emitLine("{\"ev\":\"stack\",\"state\":\"up\"}");
  return true;
}

void SimBleGatt::deinit(bool clearAll) {
  std::thread hostThread;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return;
    // Joining the host thread from the host thread is a self-join and would
    // abort. Real NimBLE deinit from the host task is equally broken; the
    // firmware never does it. Say so rather than crash.
    if (m_hostRunning && std::this_thread::get_id() == m_hostThreadId) {
      emitErrorLocked("deinit called from the host thread; ignored");
      return;
    }
  }

  advertisingStop();
  emitLine("{\"ev\":\"stack\",\"state\":\"down\"}");

  SimBleLink::get().setSink(nullptr, nullptr);
  SimBleLink::get().stop();

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hostRunning = false;
    hostThread = std::move(m_hostThread);
  }
  m_queueCv.notify_all();
  if (hostThread.joinable()) hostThread.join();

  std::lock_guard<std::mutex> lock(m_mutex);
  m_queue.clear();
  clearConnectionLocked();
  m_serverCallbacks = nullptr;
  m_initialized = false;
  m_tableBuilt = false;
  if (!clearAll) return;

  // clearAll deletes the table, same as the real API -- which is why the
  // firmware nulls its own characteristic pointers before calling this.
  for (NimBLECharacteristic *characteristic : m_characteristics) {
    delete characteristic;
  }
  m_characteristics.clear();
  for (NimBLEService *service : m_services) delete service;
  m_services.clear();
  delete m_server;
  m_server = nullptr;
  delete m_advertising;
  m_advertising = nullptr;
}

bool SimBleGatt::initialized() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_initialized;
}

void SimBleGatt::setSecurityAuth(bool, bool, bool) {
  // Recorded nowhere and acted on nowhere: there is no pairing here, and the
  // firmware asks for none (bonding, mitm and sc all false).
}

// --- the table --------------------------------------------------------------

NimBLEServer *SimBleGatt::server() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_server == nullptr) m_server = new NimBLEServer();
  return m_server;
}

NimBLEAdvertising *SimBleGatt::advertising() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_advertising == nullptr) m_advertising = new NimBLEAdvertising();
  return m_advertising;
}

NimBLEService *SimBleGatt::createService(const char *uuid) {
  std::lock_guard<std::mutex> lock(m_mutex);
  NimBLEService *service = new NimBLEService(uuid);
  m_services.push_back(service);
  return service;
}

NimBLECharacteristic *SimBleGatt::createCharacteristic(NimBLEService *service,
                                                       const char *uuid,
                                                       uint32_t properties) {
  std::lock_guard<std::mutex> lock(m_mutex);
  NimBLECharacteristic *characteristic =
      new NimBLECharacteristic(uuid, properties);
  m_characteristics.push_back(characteristic);
  if (service != nullptr) service->m_characteristics.push_back(characteristic);
  return characteristic;
}

void SimBleGatt::setServerCallbacks(NimBLEServerCallbacks *callbacks) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_serverCallbacks = callbacks;
}

void SimBleGatt::setCharacteristicCallbacks(
    NimBLECharacteristic *characteristic,
    NimBLECharacteristicCallbacks *callbacks) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (characteristic != nullptr) characteristic->m_callbacks = callbacks;
}

void SimBleGatt::setAdvertisingName(const char *name) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_advertising != nullptr) m_advertising->m_name = name != nullptr ? name : "";
}

void SimBleGatt::setAdvertisingServiceUuid(const char *uuid) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_advertising != nullptr) {
    m_advertising->m_serviceUuid = uuid != nullptr ? uuid : "";
  }
}

void SimBleGatt::setAdvertisingScanResponse(bool enable) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_advertising != nullptr) m_advertising->m_scanResponse = enable;
}

void SimBleGatt::setAdvertisingMinInterval(uint16_t interval) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_advertising != nullptr) m_advertising->m_minInterval = interval;
}

void SimBleGatt::setAdvertisingMaxInterval(uint16_t interval) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_advertising != nullptr) m_advertising->m_maxInterval = interval;
}

NimBLEAttValue
SimBleGatt::characteristicValue(NimBLECharacteristic *characteristic) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (characteristic == nullptr) return NimBLEAttValue();
  return NimBLEAttValue(characteristic->m_value);
}

bool SimBleGatt::startServer() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_initialized) return false;
  m_tableBuilt = true;
  emitGattTableLocked();
  return true;
}

void SimBleGatt::emitGattTableLocked() const {
  // One `gatt` line per service. The firmware builds one; a second would get
  // its own line rather than being folded into the first.
  for (const NimBLEService *service : m_services) {
    std::string line = "{\"ev\":\"gatt\",\"service\":\"";
    line += jsonEscape(service->m_uuid);
    line += "\",\"chars\":[";
    bool first = true;
    for (const NimBLECharacteristic *characteristic :
         service->m_characteristics) {
      if (!first) line += ",";
      first = false;
      line += "{\"uuid\":\"";
      line += jsonEscape(characteristic->m_uuid);
      line += "\",\"props\":";
      line += std::to_string(characteristic->m_properties);
      line += "}";
    }
    line += "]}";
    emitLine(line);
  }
}

// Answers the transport's synthetic `attach` op. `stack up` is emitted by
// init(), which is also what starts the listener, so no client can ever be
// connected in time to receive it -- the event is not racy, it is structurally
// undeliverable. Same for a `gatt` table built before the client showed up. A
// real central has no such problem: it scans and sees advertising.
//
// So a newly attached client is told what is true right now, in the order it
// would have heard it: the stack, then the table, then advertising. Nothing is
// replayed about a live connection, because a client that was not there for the
// connect is not the central that made it.
void SimBleGatt::replayStateLocked() const {
  emitLine("{\"ev\":\"stack\",\"state\":\"up\"}");
  if (m_tableBuilt) emitGattTableLocked();
  emitAdvertisingLocked();
}

// --- a live link ------------------------------------------------------------

void SimBleGatt::requestConnParams(uint16_t, uint16_t minInterval,
                                   uint16_t maxInterval, uint16_t latency,
                                   uint16_t timeout) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_initialized || !m_connected) {
    emitErrorLocked("updateConnParams with no connection");
    return;
  }
  // A request, not a change. Nothing moves until the central answers with a
  // `connparams` op, which is what fires onConnParamsUpdate.
  std::string line = "{\"ev\":\"connparams_request\",\"min\":";
  line += std::to_string(minInterval);
  line += ",\"max\":" + std::to_string(maxInterval);
  line += ",\"latency\":" + std::to_string(latency);
  line += ",\"timeout\":" + std::to_string(timeout);
  line += "}";
  emitLine(line);
}

bool SimBleGatt::indicate(NimBLECharacteristic *characteristic,
                          const uint8_t *data, size_t len) {
  bool clobbered = false;
  std::string clobberedUuid;
  std::vector<uint8_t> clobberedPayload;
  std::string uuid;
  std::vector<uint8_t> payload;
  bool autoConfirm = false;
  uint32_t autoConfirmDelayMs = 0;
  uint32_t seq = 0;

  bool subscribed = false;

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    // **Shim guards, not NimBLE behaviour.** Real NimBLE reaches
    // `NimBLEDevice::getServer()->getPeerDevices()` with no null check
    // (NimBLECharacteristic.cpp:296), so with the stack down or a null
    // characteristic it dereferences a null pointer rather than returning
    // anything. A fake must not crash where the real thing crashes, so these
    // two return false. Neither is reachable from a firmware that checks its own
    // initialised flag and its own pointers first.
    if (!m_initialized || characteristic == nullptr) return false;

    // **Real NimBLE refuses nothing here.** The firmware calls the
    // two-argument indicate(), so `connHandle` defaults to
    // BLE_HS_CONN_HANDLE_NONE (NimBLECharacteristic.h:60) and the call lands
    // in sendValue (NimBLECharacteristic.cpp:272-328). That function has no
    // connection check, no CCCD check and no property check. It loops over
    // `getPeerDevices()`; `rc` starts at 0 and only a real transmit error
    // moves it, so **it returns true**.
    //
    // Nothing connected: the loop body never runs, nothing is built and
    // nothing is sent, and `rc` is still 0 at `done:`. So: true, and the slot
    // is untouched. Filling it would let a later connect-then-confirm confirm
    // a payload from before the link existed.
    //
    // A firmware built against real NimBLE puts the same fact in one line:
    // "indicate() succeeds into an empty room".
    if (!m_connected) return true;

    // Connected: from here real NimBLE transmits regardless of subscription
    // and regardless of properties. ble_gatts_indicate_custom does no CCCD
    // lookup -- it calls ble_att_clt_tx_indicate unconditionally and records
    // the pending handle (ble_gattc.c:4922-4936). Its only refusals are out of
    // memory and, under BLE_GATT_CACHING, an unaware peer. So an unsubscribed
    // peer gets a real indication PDU it will drop, and the slot is genuinely
    // taken -- which is exactly why a firmware waiting on a confirm waits out its
    // whole timeout instead of failing fast.
    //
    // Whether anybody subscribed decides one thing only: whether a confirm can
    // ever come back. `auto_confirm` is the client saying "I confirm what I
    // receive", and a client with the CCCD off receives nothing to confirm.
    subscribed = characteristic->m_subValue != 0;

    // Empty payload is a different operation in real NimBLE, not a refusal:
    // sendValue falls through to ble_gatts_chr_updated
    // (NimBLECharacteristic.cpp:317-319), which pushes the characteristic's
    // stored value to whoever subscribed, and still returns true. The shim
    // does not model the stored-value push, so it returns true and emits
    // nothing. Unreachable from a firmware that never indicates zero bytes.
    if (data == nullptr || len == 0) return true;

    // One slot per connection, not per characteristic. That is what the real
    // host does, and a firmware with two indicating characteristics feels it:
    // one channel has to park a line while the other holds the slot.
    //
    // An overwrite is what real hardware does. 18 back-to-back indicate()
    // calls all returned true and the peer saw the first and the last, so a
    // second call before a confirm takes the slot and the previous payload is
    // gone. Not queued -- queueing would hide the bug this shim exists to
    // reproduce.
    clobbered = m_pending;
    if (clobbered) {
      clobberedUuid = m_pendingUuid;
      clobberedPayload = m_pendingPayload;
    }
    m_pending = true;
    m_pendingUuid = characteristic->m_uuid;
    m_pendingPayload.assign(data, data + len);
    ++m_pendingSeq;

    uuid = m_pendingUuid;
    payload = m_pendingPayload;
    seq = m_pendingSeq;
    // No subscriber, no confirm -- ever. See the reasoning above: this is what
    // reproduces the firmware's confirm timeout instead of its retry loop.
    autoConfirm = m_autoConfirm && subscribed;
    autoConfirmDelayMs = m_autoConfirmDelayMs;
  }

  if (clobbered) {
    std::string line = "{\"ev\":\"clobber\",\"uuid\":\"";
    line += jsonEscape(clobberedUuid);
    line += "\",\"dropped_hex\":\"" + toHex(clobberedPayload) + "\"}";
    emitLine(line);
  }
  std::string line = "{\"ev\":\"indicate\",\"uuid\":\"";
  line += jsonEscape(uuid);
  line += "\",\"hex\":\"" + toHex(payload) + "\"}";
  emitLine(line);

  if (autoConfirm) {
    // The confirm is out of band even when the shim generates it: it goes
    // through the host thread's queue with a delay, so indicate() has always
    // returned before onStatus can run.
    HostEvent event;
    event.kind = HostEvent::Kind::Confirm;
    event.uuid = uuid;
    event.seq = seq;
    event.flag = true;  // shim-generated, so a stale one is dropped silently
    event.due = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(autoConfirmDelayMs);
    enqueue(std::move(event));
  }
  return true;
}

bool SimBleGatt::advertisingStart() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_initialized) return false;
  // A no-op success while already advertising, same as real NimBLE
  // (NimBLEAdvertising.cpp:194-197). Nothing is emitted, because nothing
  // changed -- which is why the firmware's slow-interval switch stops first.
  if (m_advertisingUp) return true;
  m_advertisingUp = true;
  emitAdvertisingLocked();
  return true;
}

void SimBleGatt::advertisingStop() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_advertisingUp) return;
  m_advertisingUp = false;
  emitAdvertisingLocked();
}

int SimBleGatt::connRssi(uint16_t handle, int8_t *out) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_connected || handle != m_connHandle) return BLE_HS_ENOTCONN;
  if (out != nullptr) *out = m_rssi;
  return 0;
}

// --- the reader thread ------------------------------------------------------

void SimBleGatt::sinkTrampoline(void *ctx, const SimBleEvent &event) {
  static_cast<SimBleGatt *>(ctx)->onReaderEvent(event);
}

void SimBleGatt::onReaderEvent(const SimBleEvent &event) {
  // Runs on the reader thread. Enqueue only -- not one firmware callback, and
  // not one state change, so ordering is whatever the client sent and nothing
  // races the host thread.
  HostEvent out;
  out.uuid = event.uuid;
  out.op = event.op;
  out.data = event.data;
  out.a = event.a;
  out.b = event.b;
  out.c = event.c;
  out.d = event.d;
  out.flag = event.flag;

  if (event.op == "connect") {
    out.kind = HostEvent::Kind::Connect;
  } else if (event.op == "disconnect") {
    out.kind = HostEvent::Kind::Disconnect;
  } else if (event.op == "write") {
    out.kind = HostEvent::Kind::Write;
  } else if (event.op == "subscribe") {
    out.kind = HostEvent::Kind::Subscribe;
  } else if (event.op == "confirm") {
    out.kind = HostEvent::Kind::Confirm;
    out.flag = false;  // client-driven: confirms whatever is pending
  } else if (event.op == "mtu") {
    out.kind = HostEvent::Kind::Mtu;
  } else if (event.op == "connparams") {
    out.kind = HostEvent::Kind::ConnParams;
  } else if (event.op == "rssi") {
    out.kind = HostEvent::Kind::Rssi;
  } else if (event.op == "auto_confirm") {
    out.kind = HostEvent::Kind::AutoConfirm;
  } else if (event.op == "attach") {
    // Synthesized by the transport on accept, never sent by a client.
    out.kind = HostEvent::Kind::Attach;
  } else {
    out.kind = HostEvent::Kind::Unknown;
  }
  enqueue(std::move(out));
}

void SimBleGatt::enqueue(HostEvent event) {
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hostRunning) return;
    m_queue.push_back(std::move(event));
  }
  m_queueCv.notify_all();
}

// --- the host thread --------------------------------------------------------

void SimBleGatt::hostThreadMain() {
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hostThreadId = std::this_thread::get_id();
  }

  std::unique_lock<std::mutex> lock(m_mutex);
  while (m_hostRunning) {
    const auto now = std::chrono::steady_clock::now();
    // First event that is due. Events with a future due time (an auto-confirm
    // delay) do not block the ones behind them -- in a real stack the confirm
    // genuinely arrives late while other traffic keeps flowing.
    auto ready = m_queue.end();
    auto earliest = std::chrono::steady_clock::time_point::max();
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
      if (it->due <= now) {
        ready = it;
        break;
      }
      if (it->due < earliest) earliest = it->due;
    }

    if (ready == m_queue.end()) {
      if (earliest == std::chrono::steady_clock::time_point::max()) {
        m_idleCv.notify_all();
        m_queueCv.wait(lock);
      } else {
        m_queueCv.wait_until(lock, earliest);
      }
      continue;
    }

    HostEvent event = std::move(*ready);
    m_queue.erase(ready);
    m_dispatching = true;
    lock.unlock();
    // The mutex is down for the whole dispatch. Every callback below calls
    // back into this object -- the firmware's onDisconnect calls
    // advertising->start() -- and holding it would deadlock on the first one.
    dispatch(event);
    lock.lock();
    m_dispatching = false;
    m_idleCv.notify_all();
  }
  m_idleCv.notify_all();
}

void SimBleGatt::waitIdle() {
  std::unique_lock<std::mutex> lock(m_mutex);
  m_idleCv.wait(lock, [this] {
    return (m_queue.empty() && !m_dispatching) || !m_hostRunning;
  });
}

void SimBleGatt::dispatch(HostEvent &event) {
  // Snapshot what the callback needs under the lock, drop the lock, call.
  NimBLEServerCallbacks *serverCallbacks = nullptr;
  NimBLECharacteristicCallbacks *charCallbacks = nullptr;
  NimBLECharacteristic *characteristic = nullptr;
  NimBLEServer *serverObject = nullptr;
  NimBLEConnInfo info;
  int intArg = 0;
  uint16_t shortArg = 0;
  bool alsoMtu = false;
  bool alsoConnParams = false;

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
      emitErrorLocked("client op while the stack is down: " + event.op);
      return;
    }
    serverObject = m_server;

    switch (event.kind) {
    case HostEvent::Kind::Connect: {
      if (m_connected) {
        emitErrorLocked("connect while already connected");
        return;
      }
      m_connected = true;
      m_connHandle = m_nextConnHandle++;
      if (m_nextConnHandle == BLE_HS_CONN_HANDLE_NONE) m_nextConnHandle = 1;
      m_mtu = event.a != 0 ? static_cast<uint16_t>(event.a) : kDefaultMtu;
      m_interval =
          event.b != 0 ? static_cast<uint16_t>(event.b) : kDefaultInterval;
      m_latency = static_cast<uint16_t>(event.c);
      m_timeout = static_cast<uint16_t>(event.d);
      // Advertising stops on connect and the shim does not resume it, because
      // NimBLE does not. The firmware restarts it from onDisconnect.
      if (m_advertisingUp) {
        m_advertisingUp = false;
        emitAdvertisingLocked();
      }
      serverCallbacks = m_serverCallbacks;
      info = connInfoLocked();
      shortArg = m_mtu;
      alsoMtu = true;
      alsoConnParams = true;
      break;
    }
    case HostEvent::Kind::Disconnect: {
      if (!m_connected) {
        emitErrorLocked("disconnect with no connection");
        return;
      }
      // The dying connection's info, as the real callback gets it.
      info = connInfoLocked();
      intArg = static_cast<int>(event.a);
      serverCallbacks = m_serverCallbacks;
      clearConnectionLocked();
      break;
    }
    case HostEvent::Kind::Write: {
      if (!m_connected) {
        emitErrorLocked("write with no connection");
        return;
      }
      characteristic = findCharLocked(event.uuid);
      if (characteristic == nullptr) {
        emitErrorLocked("write to unknown characteristic " + event.uuid);
        return;
      }
      if ((characteristic->m_properties & NIMBLE_PROPERTY::WRITE) == 0) {
        emitErrorLocked("write to a characteristic that is not writable " +
                        event.uuid);
        return;
      }
      characteristic->m_value = event.data;
      charCallbacks = characteristic->m_callbacks;
      info = connInfoLocked();
      break;
    }
    case HostEvent::Kind::Subscribe: {
      if (!m_connected) {
        emitErrorLocked("subscribe with no connection");
        return;
      }
      characteristic = findCharLocked(event.uuid);
      if (characteristic == nullptr) {
        emitErrorLocked("subscribe to unknown characteristic " + event.uuid);
        return;
      }
      characteristic->m_subValue = static_cast<uint16_t>(event.a);
      shortArg = characteristic->m_subValue;
      charCallbacks = characteristic->m_callbacks;
      info = connInfoLocked();
      break;
    }
    case HostEvent::Kind::Confirm: {
      // A shim-generated confirm whose payload has since been clobbered is
      // dropped without a word: it is a timer, not something a client asked
      // for. One clobbered burst therefore yields one confirm, which is what
      // hardware showed.
      if (event.flag && (!m_pending || event.seq != m_pendingSeq)) return;
      if (!m_pending) {
        emitErrorLocked("confirm with no indication pending");
        return;
      }
      if (!event.uuid.empty() && event.uuid != m_pendingUuid) {
        emitErrorLocked("confirm for " + event.uuid + " but " + m_pendingUuid +
                        " is pending");
        return;
      }
      characteristic = findCharLocked(m_pendingUuid);
      m_pending = false;
      m_pendingUuid.clear();
      m_pendingPayload.clear();
      if (characteristic == nullptr) return;
      charCallbacks = characteristic->m_callbacks;
      intArg = BLE_HS_EDONE;
      info = connInfoLocked();
      break;
    }
    case HostEvent::Kind::Mtu: {
      if (!m_connected) {
        emitErrorLocked("mtu with no connection");
        return;
      }
      m_mtu = event.a != 0 ? static_cast<uint16_t>(event.a) : kDefaultMtu;
      shortArg = m_mtu;
      serverCallbacks = m_serverCallbacks;
      info = connInfoLocked();
      alsoMtu = true;
      break;
    }
    case HostEvent::Kind::ConnParams: {
      if (!m_connected) {
        emitErrorLocked("connparams with no connection");
        return;
      }
      if (event.b != 0) m_interval = static_cast<uint16_t>(event.b);
      m_latency = static_cast<uint16_t>(event.c);
      m_timeout = static_cast<uint16_t>(event.d);
      serverCallbacks = m_serverCallbacks;
      info = connInfoLocked();
      alsoConnParams = true;
      break;
    }
    case HostEvent::Kind::Rssi:
      m_rssi = static_cast<int8_t>(static_cast<int32_t>(event.a));
      return;
    case HostEvent::Kind::AutoConfirm:
      m_autoConfirm = event.flag;
      m_autoConfirmDelayMs =
          event.a != 0 ? event.a : kDefaultAutoConfirmDelayMs;
      return;
    case HostEvent::Kind::Attach:
      replayStateLocked();
      return;
    case HostEvent::Kind::Unknown:
      emitErrorLocked("unknown op " + event.op);
      return;
    }
  }

  // Lock is down. Everything below is firmware code.
  switch (event.kind) {
  case HostEvent::Kind::Connect:
    if (serverCallbacks != nullptr) {
      serverCallbacks->onConnect(serverObject, info);
      if (alsoMtu) serverCallbacks->onMTUChange(shortArg, info);
      if (alsoConnParams) serverCallbacks->onConnParamsUpdate(info);
    }
    break;
  case HostEvent::Kind::Disconnect:
    if (serverCallbacks != nullptr) {
      serverCallbacks->onDisconnect(serverObject, info, intArg);
    }
    break;
  case HostEvent::Kind::Write:
    if (charCallbacks != nullptr) charCallbacks->onWrite(characteristic, info);
    break;
  case HostEvent::Kind::Subscribe:
    if (charCallbacks != nullptr) {
      charCallbacks->onSubscribe(characteristic, info, shortArg);
    }
    break;
  case HostEvent::Kind::Confirm:
    if (charCallbacks != nullptr) {
      charCallbacks->onStatus(characteristic, info, intArg);
    }
    break;
  case HostEvent::Kind::Mtu:
    if (serverCallbacks != nullptr) {
      serverCallbacks->onMTUChange(shortArg, info);
    }
    break;
  case HostEvent::Kind::ConnParams:
    if (serverCallbacks != nullptr) serverCallbacks->onConnParamsUpdate(info);
    break;
  case HostEvent::Kind::Rssi:
  case HostEvent::Kind::AutoConfirm:
  case HostEvent::Kind::Attach:
  case HostEvent::Kind::Unknown:
    break;
  }
}

// --- lock-held helpers ------------------------------------------------------

NimBLECharacteristic *
SimBleGatt::findCharLocked(const std::string &uuid) const {
  for (NimBLECharacteristic *characteristic : m_characteristics) {
    if (characteristic->m_uuid == uuid) return characteristic;
  }
  return nullptr;
}

NimBLEConnInfo SimBleGatt::connInfoLocked() const {
  return NimBLEConnInfo(m_connHandle, m_interval, m_latency, m_timeout);
}

void SimBleGatt::clearConnectionLocked() {
  m_connected = false;
  m_connHandle = BLE_HS_CONN_HANDLE_NONE;
  m_mtu = kDefaultMtu;
  m_interval = 0;
  m_latency = 0;
  m_timeout = 0;
  // A subscription belongs to a connection, and NimBLE fires no unsubscribe
  // callback when the link drops. The shim clears it here, silently, because a
  // firmware written against real NimBLE relies on there being no callback.
  for (NimBLECharacteristic *characteristic : m_characteristics) {
    characteristic->m_subValue = 0;
  }
  // The pending indication belonged to the peer that just left.
  m_pending = false;
  m_pendingUuid.clear();
  m_pendingPayload.clear();
}

void SimBleGatt::emitAdvertisingLocked() const {
  std::string line = "{\"ev\":\"advertising\",\"up\":";
  line += m_advertisingUp ? "true" : "false";
  const uint16_t minInterval =
      m_advertising != nullptr ? m_advertising->m_minInterval : 0;
  const uint16_t maxInterval =
      m_advertising != nullptr ? m_advertising->m_maxInterval : 0;
  // 0/0 means "let the host pick", and the host picks the fast pair. Report
  // what the radio would actually use, not the sentinel.
  line += ",\"interval_min\":" +
          std::to_string(minInterval != 0 ? minInterval
                                          : BLE_GAP_ADV_FAST_INTERVAL1_MIN);
  line += ",\"interval_max\":" +
          std::to_string(maxInterval != 0 ? maxInterval
                                          : BLE_GAP_ADV_FAST_INTERVAL1_MAX);
  line += ",\"name\":\"";
  line += jsonEscape(m_advertising != nullptr ? m_advertising->m_name
                                              : m_deviceName);
  line += "\",\"service\":\"";
  line += jsonEscape(m_advertising != nullptr ? m_advertising->m_serviceUuid
                                              : std::string());
  line += "\"}";
  emitLine(line);
}

void SimBleGatt::emitErrorLocked(const std::string &message) const {
  emitLine("{\"ev\":\"error\",\"msg\":\"" + jsonEscape(message) + "\"}");
}
