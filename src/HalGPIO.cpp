#include "HalGPIO.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "HalDisplay.h"
#include "SimulatorLifecycle.h"

// Defined in HalDisplay.cpp — set here so all SDL event polling lives in one
// place.
extern std::atomic<bool> quitRequested;
extern GfxRenderer renderer;

// Keyboard mapping:
//   BTN_BACK    (0) → Escape
//   BTN_CONFIRM (1) → Return
//   BTN_LEFT    (2) → Left arrow
//   BTN_RIGHT   (3) → Right arrow
//   BTN_UP      (4) → Up arrow
//   BTN_DOWN    (5) → Down arrow
//   BTN_POWER   (6) → P
//   Simulator sleep shortcut → S

static constexpr int NUM_BUTTONS = 7;
static constexpr SDL_Scancode SIMULATOR_SLEEP_SCANCODE = SDL_SCANCODE_S;
static constexpr SDL_Scancode HOME_KEY_SCANCODE = SDL_SCANCODE_H;
static constexpr int TOUCH_TAP_SLOP_PX = 28;
static constexpr int TOUCH_SWIPE_MIN_PX = 60;
static constexpr unsigned long TOUCH_SWIPE_MAX_MS = 700;
static constexpr unsigned long TOUCH_LONG_PRESS_MS = 500;
static constexpr unsigned long HOME_KEY_LONG_PRESS_MS = 700;

static const SDL_Scancode buttonScancode[NUM_BUTTONS] = {
    SDL_SCANCODE_ESCAPE, // BTN_BACK
    SDL_SCANCODE_RETURN, // BTN_CONFIRM
    SDL_SCANCODE_LEFT,   // BTN_LEFT
    SDL_SCANCODE_RIGHT,  // BTN_RIGHT
    SDL_SCANCODE_UP,     // BTN_UP
    SDL_SCANCODE_DOWN,   // BTN_DOWN
    SDL_SCANCODE_P,      // BTN_POWER
};

static bool pressedThisFrame[NUM_BUTTONS] = {};
static bool releasedThisFrame[NUM_BUTTONS] = {};
static unsigned long buttonPressTime[NUM_BUTTONS] = {};
static bool syntheticButtonDown[NUM_BUTTONS] = {};
static bool simulatorSleepRequested = false;

namespace {

struct TouchState {
  bool down = false;
  bool pressedThisFrame = false;
  bool releasedThisFrame = false;
  bool movedBeyondTapSlop = false;
  bool longPressThisFrame = false;
  bool longPressFired = false;
  bool suppressed = false;
  bool activityThisFrame = false;
  float startNx = 0.0f;
  float startNy = 0.0f;
  float currentNx = 0.0f;
  float currentNy = 0.0f;
  unsigned long pressedAt = 0;
  unsigned long lastHeldMs = 0;
};

TouchState touchState;
bool homeKeyDown = false;
bool homeKeyPressedThisFrame = false;
bool homeKeyTappedThisFrame = false;
bool homeKeyLongPressedThisFrame = false;
bool homeKeyLongFired = false;
unsigned long homeKeyPressedAt = 0;

enum class SyntheticAction {
  KeyDown,
  KeyUp,
  TouchDown,
  TouchUp,
  HomeDown,
  HomeUp,
  Sleep,
  Quit
};

struct SyntheticEvent {
  unsigned long atMs;
  SyntheticAction action;
  int button = -1;
  float logicalNx = 0.0f;
  float logicalNy = 0.0f;
  bool handled = false;
};

std::vector<SyntheticEvent> syntheticEvents;
bool syntheticEventsInitialized = false;

float clamp01(float value) { return std::max(0.0f, std::min(1.0f, value)); }

void logicalToPanelNormalized(float logicalNx, float logicalNy, float &panelNx,
                              float &panelNy) {
  const int logicalWidth = renderer.getScreenWidth();
  const int logicalHeight = renderer.getScreenHeight();
  const int lx = static_cast<int>(clamp01(logicalNx) *
                                  static_cast<float>(logicalWidth - 1));
  const int ly = static_cast<int>(clamp01(logicalNy) *
                                  static_cast<float>(logicalHeight - 1));

  int physicalX = 0;
  int physicalY = 0;
  switch (renderer.getOrientation()) {
  case GfxRenderer::Portrait:
    physicalX = ly;
    physicalY = HalDisplay::DISPLAY_HEIGHT - 1 - lx;
    break;
  case GfxRenderer::PortraitInverted:
    physicalX = HalDisplay::DISPLAY_WIDTH - 1 - ly;
    physicalY = lx;
    break;
  case GfxRenderer::LandscapeClockwise:
    physicalX = HalDisplay::DISPLAY_WIDTH - 1 - lx;
    physicalY = HalDisplay::DISPLAY_HEIGHT - 1 - ly;
    break;
  case GfxRenderer::LandscapeCounterClockwise:
  default:
    physicalX = lx;
    physicalY = ly;
    break;
  }

  panelNx = clamp01(static_cast<float>(physicalX) /
                    static_cast<float>(HalDisplay::DISPLAY_WIDTH - 1));
  panelNy = clamp01(static_cast<float>(physicalY) /
                    static_cast<float>(HalDisplay::DISPLAY_HEIGHT - 1));
}

void updateTouchMovement(float panelNx, float panelNy) {
  touchState.currentNx = panelNx;
  touchState.currentNy = panelNy;
  const float dx =
      (touchState.currentNx - touchState.startNx) * HalDisplay::DISPLAY_WIDTH;
  const float dy =
      (touchState.currentNy - touchState.startNy) * HalDisplay::DISPLAY_HEIGHT;
  if (std::abs(dx) > TOUCH_TAP_SLOP_PX || std::abs(dy) > TOUCH_TAP_SLOP_PX) {
    touchState.movedBeyondTapSlop = true;
  }
}

void beginTouch(float logicalNx, float logicalNy) {
  if (!BoardConfig::hasTouch())
    return;
  float panelNx = 0.0f;
  float panelNy = 0.0f;
  logicalToPanelNormalized(logicalNx, logicalNy, panelNx, panelNy);
  touchState.down = true;
  touchState.pressedThisFrame = true;
  touchState.activityThisFrame = true;
  touchState.movedBeyondTapSlop = false;
  touchState.longPressThisFrame = false;
  touchState.longPressFired = false;
  touchState.suppressed = false;
  touchState.startNx = panelNx;
  touchState.startNy = panelNy;
  touchState.currentNx = panelNx;
  touchState.currentNy = panelNy;
  touchState.pressedAt = SDL_GetTicks();
}

void moveTouch(float logicalNx, float logicalNy) {
  if (!touchState.down)
    return;
  float panelNx = 0.0f;
  float panelNy = 0.0f;
  logicalToPanelNormalized(logicalNx, logicalNy, panelNx, panelNy);
  updateTouchMovement(panelNx, panelNy);
}

void endTouch(float logicalNx, float logicalNy) {
  if (!touchState.down)
    return;
  moveTouch(logicalNx, logicalNy);
  touchState.down = false;
  touchState.releasedThisFrame = true;
  touchState.activityThisFrame = true;
  touchState.lastHeldMs = SDL_GetTicks() - touchState.pressedAt;
}

void updateTouchHold() {
  if (touchState.down && !touchState.movedBeyondTapSlop &&
      !touchState.longPressFired && !touchState.suppressed &&
      SDL_GetTicks() - touchState.pressedAt >= TOUCH_LONG_PRESS_MS) {
    touchState.longPressFired = true;
    touchState.longPressThisFrame = true;
  }
}

void beginHomeKey() {
  if (!BoardConfig::hasHomeKey() || homeKeyDown)
    return;
  homeKeyDown = true;
  homeKeyPressedThisFrame = true;
  homeKeyLongFired = false;
  homeKeyPressedAt = SDL_GetTicks();
}

void endHomeKey() {
  if (!homeKeyDown)
    return;
  if (!homeKeyLongFired &&
      SDL_GetTicks() - homeKeyPressedAt < HOME_KEY_LONG_PRESS_MS) {
    homeKeyTappedThisFrame = true;
  }
  homeKeyDown = false;
}

void updateHomeKeyHold() {
  if (homeKeyDown && !homeKeyLongFired &&
      SDL_GetTicks() - homeKeyPressedAt >= HOME_KEY_LONG_PRESS_MS) {
    homeKeyLongFired = true;
    homeKeyLongPressedThisFrame = true;
  }
}

bool parseTouchSpec(const std::string &detail, float &x1, float &y1, float &x2,
                    float &y2, unsigned long &duration, bool swipe) {
  unsigned parsedDuration = swipe ? 250 : 80;
  int parsed = 0;
  if (swipe) {
    parsed = std::sscanf(detail.c_str(), "%f,%f,%f,%f,%u", &x1, &y1, &x2, &y2,
                         &parsedDuration);
    if (parsed < 4)
      return false;
  } else {
    parsed = std::sscanf(detail.c_str(), "%f,%f,%u", &x1, &y1, &parsedDuration);
    if (parsed < 2)
      return false;
    x2 = x1;
    y2 = y1;
  }

  // Scripts normally use logical display pixels because those coordinates are
  // easy to read from UI layouts and screenshots. Preserve support for the
  // earlier 0.0-1.0 normalized form so existing local QA scripts keep working.
  const auto normalize = [](float value, int extent) {
    if (value >= 0.0f && value <= 1.0f)
      return value;
    return clamp01(value / static_cast<float>(std::max(1, extent - 1)));
  };
  const int logicalWidth = renderer.getScreenWidth();
  const int logicalHeight = renderer.getScreenHeight();
  x1 = normalize(x1, logicalWidth);
  y1 = normalize(y1, logicalHeight);
  x2 = normalize(x2, logicalWidth);
  y2 = normalize(y2, logicalHeight);
  duration = parsedDuration;
  return true;
}

void requestSimulatorSleep() {
  simulatorSleepRequested = true;
  // Current CrossPoint firmware sleeps on a held physical power button. Keep
  // the compatibility latch above for older consumers, and also drive the
  // current public HalGPIO state so the S shortcut follows the real firmware
  // sleep path.
  pressedThisFrame[HalGPIO::BTN_POWER] = true;
  syntheticButtonDown[HalGPIO::BTN_POWER] = true;
  buttonPressTime[HalGPIO::BTN_POWER] = SDL_GetTicks();
}

std::string uppercase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return value;
}

int namedButton(const std::string &name) {
  if (name == "ESCAPE" || name == "BACK")
    return HalGPIO::BTN_BACK;
  if (name == "RETURN" || name == "ENTER" || name == "CONFIRM")
    return HalGPIO::BTN_CONFIRM;
  if (name == "LEFT")
    return HalGPIO::BTN_LEFT;
  if (name == "RIGHT")
    return HalGPIO::BTN_RIGHT;
  if (name == "UP")
    return HalGPIO::BTN_UP;
  if (name == "DOWN")
    return HalGPIO::BTN_DOWN;
  if (name == "P" || name == "POWER")
    return HalGPIO::BTN_POWER;
  return -1;
}

void initializeSyntheticEvents() {
  if (syntheticEventsInitialized)
    return;
  syntheticEventsInitialized = true;

  const char *script = std::getenv("CROSSPOINT_SIM_INPUT_SCRIPT");
  if (!script || script[0] == '\0')
    return;

  const std::string spec(script);
  size_t start = 0;
  while (start < spec.size()) {
    const size_t end = spec.find(';', start);
    const std::string item = spec.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    const size_t firstColon = item.find(':');
    const size_t secondColon = firstColon == std::string::npos
                                   ? std::string::npos
                                   : item.find(':', firstColon + 1);
    if (firstColon != std::string::npos) {
      const unsigned long atMs =
          std::strtoul(item.substr(0, firstColon).c_str(), nullptr, 10);
      const std::string key = uppercase(
          item.substr(firstColon + 1, secondColon == std::string::npos
                                          ? std::string::npos
                                          : secondColon - firstColon - 1));
      if (key == "QUIT") {
        syntheticEvents.push_back({atMs, SyntheticAction::Quit});
      } else if (key == "S" || key == "SLEEP") {
        syntheticEvents.push_back({atMs, SyntheticAction::Sleep});
      } else if (key == "HOME") {
        const unsigned long holdMs =
            secondColon == std::string::npos
                ? 80
                : std::strtoul(item.substr(secondColon + 1).c_str(), nullptr,
                               10);
        syntheticEvents.push_back({atMs, SyntheticAction::HomeDown});
        syntheticEvents.push_back({atMs + holdMs, SyntheticAction::HomeUp});
      } else if ((key == "TAP" || key == "SWIPE") &&
                 secondColon != std::string::npos) {
        float x1 = 0.0f;
        float y1 = 0.0f;
        float x2 = 0.0f;
        float y2 = 0.0f;
        unsigned long duration = 0;
        const bool swipe = key == "SWIPE";
        if (parseTouchSpec(item.substr(secondColon + 1), x1, y1, x2, y2,
                           duration, swipe)) {
          syntheticEvents.push_back(
              {atMs, SyntheticAction::TouchDown, -1, x1, y1});
          syntheticEvents.push_back(
              {atMs + duration, SyntheticAction::TouchUp, -1, x2, y2});
        }
      } else {
        const int button = namedButton(key);
        if (button >= 0) {
          const unsigned long holdMs =
              secondColon == std::string::npos
                  ? 80
                  : std::strtoul(item.substr(secondColon + 1).c_str(), nullptr,
                                 10);
          syntheticEvents.push_back({atMs, SyntheticAction::KeyDown, button});
          syntheticEvents.push_back(
              {atMs + holdMs, SyntheticAction::KeyUp, button});
        }
      }
    }

    if (end == std::string::npos)
      break;
    start = end + 1;
  }

  std::sort(syntheticEvents.begin(), syntheticEvents.end(),
            [](const SyntheticEvent &a, const SyntheticEvent &b) {
              return a.atMs < b.atMs;
            });
}

void processSyntheticEvents() {
  initializeSyntheticEvents();
  const unsigned long now = millis();
  for (auto &event : syntheticEvents) {
    if (event.handled || event.atMs > now)
      continue;
    event.handled = true;
    switch (event.action) {
    case SyntheticAction::KeyDown:
      pressedThisFrame[event.button] = true;
      syntheticButtonDown[event.button] = true;
      // Held-time calculations use SDL_GetTicks() for real keyboard events;
      // synthetic presses must use the same clock origin to avoid unsigned
      // underflow being mistaken for an immediate long press.
      buttonPressTime[event.button] = SDL_GetTicks();
      break;
    case SyntheticAction::KeyUp:
      releasedThisFrame[event.button] = true;
      syntheticButtonDown[event.button] = false;
      break;
    case SyntheticAction::TouchDown:
      beginTouch(event.logicalNx, event.logicalNy);
      break;
    case SyntheticAction::TouchUp:
      endTouch(event.logicalNx, event.logicalNy);
      break;
    case SyntheticAction::HomeDown:
      beginHomeKey();
      break;
    case SyntheticAction::HomeUp:
      endHomeKey();
      break;
    case SyntheticAction::Sleep:
      requestSimulatorSleep();
      break;
    case SyntheticAction::Quit:
      quitRequested.store(true);
      break;
    }
  }
}

} // namespace

static void clearButtonState() {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
    buttonPressTime[i] = 0;
    syntheticButtonDown[i] = false;
  }
  touchState = {};
  homeKeyDown = false;
  homeKeyPressedThisFrame = false;
  homeKeyTappedThisFrame = false;
  homeKeyLongPressedThisFrame = false;
  homeKeyLongFired = false;
  homeKeyPressedAt = 0;
}

static int scancodeToButton(SDL_Scancode sc) {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (buttonScancode[i] == sc)
      return i;
  }
  return -1;
}

void HalGPIO::begin() {
#if defined(SIMULATOR_DEVICE_STICKY)
  // The firmware's non-C3 path leaves the legacy device discriminator on X4;
  // BoardConfig carries the actual Sticky identity and capabilities.
  _deviceType = DeviceType::X4;
  BoardConfig::selectDevice(BoardConfig::Board::Sticky);
#elif defined(SIMULATOR_DEVICE_X4_PRO)
  _deviceType = DeviceType::X4;
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX4Pro);
#elif defined(SIMULATOR_DEVICE_X3)
  _deviceType = DeviceType::X3;
#if defined(SIMULATOR_DISPLAY_UC8279)
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
#else
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX3);
#endif
#else
  _deviceType = DeviceType::X4;
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);
#endif
}

bool HalGPIO::isXteinkDevice() const {
  // Match the firmware helper's narrower meaning: the runtime-detected C3
  // X3/X4 pair. X4 Pro is an Xteink product but uses its own S3 board profile.
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4;
}

bool HalGPIO::hasEdgeSideButtons() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Pro;
}

void HalGPIO::beginFrame() {
  // Clear the press/release edge latches once per frame. See update() for why
  // this is deliberately separate from the SDL poll.
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
  }
  touchState.pressedThisFrame = false;
  touchState.releasedThisFrame = false;
  touchState.activityThisFrame = false;
  touchState.longPressThisFrame = false;
  homeKeyPressedThisFrame = false;
  homeKeyTappedThisFrame = false;
  homeKeyLongPressedThisFrame = false;
}

void HalGPIO::update() {
  // Per-frame press/release edges are intentionally NOT cleared here; that
  // happens once per frame in beginFrame(). The firmware calls update() several
  // times within a single frame (e.g. CrossPointWebServerActivity polls input
  // between handleClient() bursts, on top of the top-of-loop gpio.update() in
  // main.cpp). If edges were cleared on every update(), a key press drained by
  // an earlier update() would be wiped before a later update()'s wasPressed()
  // check could observe it — which made Back/Exit require repeated presses.
  // Latching edges for the whole frame keeps wasPressed() stable across all
  // update() calls in that frame, matching the on-device InputManager.

  // HalGPIO owns all SDL event polling so keyboard and quit events are never
  // split between two callers (HalDisplay::presentIfNeeded only renders).
  SDL_Event e;
  while (SDL_PollEvent(&e) != 0) {
    if (e.type == SDL_QUIT) {
      quitRequested.store(true);
    } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
      if (e.key.keysym.scancode == HOME_KEY_SCANCODE) {
        beginHomeKey();
        continue;
      }
      if (e.key.keysym.scancode == SIMULATOR_SLEEP_SCANCODE) {
        requestSimulatorSleep();
        continue;
      }
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        pressedThisFrame[btn] = true;
        buttonPressTime[btn] = SDL_GetTicks();
      }
    } else if (e.type == SDL_KEYUP) {
      if (e.key.keysym.scancode == HOME_KEY_SCANCODE) {
        endHomeKey();
        continue;
      }
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        releasedThisFrame[btn] = true;
      }
    } else if (e.type == SDL_MOUSEBUTTONDOWN &&
               e.button.button == SDL_BUTTON_LEFT) {
      const float logicalNx =
          static_cast<float>(e.button.x) /
          std::max(1, static_cast<int>(renderer.getScreenWidth()) - 1);
      const float logicalNy =
          static_cast<float>(e.button.y) /
          std::max(1, static_cast<int>(renderer.getScreenHeight()) - 1);
      beginTouch(logicalNx, logicalNy);
    } else if (e.type == SDL_MOUSEMOTION && touchState.down) {
      const float logicalNx =
          static_cast<float>(e.motion.x) /
          std::max(1, static_cast<int>(renderer.getScreenWidth()) - 1);
      const float logicalNy =
          static_cast<float>(e.motion.y) /
          std::max(1, static_cast<int>(renderer.getScreenHeight()) - 1);
      moveTouch(logicalNx, logicalNy);
    } else if (e.type == SDL_MOUSEBUTTONUP &&
               e.button.button == SDL_BUTTON_LEFT) {
      const float logicalNx =
          static_cast<float>(e.button.x) /
          std::max(1, static_cast<int>(renderer.getScreenWidth()) - 1);
      const float logicalNy =
          static_cast<float>(e.button.y) /
          std::max(1, static_cast<int>(renderer.getScreenHeight()) - 1);
      endTouch(logicalNx, logicalNy);
    }
  }
  processSyntheticEvents();
  updateTouchHold();
  updateHomeKeyHold();
}

bool HalGPIO::isPressed(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS)
    return false;
  const uint8_t *state = SDL_GetKeyboardState(NULL);
  return state[buttonScancode[buttonIndex]] || syntheticButtonDown[buttonIndex];
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS)
    return false;
  return pressedThisFrame[buttonIndex];
}

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS)
    return false;
  return releasedThisFrame[buttonIndex];
}

bool HalGPIO::wasAnyPressed() const {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (pressedThisFrame[i])
      return true;
  }
  return false;
}

bool HalGPIO::wasAnyReleased() const {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (releasedThisFrame[i])
      return true;
  }
  return false;
}

unsigned long HalGPIO::getHeldTime() const {
  // Return the longest held time among all currently pressed buttons
  unsigned long now = SDL_GetTicks();
  unsigned long maxHeld = 0;
  const uint8_t *state = SDL_GetKeyboardState(NULL);
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if ((state[buttonScancode[i]] || syntheticButtonDown[i]) &&
        buttonPressTime[i] > 0) {
      unsigned long held = now - buttonPressTime[i];
      if (held > maxHeld)
        maxHeld = held;
    }
  }
  return maxHeld;
}

unsigned long HalGPIO::getPowerButtonHeldTime() const {
  const uint8_t *state = SDL_GetKeyboardState(NULL);
  if ((!state[buttonScancode[BTN_POWER]] && !syntheticButtonDown[BTN_POWER]) ||
      buttonPressTime[BTN_POWER] == 0)
    return 0;
  return SDL_GetTicks() - buttonPressTime[BTN_POWER];
}

bool HalGPIO::hasTouch() const { return BoardConfig::hasTouch(); }

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

bool HalGPIO::wasHomeKeyPressed() const { return homeKeyPressedThisFrame; }

bool HalGPIO::wasHomeKeyTapped() const { return homeKeyTappedThisFrame; }

bool HalGPIO::wasHomeKeyLongPressed() const {
  return homeKeyLongPressedThisFrame;
}

bool HalGPIO::wasTouchTap(float &nx, float &ny) const {
  if (!touchState.releasedThisFrame || touchState.movedBeyondTapSlop ||
      touchState.suppressed)
    return false;
  nx = touchState.startNx;
  ny = touchState.startNy;
  return true;
}

bool HalGPIO::wasTouchDown(float &nx, float &ny) const {
  if (!touchState.pressedThisFrame)
    return false;
  nx = touchState.startNx;
  ny = touchState.startNy;
  return true;
}

bool HalGPIO::wasTouchReleased() const { return touchState.releasedThisFrame; }

bool HalGPIO::isTouchTapCandidate(float &nx, float &ny,
                                  unsigned long &heldMs) const {
  if (!touchState.down || touchState.movedBeyondTapSlop ||
      touchState.suppressed) {
    heldMs = 0;
    return false;
  }
  nx = touchState.startNx;
  ny = touchState.startNy;
  heldMs = SDL_GetTicks() - touchState.pressedAt;
  return true;
}

bool HalGPIO::isTouchHeldAt(float &nx, float &ny) const {
  if (!touchState.down || touchState.suppressed)
    return false;
  nx = touchState.currentNx;
  ny = touchState.currentNy;
  return true;
}

bool HalGPIO::wasTouchLongPress(float &nx, float &ny) const {
  if (!touchState.longPressThisFrame || touchState.suppressed)
    return false;
  nx = touchState.startNx;
  ny = touchState.startNy;
  return true;
}

void HalGPIO::suppressTouchContact() {
  if (touchState.down || touchState.releasedThisFrame)
    touchState.suppressed = true;
}

unsigned long HalGPIO::lastTouchHeldMs() const { return touchState.lastHeldMs; }

bool HalGPIO::wasSwipe(float &nxStart, float &nyStart, float &nxEnd,
                       float &nyEnd) const {
  if (!touchState.releasedThisFrame || touchState.suppressed ||
      touchState.lastHeldMs > TOUCH_SWIPE_MAX_MS)
    return false;
  const float dx =
      (touchState.currentNx - touchState.startNx) * HalDisplay::DISPLAY_WIDTH;
  const float dy =
      (touchState.currentNy - touchState.startNy) * HalDisplay::DISPLAY_HEIGHT;
  if (std::abs(dx) < TOUCH_SWIPE_MIN_PX && std::abs(dy) < TOUCH_SWIPE_MIN_PX)
    return false;
  nxStart = touchState.startNx;
  nyStart = touchState.startNy;
  nxEnd = touchState.currentNx;
  nyEnd = touchState.currentNy;
  return true;
}

bool HalGPIO::wasTouchActivity() const { return touchState.activityThisFrame; }
void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(bool /*enabled*/) {}

bool HalGPIO::consumeSimulatorSleepRequest() {
  const bool requested = simulatorSleepRequested;
  simulatorSleepRequested = false;
  return requested;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  if (SimulatorLifecycle::consumeWakeReason() ==
      SimulatorLifecycle::WakeReason::PowerButton) {
    return WakeupReason::PowerButton;
  }
  return WakeupReason::Other;
}
bool HalGPIO::isUsbConnected() const { return true; }
bool HalGPIO::wasUsbStateChanged() const { return false; }
void HalGPIO::startDeepSleep() {
  clearButtonState();

  while (true) {
    processSyntheticEvents();
    if (quitRequested.load())
      return;
    for (int button = 0; button < NUM_BUTTONS; button++) {
      if (syntheticButtonDown[button]) {
        clearButtonState();
        SimulatorLifecycle::rebootAsPowerWake();
      }
    }

    SDL_Event e;
    while (SDL_PollEvent(&e) != 0) {
      if (e.type == SDL_QUIT) {
        quitRequested.store(true);
        return;
      }

      if (e.type == SDL_KEYDOWN && !e.key.repeat &&
          scancodeToButton(e.key.keysym.scancode) >= 0) {
        clearButtonState();
        SimulatorLifecycle::rebootAsPowerWake();
      }
    }

    SDL_Delay(10);
  }
}
bool HalGPIO::verifyPowerButtonWakeup(uint16_t /*requiredDurationMs*/,
                                      bool /*shortPressAllowed*/) {
  return true;
}

HalGPIO gpio;
