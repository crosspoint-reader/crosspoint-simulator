#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>

#include <string>

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#ifndef EPD_SCLK
#define EPD_SCLK 8 // SPI Clock
#endif
#ifndef EPD_MOSI
#define EPD_MOSI 10 // SPI MOSI (Master Out Slave In)
#endif
#ifndef EPD_CS
#define EPD_CS 21 // Chip Select
#endif
#ifndef EPD_DC
#define EPD_DC 4 // Data/Command
#endif
#ifndef EPD_RST
#define EPD_RST 5 // Reset
#endif
#ifndef EPD_BUSY
#define EPD_BUSY 6 // Busy
#endif

#define SPI_MISO                                                               \
  7 // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 0 // Battery voltage

#define UART0_RXD 20 // Used for USB connection detection

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

public:
  enum class DeviceType : uint8_t { X4, X3 };

private:
  DeviceType _deviceType = DeviceType::X4;

public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }
  bool isXteinkDevice() const;
  bool hasEdgeSideButtons() const;

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Clears the per-frame press/release edge latches. Must be called exactly
  // once per frame (before the firmware's loop()), NOT on every update().
  void beginFrame();

  // Button input methods
  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;

  // --- Live button injection (simulator-only; no firmware counterpart) ------
  //
  // Drives one button's full state — press edge, held level and press
  // timestamp — from a host harness that has no real keyboard behind it (the
  // on-screen pad on a phone, CROSSPOINT_SIM_INPUT_SCRIPT, a future automation
  // hook).
  //
  // WHY THIS EXISTS RATHER THAN SDL_PushEvent. Measured, not assumed:
  // SDL_PushEvent delivers an event to SDL's queue but never writes SDL's
  // internal keyboard state array, which is only filled on the real-input path.
  // A harness that injects by pushing events therefore drives the EDGE reads
  // (wasPressed/wasReleased, fed from the dequeued event) while every LEVEL read
  // — isPressed(), getHeldTime(), getPowerButtonHeldTime(), all of which consult
  // SDL_GetKeyboardState() — stays false. Everything timed off a held button
  // silently never fires: long-press-to-sleep, page-turn autorepeat, the
  // reader's font-family hold. These two entry points write the same three
  // arrays the SDL path writes, so both halves come from one place.
  //
  // Deliberately platform-neutral: it takes a HalGPIO::BTN_* index and contains
  // no #ifdef. The knowledge of which host maps what onto which button belongs
  // to the harness, above this layer.
  void injectButtonDown(uint8_t buttonIndex);
  void injectButtonUp(uint8_t buttonIndex);

  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;
  bool hasTouch() const;
  bool hasHomeKey() const;
  bool wasHomeKeyPressed() const;
  bool wasHomeKeyTapped() const;
  bool wasHomeKeyLongPressed() const;
  bool wasTouchTap(float &nx, float &ny) const;
  bool wasTouchDown(float &nx, float &ny) const;
  bool wasTouchReleased() const;
  bool isTouchTapCandidate(float &nx, float &ny, unsigned long &heldMs) const;
  bool isTouchHeldAt(float &nx, float &ny) const;
  unsigned long lastTouchHeldMs() const;
  bool wasSwipe(float &nxStart, float &nyStart, float &nxEnd,
                float &nyEnd) const;
  bool wasTouchActivity() const;
  void setSharedConfirmPowerShortPressEmitsPower(bool enabled);
  bool consumeSimulatorSleepRequest();

  // --- Host keyboard text entry (firmware HAL surface; see lib/hal/HalGPIO.h)
  //
  // The firmware's text-entry activities announce a field opening/closing and
  // drain typed bytes every frame. Stubs for now: no host keyboard is wired
  // into this HAL, so a field behaves exactly as it does on device (peck the
  // characters out of the on-screen grid). They exist because
  // MappedInputManager.h calls both unconditionally and TypedTextInput.h reads
  // the three control bytes, so the firmware does not link without them.
  void setTextEntryActive(bool /*active*/) {}
  bool consumeTypedText(std::string & /*out*/) { return false; }

  static constexpr char TYPED_BACKSPACE = '\b';
  static constexpr char TYPED_COMMIT = '\n';
  static constexpr char TYPED_CANCEL = '\x1b';

  // Setup wake up GPIO and enter deep sleep
  void startDeepSleep();

  // Verify power button was held long enough after wakeup.
  // The host wake path is synthetic, so verification always succeeds.
  bool verifyPowerButtonWakeup(uint16_t requiredDurationMs,
                               bool shortPressAllowed);

  // Check if USB is connected
  bool isUsbConnected() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio; // Singleton
