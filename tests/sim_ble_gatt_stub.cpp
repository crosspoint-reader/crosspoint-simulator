// A SimBleLink that keeps no socket. See sim_ble_gatt_selftest.h for why this
// lives in tests/ and must stay there.
//
// It is the GATT self-test's stand-in for the real transport, so the model can
// be driven without a socket. The real transport is exercised separately by
// tests/sim_ble_gatt_attach_selftest.cpp.

#include "sim_ble_gatt_selftest.h"

#include <mutex>

namespace {

std::mutex g_mutex;
void (*g_sink)(void *, const SimBleEvent &) = nullptr;
void *g_sinkCtx = nullptr;
bool g_running = false;
std::vector<std::string> g_emitted;

}  // namespace

SimBleLink &SimBleLink::get() {
  static SimBleLink instance;
  return instance;
}

bool SimBleLink::start(uint16_t port) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_running = port != 0;
  return g_running;
}

void SimBleLink::stop() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_running = false;
}

bool SimBleLink::running() const {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_running;
}

void SimBleLink::setSink(void (*fn)(void *, const SimBleEvent &), void *ctx) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_sink = fn;
  g_sinkCtx = ctx;
}

void SimBleLink::emit(const char *json) {
  if (json == nullptr) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_emitted.emplace_back(json);
}

namespace simble_selftest {

void feed(const SimBleEvent &event) {
  void (*sink)(void *, const SimBleEvent &) = nullptr;
  void *ctx = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    sink = g_sink;
    ctx = g_sinkCtx;
  }
  if (sink != nullptr) sink(ctx, event);
}

std::vector<std::string> emitted() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_emitted;
}

void clearEmitted() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_emitted.clear();
}

}  // namespace simble_selftest
