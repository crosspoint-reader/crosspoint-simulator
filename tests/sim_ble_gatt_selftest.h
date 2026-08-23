#pragma once

// Test-only seam for the NimBLE shim's GATT self-test.
//
// sim_ble_gatt_stub.cpp implements SimBleLink over these two calls: feed()
// plays the reader thread, emitted() captures what would have gone down the
// socket. That keeps the GATT model provable without a socket, and keeps the
// assertions deterministic.
//
// **Lives in tests/, and must stay there.** These files define a second main()
// and a second SimBleLink. The library has no srcFilter, so anything under
// src/ lands in the archive next to simulator_main.o and the linker is free to
// satisfy main() from the wrong one. A header comment saying "do not link this"
// is not enforcement; a directory that is not compiled is.

#include <string>
#include <vector>

#include "SimBleLink.h"

namespace simble_selftest {

// Hands one decoded op to the sink SimBleGatt registered, exactly as the
// reader thread would.
void feed(const SimBleEvent &event);

// Every JSON line the shim emitted, oldest first.
std::vector<std::string> emitted();
void clearEmitted();

}  // namespace simble_selftest
