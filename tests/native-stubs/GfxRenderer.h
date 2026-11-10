#pragma once
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <cmath>

// Geometry-only renderer for native HAL input tests. Firmware runtime tests
// exercise the real renderer separately.
class GfxRenderer {
public:
  enum Orientation {
    Portrait,
    LandscapeClockwise,
    PortraitInverted,
    LandscapeCounterClockwise
  };
  void setOrientation(Orientation value) { orientation = value; }
  Orientation getOrientation() const { return orientation; }
  bool portrait() const {
    return orientation == Portrait || orientation == PortraitInverted;
  }
  int getScreenWidth() const {
    const int physical =
        portrait() ? EInkDisplay::DISPLAY_HEIGHT : EInkDisplay::DISPLAY_WIDTH;
    return static_cast<int>(std::lround(physical / BoardConfig::ACTIVE.uiScale));
  }
  int getScreenHeight() const {
    const int physical =
        portrait() ? EInkDisplay::DISPLAY_WIDTH : EInkDisplay::DISPLAY_HEIGHT;
    return static_cast<int>(std::lround(physical / BoardConfig::ACTIVE.uiScale));
  }

private:
  Orientation orientation = Portrait;
};
