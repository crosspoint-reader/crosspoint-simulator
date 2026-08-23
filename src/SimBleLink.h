#pragma once

// SimBleLink -- the transport seam of the simulator's fake BLE peripheral.
//
// FROZEN INTERFACE. Two agents share this file's consumers and neither may
// change it: the GATT/API side calls it, the socket side implements it. A
// change here breaks the other half silently, so a change is a report, not an
// edit.
//
// Split of duty:
//   - This class owns the socket, the listener, the reader thread and the
//     line framing. It never calls firmware code.
//   - The sink it invokes (setSink) is the GATT model's entry point. The sink
//     is called ON THE READER THREAD and must not run firmware callbacks
//     itself: it queues them for the host thread. See docs/ble-shim.md,
//     "Threading model".
//
// Wire format is one JSON object per line, UTF-8, newline delimited. The
// decode happens below this seam: a SimBleEvent is already parsed.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// One decoded client op. Deliberately flat: the ops carry at most four small
// integers, and a tagged union would buy nothing but ceremony.
//
// Field meaning per op (docs/ble-shim.md has the table):
//   connect     a=mtu, b=interval, c=latency, d=timeout
//   disconnect  a=reason
//   write       uuid, data, flag=response
//   subscribe   uuid, a=subValue
//   confirm     uuid
//   mtu         a=mtu
//   connparams  b=interval, c=latency, d=timeout
//   rssi        a=value (cast to int8_t by the consumer)
//   auto_confirm flag=enabled, a=delay_ms
struct SimBleEvent {
  std::string op, uuid;
  std::vector<uint8_t> data;
  uint32_t a = 0, b = 0, c = 0, d = 0;  // mtu/interval/latency/timeout/subValue
  bool flag = true;
};

class SimBleLink {
 public:
  static SimBleLink& get();

  // Binds a loopback TCP listener and starts the reader thread.
  // Returns false if port is 0 or the bind fails. A false return is not an
  // error the caller recovers from: it means the feature stays off and the
  // simulator behaves as it did before BLE existed.
  bool start(uint16_t port);

  // Closes the socket, joins the reader thread. Safe to call when not running.
  void stop();

  bool running() const;

  // Registers the decoded-op sink. Called on the reader thread, so the sink
  // must be cheap and must not block. Passing nullptr clears it.
  void setSink(void (*fn)(void*, const SimBleEvent&), void* ctx);

  // Writes one JSON line to the connected client. Thread safe: the activity
  // thread and the host thread both emit. A no-op when nothing is connected.
  void emit(const char* json);

 private:
  SimBleLink() = default;
  SimBleLink(const SimBleLink&) = delete;
  SimBleLink& operator=(const SimBleLink&) = delete;
};
