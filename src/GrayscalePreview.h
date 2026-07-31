#pragma once
#include <cstdint>

// Pure decode of the e-ink two-plane grayscale representation into the
// simulator's preview gray levels. Kept free of SDL and HalDisplay state so a
// plain host test can exercise it (the same way fstest/roottest exercised the
// storage layer).
//
// Representation recap (mirrors the firmware's GfxRenderer grayscale passes):
// the BW base frame paints every non-white pixel black, then two 1bpp planes
// are written where a set bit means "update this pixel toward a gray target":
//   MSB only  -> light gray
//   MSB + LSB -> dark gray
//   LSB only  -> dark gray (never emitted by current firmware, decoded anyway)
//   neither   -> pixel keeps its base color
// Plane flags only lighten base-black pixels; the firmware never flags a
// base-white pixel and the panel waveforms are not driven that way, so a white
// pixel stays white regardless of plane bits.
namespace GrayscalePreview {

// Preview levels, chosen to read like the panel's four optical gray levels.
constexpr uint8_t kWhite = 255;
constexpr uint8_t kLight = 200;
constexpr uint8_t kDark = 96;
constexpr uint8_t kBlack = 0;

constexpr uint8_t previewLevel(bool baseWhite, bool msbFlagged,
                               bool lsbFlagged) {
  if (baseWhite)
    return kWhite;
  if (msbFlagged)
    return lsbFlagged ? kDark : kLight;
  if (lsbFlagged)
    return kDark;
  return kBlack;
}

} // namespace GrayscalePreview
