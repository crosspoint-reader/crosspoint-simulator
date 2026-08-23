#pragma once

// Test-only seam for the NimBLE shim's self-test.
//
// A3 owns the real SimBleLink implementation. Until it lands, and to keep the
// self-test free of sockets either way, SimBleGattSelfTestStub.cpp implements
// SimBleLink over these two calls: feed() plays the reader thread, emitted()
// captures what would have gone down the socket.
//
// Delete this pair once the shim is exercised through the real transport.

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
