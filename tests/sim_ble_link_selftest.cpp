// sim_ble_link_selftest -- standalone driver for SimBleLink and
// SimBleProtocol. It builds from those two files alone, with no simulator and
// no firmware, so the transport can be proven before the GATT model exists.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -O1 -Isrc
//       src/SimBleLink.cpp src/SimBleProtocol.cpp
//       tests/sim_ble_link_selftest.cpp -o /tmp/sim_ble_link_selftest -lpthread
//
// Run: the port comes from argv[1]. Commands arrive on stdin, one per line:
//   emit <json>       emit one line to the client
//   emitstress <n>    two threads emit n lines each, concurrently
//   stop              time SimBleLink::stop() and exit
// Every decoded op is printed as one SINK line on stdout. The python driver
// tests/sim_ble_link_selftest.py reads those.

#include "SimBleLink.h"
#include "SimBleProtocol.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex outMutex;

void say(const std::string &line) {
  std::lock_guard<std::mutex> lock(outMutex);
  std::cout << line << "\n";
  std::cout.flush();
}

void recordEvent(void *ctx, const SimBleEvent &ev) {
  (void)ctx;
  std::string line = "SINK op=" + ev.op + " uuid=" + (ev.uuid.empty() ? "-" : ev.uuid) +
                     " data=" +
                     (ev.data.empty()
                          ? std::string("-")
                          : crosspoint_simulator::ble::encodeHex(ev.data)) +
                     " a=" + std::to_string(ev.a) + " b=" + std::to_string(ev.b) +
                     " c=" + std::to_string(ev.c) + " d=" + std::to_string(ev.d) +
                     " flag=" + (ev.flag ? "1" : "0");
  say(line);
}

void emitStress(int perThread) {
  auto worker = [perThread](char tag) {
    for (int i = 0; i < perThread; ++i) {
      // A long payload so an interleave would be obvious in the received line.
      std::string payload(200, tag);
      const std::string json = "{\"ev\":\"stress\",\"tag\":\"" +
                               std::string(1, tag) + "\",\"n\":" +
                               std::to_string(i) + ",\"pad\":\"" + payload +
                               "\"}";
      SimBleLink::get().emit(json.c_str());
    }
  };
  std::thread a(worker, 'A');
  std::thread b(worker, 'B');
  a.join();
  b.join();
  say("STRESS done");
}

}  // namespace

int main(int argc, char **argv) {
  // A zero port must be refused, and the refusal must not be fatal.
  if (SimBleLink::get().start(0))
    say("FAIL start(0) returned true");
  else
    say("OK start(0) refused");
  say(std::string("OK running after start(0) = ") +
      (SimBleLink::get().running() ? "true" : "false"));

  // stop() before any start must be a no-op, not a hang or a crash.
  SimBleLink::get().stop();
  say("OK stop() before start returned");

  const uint16_t port = static_cast<uint16_t>(argc > 1 ? std::atoi(argv[1]) : 0);
  SimBleLink::get().setSink(&recordEvent, nullptr);
  if (!SimBleLink::get().start(port)) {
    say("FAIL start failed");
    return 1;
  }
  say("READY " + std::to_string(port));

  std::string command;
  while (std::getline(std::cin, command)) {
    if (command.rfind("emitstress ", 0) == 0) {
      emitStress(std::atoi(command.c_str() + 11));
    } else if (command.rfind("emit ", 0) == 0) {
      SimBleLink::get().emit(command.c_str() + 5);
    } else if (command == "stop") {
      const auto t0 = std::chrono::steady_clock::now();
      SimBleLink::get().stop();
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
      say("STOPPED in " + std::to_string(ms) + " ms, running=" +
          (SimBleLink::get().running() ? "true" : "false"));
      break;
    }
  }

  SimBleLink::get().setSink(nullptr, nullptr);
  SimBleLink::get().stop();
  say("EXIT");
  return 0;
}
