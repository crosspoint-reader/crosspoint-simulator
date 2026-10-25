#pragma once

#include <cstdlib>
#include <string>

namespace crosspoint_simulator {

// Host folder that stands in for the SD card.
inline std::string storageRoot() {
  const char *root = std::getenv("CROSSPOINT_SIM_SD");
  if (!root || !*root) {
    root = std::getenv("CROSSPOINT_EMU_SD");
  }
  return (root && *root) ? std::string(root) : std::string("./fs_");
}

} // namespace crosspoint_simulator
