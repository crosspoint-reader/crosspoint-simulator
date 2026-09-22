#include "HalMemory.h"

#include "Arduino.h"

namespace {
HalMemory::HeapStats readMockHeap() {
  return {ESP.getFreeHeap(), ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap()};
}
}  // namespace

// The simulator has no separate internal/PSRAM heaps; both report the same mock heap.
HalMemory::HeapStats HalMemory::getDefaultHeap() { return readMockHeap(); }

HalMemory::HeapStats HalMemory::getInternalHeap() { return readMockHeap(); }

HalMemory::HeapStats HalMemory::getPsramHeap() { return {0, 0, 0, 0}; }

void HalMemory::PsramDeleter::operator()(uint8_t*) const {}

HalMemory::PsramBuffer HalMemory::allocatePsram(size_t) { return PsramBuffer(nullptr, PsramDeleter{}); }
