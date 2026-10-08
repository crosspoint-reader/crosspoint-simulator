#pragma once
#include <EInkDisplay.h>

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
  int getScreenWidth() const {
    return portrait() ? EInkDisplay::DISPLAY_HEIGHT
                      : EInkDisplay::DISPLAY_WIDTH;
  }
  int getScreenHeight() const {
    return portrait() ? EInkDisplay::DISPLAY_WIDTH
                      : EInkDisplay::DISPLAY_HEIGHT;
  }

private:
  bool portrait() const {
    return orientation == Portrait || orientation == PortraitInverted;
  }
  Orientation orientation = Portrait;
};
