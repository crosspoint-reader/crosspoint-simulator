#include "PadCore.h"

std::vector<PadCore::Action> PadCore::fingerDown(int slot, long long fingerId) {
  if (slot < 0 || slot >= static_cast<int>(down_.size())) return {};
  const size_t i = static_cast<size_t>(slot);
  if (down_[i]) return {};  // already held by another finger; ignore
  down_[i] = true;
  finger_[i] = fingerId;
  return {{Action::Press, slot}};
}

int PadCore::slotHeldBy(long long fingerId) const {
  for (size_t i = 0; i < down_.size(); i++)
    if (down_[i] && finger_[i] == fingerId) return static_cast<int>(i);
  return kNoSlot;
}

std::vector<PadCore::Action> PadCore::fingerUp(long long fingerId) {
  const int slot = slotHeldBy(fingerId);
  if (slot == kNoSlot) return {};
  down_[static_cast<size_t>(slot)] = false;
  return {{Action::Release, slot}};
}

std::vector<PadCore::Action> PadCore::fingerLeftSlot(long long fingerId) {
  // Same release semantics as a lift; only the trigger differs.
  return fingerUp(fingerId);
}

std::vector<PadCore::Action> PadCore::reset() {
  std::vector<Action> out;
  for (size_t i = 0; i < down_.size(); i++) {
    if (down_[i]) {
      down_[i] = false;
      out.push_back({Action::Release, static_cast<int>(i)});
    }
  }
  return out;
}
