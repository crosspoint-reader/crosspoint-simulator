// Self-test for the NimBLE shim. No socket, no device, no firmware: it drives
// the shim's API the way a firmware BLE server does and asserts the behaviour
// the contract calls for.
//
// Build and run (one line, no continuations):
//   g++ -std=c++17 -Wall -Wextra -pthread -Isrc -o /tmp/sim_ble_gatt_selftest
//   src/NimBLEDevice.cpp src/SimBleGatt.cpp src/SimBleProtocol.cpp
//   tests/sim_ble_gatt_stub.cpp tests/sim_ble_gatt_selftest.cpp
//   /tmp/sim_ble_gatt_selftest
//
// SimBleProtocol.cpp is linked for portFromEnv() only. The transport itself is
// the stub in tests/sim_ble_gatt_stub.cpp: no socket is opened.
//
// **Never under src/.** See sim_ble_gatt_selftest.h.

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "NimBLEDevice.h"
#include "SimBleGatt.h"
#include "sim_ble_gatt_selftest.h"
#include "host/ble_gap.h"

namespace {

// UUIDs shaped like the firmware's: one service, a write-only channel, a
// write+notify+indicate command channel, an indicate-only status channel.
const char *kServiceUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0001";
const char *kPosUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0002";
const char *kCmdUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0003";
const char *kStatusUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0005";

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *what) {
  ++g_checks;
  if (!ok) ++g_failures;
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

std::thread::id g_mainThread;

struct Counters {
  int onWrite = 0;
  int onStatus = 0;
  int lastStatusCode = 0;
  int onSubscribe = 0;
  uint16_t lastSubValue = 0;
  int onConnect = 0;
  int onDisconnect = 0;
  int lastDisconnectReason = 0;
  int onConnParams = 0;
  int onMtu = 0;
  uint16_t lastMtu = 0;
  uint16_t lastHandle = 0;
  std::thread::id callbackThread{};
  std::vector<uint8_t> lastWrite;
};

Counters g_counters;

class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic,
               NimBLEConnInfo &) override {
    const NimBLEAttValue value = characteristic->getValue();
    g_counters.lastWrite.assign(value.data(), value.data() + value.size());
    ++g_counters.onWrite;
    g_counters.callbackThread = std::this_thread::get_id();
  }
  void onStatus(NimBLECharacteristic *, NimBLEConnInfo &, int code) override {
    g_counters.lastStatusCode = code;
    ++g_counters.onStatus;
    g_counters.callbackThread = std::this_thread::get_id();
  }
  void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &,
                   uint16_t subValue) override {
    g_counters.lastSubValue = subValue;
    ++g_counters.onSubscribe;
    g_counters.callbackThread = std::this_thread::get_id();
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &info) override {
    g_counters.lastHandle = info.getConnHandle();
    ++g_counters.onConnect;
    g_counters.callbackThread = std::this_thread::get_id();
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    g_counters.lastDisconnectReason = reason;
    ++g_counters.onDisconnect;
    g_counters.callbackThread = std::this_thread::get_id();
  }
  void onConnParamsUpdate(NimBLEConnInfo &) override {
    ++g_counters.onConnParams;
    g_counters.callbackThread = std::this_thread::get_id();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo &) override {
    g_counters.lastMtu = mtu;
    ++g_counters.onMtu;
    g_counters.callbackThread = std::this_thread::get_id();
  }
};

CmdCallbacks g_cmdCallbacks;
ServerCallbacks g_serverCallbacks;

SimBleEvent op(const char *name) {
  SimBleEvent event;
  event.op = name;
  return event;
}

bool sawEvent(const char *needle) {
  for (const std::string &line : simble_selftest::emitted()) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

void feedAndSettle(const SimBleEvent &event) {
  simble_selftest::feed(event);
  SimBleGatt::get().waitIdle();
}

}  // namespace

int main() {
  g_mainThread = std::this_thread::get_id();

  // --- build the table, same order the firmware does --------------------
  check(NimBLEDevice::init("sim-selftest"), "init returns true");
  check(NimBLEDevice::isInitialized(), "isInitialized after init");

  NimBLEServer *server = NimBLEDevice::createServer();
  check(server != nullptr, "createServer");
  server->setCallbacks(&g_serverCallbacks, false);

  NimBLEService *service = server->createService(kServiceUuid);
  NimBLECharacteristic *posChar =
      service->createCharacteristic(kPosUuid, NIMBLE_PROPERTY::WRITE);
  NimBLECharacteristic *cmdChar = service->createCharacteristic(
      kCmdUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY |
                    NIMBLE_PROPERTY::INDICATE);
  NimBLECharacteristic *statusChar =
      service->createCharacteristic(kStatusUuid, NIMBLE_PROPERTY::INDICATE);
  cmdChar->setCallbacks(&g_cmdCallbacks);
  check(server->start(), "server->start builds the table");
  check(sawEvent("\"ev\":\"gatt\""), "gatt event emitted");

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->enableScanResponse(true);
  advertising->setName("sim-selftest");
  check(advertising->start(), "advertising->start");
  check(sawEvent("\"ev\":\"advertising\",\"up\":true"),
        "advertising up event emitted");
  check(advertising->start(), "advertising->start again is a no-op success");

  // --- attach replays the current state to a late client -----------------
  // The transport synthesizes this op when a client connects. `stack up` was
  // emitted by init(), before any listener existed, so a client can never have
  // received it: the replay is the only way it learns.
  simble_selftest::clearEmitted();
  feedAndSettle(op("attach"));
  {
    const std::vector<std::string> replay = simble_selftest::emitted();
    const bool order =
        replay.size() == 3 &&
        replay[0].find("\"ev\":\"stack\",\"state\":\"up\"") != std::string::npos &&
        replay[1].find("\"ev\":\"gatt\"") != std::string::npos &&
        replay[2].find("\"ev\":\"advertising\",\"up\":true") !=
            std::string::npos;
    check(order, "attach replays stack up, then gatt, then advertising");
    if (!order) {
      for (const std::string &line : replay) printf("      got: %s\n", line.c_str());
    }
    check(g_counters.onConnect == 0,
          "attach fires no firmware callback of its own");
  }

  // --- 5: a write with no connection is refused, no callback -------------
  simble_selftest::clearEmitted();
  SimBleEvent write = op("write");
  write.uuid = kCmdUuid;
  write.data = {'x'};
  feedAndSettle(write);
  check(g_counters.onWrite == 0, "write with no connection fires no callback");
  check(sawEvent("\"ev\":\"error\""), "write with no connection emits error");

  // --- 1: callbacks run on the host thread, not the caller's -------------
  simble_selftest::clearEmitted();
  SimBleEvent connect = op("connect");
  connect.a = 0;   // mtu absent -> the pessimistic default
  connect.b = 24;  // 30 ms
  connect.c = 0;
  connect.d = 500;
  feedAndSettle(connect);
  check(g_counters.onConnect == 1, "connect fires onConnect once");
  check(g_counters.onMtu == 1 && g_counters.onConnParams == 1,
        "connect also fires onMTUChange and onConnParamsUpdate");
  check(g_counters.callbackThread != g_mainThread &&
            g_counters.callbackThread != std::thread::id(),
        "callback ran on a different thread than the caller");
  // --- 4: the client owns the MTU ---------------------------------------
  check(g_counters.lastMtu == SimBleGatt::kDefaultMtu,
        "MTU defaults to 23 when the client says nothing");
  check(sawEvent("\"ev\":\"advertising\",\"up\":false"),
        "advertising goes down on connect");

  // --- indicate with nobody subscribed returns TRUE ----------------------
  // Real NimBLE has no CCCD check: sendValue transmits and returns true
  // (NimBLECharacteristic.cpp:272-328). A shim returning false here sends the
  // firmware down its 40 x 25 ms retry loop instead of its 3000 ms confirm
  // wait -- different duration, different log line, different persistent
  // state. These two assertions used to claim the opposite; they are inverted
  // so a regression to it fails.
  const uint8_t payloadA[] = {'l', 'i', 'n', 'e', '-', 'A', '\n'};
  const uint8_t payloadB[] = {'l', 'i', 'n', 'e', '-', 'B', '\n'};
  simble_selftest::clearEmitted();
  g_counters.onStatus = 0;
  check(cmdChar->indicate(payloadA, sizeof(payloadA)),
        "indicate with nobody subscribed returns true");
  check(sawEvent("\"ev\":\"indicate\""),
        "an unsubscribed indication still goes out on the wire");
  SimBleGatt::get().waitIdle();
  check(g_counters.onStatus == 0,
        "nothing confirms an unsubscribed indication, even with auto_confirm on");
  check(statusChar->indicate(payloadA, sizeof(payloadA)),
        "indicate on an unsubscribed second characteristic returns true");
  check(sawEvent("\"ev\":\"clobber\""),
        "the second unsubscribed indication clobbered the first, slot and all");

  // Nothing connected is the other true: the peer loop never runs, so nothing
  // is built, nothing is sent, and the slot is left alone.
  SimBleEvent dropForNoConn = op("disconnect");
  dropForNoConn.a = 0x13;
  feedAndSettle(dropForNoConn);
  simble_selftest::clearEmitted();
  check(cmdChar->indicate(payloadA, sizeof(payloadA)),
        "indicate with nothing connected returns true");
  check(simble_selftest::emitted().empty(),
        "indicate with nothing connected emits nothing at all");
  feedAndSettle(connect);
  simble_selftest::clearEmitted();

  SimBleEvent subscribe = op("subscribe");
  subscribe.uuid = kCmdUuid;
  subscribe.a = 2;  // indications
  feedAndSettle(subscribe);
  check(g_counters.onSubscribe == 1 && g_counters.lastSubValue == 2,
        "subscribe fires onSubscribe with the subValue");

  // --- 2: the confirm is out of band ------------------------------------
  // auto_confirm on, delayed. indicate() must return with the confirm still
  // in the future.
  SimBleEvent autoConfirmSlow = op("auto_confirm");
  autoConfirmSlow.flag = true;
  autoConfirmSlow.a = 150;
  feedAndSettle(autoConfirmSlow);
  g_counters.onStatus = 0;
  const auto started = std::chrono::steady_clock::now();
  const bool accepted = cmdChar->indicate(payloadA, sizeof(payloadA));
  const int statusRightAfter = g_counters.onStatus;
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  check(accepted, "indicate returns true when the slot accepts");
  check(statusRightAfter == 0 && elapsed < 100,
        "indicate returned before any confirm arrived");
  SimBleGatt::get().waitIdle();
  check(g_counters.onStatus == 1, "the delayed confirm arrived later");
  check(g_counters.lastStatusCode == BLE_HS_EDONE,
        "onStatus code is BLE_HS_EDONE");

  // A withheld confirm never arrives. This is the firmware's timeout path.
  SimBleEvent autoConfirmOff = op("auto_confirm");
  autoConfirmOff.flag = false;
  feedAndSettle(autoConfirmOff);
  g_counters.onStatus = 0;
  check(cmdChar->indicate(payloadA, sizeof(payloadA)),
        "indicate accepted with auto_confirm off");
  SimBleGatt::get().waitIdle();
  check(g_counters.onStatus == 0, "a withheld confirm never fires onStatus");

  // --- 3: a second indicate clobbers the first ---------------------------
  simble_selftest::clearEmitted();
  check(cmdChar->indicate(payloadB, sizeof(payloadB)),
        "a second indicate before a confirm still returns true");
  check(sawEvent("\"ev\":\"clobber\""), "the second indicate emitted clobber");
  check(sawEvent("\"dropped_hex\":\"6c696e652d410a\""),
        "clobber names the dropped payload (line-A)");
  check(g_counters.onStatus == 0, "the clobber fired no confirm of its own");

  SimBleEvent confirm = op("confirm");
  confirm.uuid = kCmdUuid;
  feedAndSettle(confirm);
  check(g_counters.onStatus == 1,
        "a client confirm confirms the surviving payload, once");

  // --- a write on a live connection reaches the callback ------------------
  g_counters.onWrite = 0;
  SimBleEvent liveWrite = op("write");
  liveWrite.uuid = kCmdUuid;
  liveWrite.data = {'t', 'i', 'l', 'e', 's'};
  feedAndSettle(liveWrite);
  check(g_counters.onWrite == 1, "write on a live connection fires onWrite");
  check(g_counters.lastWrite == liveWrite.data,
        "getValue returns the bytes that were written");

  // A characteristic with no WRITE property is refused.
  simble_selftest::clearEmitted();
  SimBleEvent badWrite = op("write");
  badWrite.uuid = kStatusUuid;
  badWrite.data = {'x'};
  feedAndSettle(badWrite);
  check(sawEvent("\"ev\":\"error\""),
        "write to an indicate-only characteristic emits error");

  // --- rssi comes from the client ----------------------------------------
  SimBleEvent rssi = op("rssi");
  rssi.a = static_cast<uint32_t>(static_cast<int32_t>(-72));
  feedAndSettle(rssi);
  int8_t rssiOut = 0;
  check(ble_gap_conn_rssi(g_counters.lastHandle, &rssiOut) == 0 &&
            rssiOut == -72,
        "ble_gap_conn_rssi returns what the client set");

  // --- 5: disconnect clears the subscription, with no callback -----------
  simble_selftest::clearEmitted();
  g_counters.onSubscribe = 0;
  g_counters.onDisconnect = 0;
  SimBleEvent disconnect = op("disconnect");
  disconnect.a = 0x13;
  feedAndSettle(disconnect);
  check(g_counters.onDisconnect == 1 &&
            g_counters.lastDisconnectReason == 0x13,
        "disconnect fires onDisconnect with the reason");
  check(g_counters.onSubscribe == 0,
        "disconnect fires no unsubscribe callback, same as NimBLE");
  // The old assertion here was `!indicate(...)`, which only passed because of
  // the inversion above. Proving the subscription cleared now takes the
  // consequence rather than the return value: reconnect, indicate, and see
  // that no confirm comes back until a fresh subscribe.
  feedAndSettle(connect);
  g_counters.onStatus = 0;
  check(cmdChar->indicate(payloadA, sizeof(payloadA)),
        "indicate is accepted again on the new link");
  SimBleGatt::get().waitIdle();
  check(g_counters.onStatus == 0,
        "the subscription is gone after a disconnect: no confirm on the new link");
  SimBleEvent resub = op("subscribe");
  resub.uuid = kCmdUuid;
  resub.a = 2;
  feedAndSettle(resub);
  // auto_confirm was switched off earlier to test a withheld confirm. Back on,
  // so the positive half of this pair can actually confirm.
  SimBleEvent autoConfirmOn = op("auto_confirm");
  autoConfirmOn.flag = true;
  autoConfirmOn.a = 10;
  feedAndSettle(autoConfirmOn);
  g_counters.onStatus = 0;
  check(cmdChar->indicate(payloadB, sizeof(payloadB)),
        "indicate accepted after re-subscribing");
  SimBleGatt::get().waitIdle();
  check(g_counters.onStatus == 1,
        "a fresh subscribe restores the confirm, so the clear was real");
  SimBleEvent dropAgain = op("disconnect");
  dropAgain.a = 0x13;
  feedAndSettle(dropAgain);
  check(ble_gap_conn_rssi(g_counters.lastHandle, &rssiOut) != 0,
        "ble_gap_conn_rssi fails with no connection");
  check(posChar != nullptr, "the position characteristic outlives the link");

  // --- teardown ----------------------------------------------------------
  simble_selftest::clearEmitted();
  NimBLEDevice::deinit(true);
  check(!NimBLEDevice::isInitialized(), "deinit clears the stack");
  check(sawEvent("\"ev\":\"stack\",\"state\":\"down\""),
        "deinit emits stack down");

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  // Flush, then leave without running global destructors. A test binary must
  // report its verdict even when something else in the link is unhappy at
  // teardown; a hijacked main() that returned into a foreign destructor is what
  // taught that (see the header of this file).
  fflush(stdout);
  _exit(g_failures == 0 ? 0 : 1);
}
