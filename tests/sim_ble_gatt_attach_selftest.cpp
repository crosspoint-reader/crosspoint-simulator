// sim_ble_gatt_attach_selftest -- the GATT model against the REAL transport.
//
// The other GATT self-test uses a stub SimBleLink, so it cannot prove the one
// behaviour that only exists in the socket path: a client that connects after
// the firmware built its GATT table is told the current state without asking.
// This binary links src/SimBleLink.cpp, opens a real loopback socket to itself
// and reads what arrives.
//
// Build and run (one line, no continuations):
//   g++ -std=c++17 -Wall -Wextra -pthread -Isrc
//   -o /tmp/sim_ble_gatt_attach_selftest src/NimBLEDevice.cpp
//   src/SimBleGatt.cpp src/SimBleLink.cpp src/SimBleProtocol.cpp
//   tests/sim_ble_gatt_attach_selftest.cpp
//   /tmp/sim_ble_gatt_attach_selftest
//
// **Never under src/.** It defines main(); see sim_ble_gatt_selftest.h.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "NimBLEDevice.h"
#include "SimBleLink.h"

namespace {

const char *kServiceUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0001";
const char *kCmdUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0003";

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *what) {
  ++g_checks;
  if (!ok) ++g_failures;
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

int connectTo(uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  timeval tv{};
  tv.tv_sec = 3;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

// Reads until `want` newline-terminated lines have arrived or the socket
// timeout fires.
std::vector<std::string> readLines(int fd, size_t want) {
  std::vector<std::string> lines;
  std::string buffer;
  char chunk[1024];
  while (lines.size() < want) {
    const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
    if (got <= 0) break;
    buffer.append(chunk, static_cast<size_t>(got));
    size_t at = 0;
    for (;;) {
      const size_t nl = buffer.find('\n', at);
      if (nl == std::string::npos) break;
      lines.push_back(buffer.substr(at, nl - at));
      at = nl + 1;
    }
    buffer.erase(0, at);
  }
  return lines;
}

bool has(const std::string &line, const char *needle) {
  return line.find(needle) != std::string::npos;
}

void checkReplay(const std::vector<std::string> &lines, const char *label) {
  const bool ok = lines.size() == 3 &&
                  has(lines[0], "\"ev\":\"stack\",\"state\":\"up\"") &&
                  has(lines[1], "\"ev\":\"gatt\"") &&
                  has(lines[2], "\"ev\":\"advertising\",\"up\":true");
  check(ok, label);
  if (!ok) {
    for (const std::string &line : lines) printf("      got: %s\n", line.c_str());
  }
}

}  // namespace

int main() {
  // Find a port the listener actually binds. init() cannot report a bind
  // failure, but SimBleLink::running() can.
  uint16_t port = 0;
  for (uint16_t candidate = 45311; candidate < 45361; ++candidate) {
    setenv("CROSSPOINT_SIM_BLE_PORT", std::to_string(candidate).c_str(), 1);
    NimBLEDevice::init("sim-attach-selftest");
    if (SimBleLink::get().running()) {
      port = candidate;
      break;
    }
    NimBLEDevice::deinit(true);
  }
  if (port == 0) {
    printf("FAIL  no free loopback port in 45311..45360\n");
    fflush(stdout);
    _exit(1);
  }
  printf("listener on 127.0.0.1:%u\n", static_cast<unsigned>(port));
  check(NimBLEDevice::isInitialized(), "init with a port set");

  // Build the table with nobody connected. Every event these emit is dropped,
  // which is the whole reason the replay exists.
  NimBLEServer *server = NimBLEDevice::createServer();
  NimBLEService *service = server->createService(kServiceUuid);
  service->createCharacteristic(kCmdUuid, NIMBLE_PROPERTY::WRITE |
                                              NIMBLE_PROPERTY::INDICATE);
  check(server->start(), "server->start with nobody connected");
  check(NimBLEDevice::getAdvertising()->start(),
        "advertising->start with nobody connected");

  const int first = connectTo(port);
  check(first >= 0, "a client connects");
  checkReplay(readLines(first, 3),
              "a late client receives stack up, gatt and advertising, unasked");

  // The slot frees when the client leaves, and the next client gets the same
  // replay. The dropped socket also synthesizes a disconnect, which the model
  // answers with an error because no central was connected -- expected, and it
  // must not disturb the next replay.
  ::close(first);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const int second = connectTo(port);
  check(second >= 0, "a second client connects after the first left");
  checkReplay(readLines(second, 3), "the replay happens for every client");

  ::close(second);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  NimBLEDevice::deinit(true);
  check(!NimBLEDevice::isInitialized(), "deinit tears the stack down");
  check(!SimBleLink::get().running(), "deinit stops the listener");

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  fflush(stdout);
  _exit(g_failures == 0 ? 0 : 1);
}
