#pragma once
#include <Arduino.h>
#include <EInkDisplay.h>

#include <atomic>

// Supersampling factor between the LOGICAL screen coordinate space the firmware
// draws in and the PHYSICAL framebuffer this HAL owns. Mirrors the firmware
// header's macro (which defaults to 1); here it is >1 so text can be rasterised
// at the host panel's real pixel density.
//
// LAYOUT IS UNAFFECTED BY DESIGN: GfxRenderer::getScreenWidth()/getScreenHeight()
// still report 528x792 on X3, every advance/kern/line-break still comes from the
// 1x .cpfont tables, and only the pixel-write layer multiplies up. Raising this
// must therefore never move a line break; if it does, something started reading
// panel geometry as layout input.
//
// Cost at 2: framebuffer 792*528/8 = 52,272 B -> 1584*1056/8 = 209,088 B, and
// HalDisplay.cpp holds four of those (bwBase + lsb + msb + frameBufferStorage)
// plus an ARGB present buffer of DISPLAY_WIDTH*DISPLAY_HEIGHT*4 = 6.7 MB.
#ifndef CROSSPOINT_RENDER_SCALE
#define CROSSPOINT_RENDER_SCALE 2
#endif

class HalDisplay {
public:
  // Constructor with pin configuration
  HalDisplay();

  // Destructor
  ~HalDisplay();

  // Refresh modes
  enum RefreshMode {
    FULL_REFRESH, // Full refresh with complete waveform
    HALF_REFRESH, // Half refresh (1720ms) - balanced quality and speed
    FAST_REFRESH  // Fast refresh using custom LUT
  };

  // Initialize the display hardware and driver
  void begin();
  void begin(bool seamless);

  // Display dimensions, in PHYSICAL framebuffer pixels. At
  // CROSSPOINT_RENDER_SCALE > 1 these are a multiple of the panel the firmware
  // believes it is driving; the logical panel the firmware lays out against is
  // still EInkDisplay::DISPLAY_WIDTH x DISPLAY_HEIGHT and is what
  // GfxRenderer::getScreenWidth()/getScreenHeight() report.
  static constexpr int RENDER_SCALE = CROSSPOINT_RENDER_SCALE;
  static constexpr uint16_t DISPLAY_WIDTH = EInkDisplay::DISPLAY_WIDTH * RENDER_SCALE;
  static constexpr uint16_t DISPLAY_HEIGHT = EInkDisplay::DISPLAY_HEIGHT * RENDER_SCALE;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE =
      static_cast<uint32_t>(DISPLAY_WIDTH_BYTES) * DISPLAY_HEIGHT;
  // Logical panel geometry (what the firmware would see on real hardware).
  static constexpr uint16_t LOGICAL_WIDTH = EInkDisplay::DISPLAY_WIDTH;
  static constexpr uint16_t LOGICAL_HEIGHT = EInkDisplay::DISPLAY_HEIGHT;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t *imageData, uint16_t x, uint16_t y, uint16_t w,
                 uint16_t h, bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t *imageData, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;

  // Persistent black/white polarity used by the X4 Pro frontlight panel.
  void setInverted(bool inverted);
  bool toggleInverted();
  bool isInverted() const;

  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH,
                     bool turnOffScreen = false);
  void displayBufferAsync(RefreshMode mode = RefreshMode::FAST_REFRESH);
  void waitRefreshComplete();
  bool supportsAsyncRefresh() const;
  void displayWindow(int x, int y, int w, int h);
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH,
                      bool turnOffScreen = false);

  // Power management
  void deepSleep();

  // Access to frame buffer
  uint8_t *getFrameBuffer() const;
  uint8_t *lendFrameBufferStorage(uint32_t *sizeOut);
  void returnFrameBufferStorage();

  // Runtime geometry passthrough
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;
  uint32_t getBufferSize() const;

  void displayGrayscaleBase(RefreshMode fallback = HALF_REFRESH,
                            bool turnOffScreen = false);
  void preconditionGrayscale();
  void preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

  void copyGrayscaleBuffers(const uint8_t *lsbBuffer, const uint8_t *msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t *lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t *msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t *bwBuffer);

  void displayGrayBuffer(bool turnOffScreen = false,
                         const unsigned char *lut = nullptr,
                         bool factoryMode = false);

  // The simulator intentionally advertises strip grayscale support so host
  // builds exercise the same low-memory path as the device firmware, and so
  // streamed plane data can feed the same grayscale preview compositor as the
  // legacy full-frame API.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t *rows,
                                uint16_t yStart, uint16_t numRows);
  bool supportsStripGrayscale() const;

  // Simulator only: call from main thread to push rendered pixels to SDL.
  void presentIfNeeded();
  // Simulator only: returns true once a hard shutdown has been requested.
  bool shouldQuit() const;

private:
  EInkDisplay einkDisplay;
  // Atomic because polarity is written from the main thread (host appearance /
  // CROSSPOINT_SIM_DARK via SimulatorOverlay::setPanelDark) while the render
  // task reads it during framebuffer conversion. Private member only -- the
  // public surface above still mirrors the firmware HAL exactly.
  std::atomic<bool> inverted{false};
};

extern HalDisplay display;
