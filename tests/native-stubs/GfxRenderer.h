#pragma once

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
  int getScreenWidth() const { return portrait() ? 480 : 800; }
  int getScreenHeight() const { return portrait() ? 800 : 480; }

private:
  bool portrait() const {
    return orientation == Portrait || orientation == PortraitInverted;
  }
  Orientation orientation = Portrait;
};
