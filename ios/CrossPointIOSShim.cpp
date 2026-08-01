// CrossPoint X3 -> iPhone harness.
//
// THE MODEL. The iPhone impersonates X3 peripherals; it is not a new CrossPoint
// board. There are two surfaces and exactly one translation point between them:
//
//   harness layer (this file)  draws an on-screen button pad and reads touches
//                              on it. Lives outside the simulated device.
//   device layer (HalGPIO)     sees only the X3's seven GPIO buttons, by index.
//
// The harness translates the first into the second by calling HalGPIO's
// platform-neutral live-injection API, gpio.injectButtonDown/Up(BTN_*). The
// device layer cannot tell an injected button from a keyboard one, so no
// #if TARGET_OS_IPHONE appears in HalGPIO or the firmware.
//
// hasTouch() stays false for X3 and iPhone touches become BUTTON events, never
// touch events. Hit-testing happens HERE, above SDL; no coordinate is ever handed
// to the firmware. Letting one reach the hasTouch() branch would make the
// firmware take X4-Pro-only paths and we would be testing a device that does not
// exist.
//
// ONE CONTROL PER PHYSICAL BUTTON, and nothing else. Each is down-on-touch and
// up-on-lift, so it expresses a genuine hold -- which is what page-turn
// autorepeat and long-press-to-sleep need.
//
// POWER is the one control whose GESTURE is widened, because a rectangle on
// glass is not a key you can lean on: a tap holds the injected button past the
// firmware's own sleep threshold instead of lifting with the finger. The device
// layer still sees nothing but an ordinary long press. See kSleepHoldMs.
//
// WHY AN EVENT WATCH, NOT A POLL LOOP. HalGPIO::update() owns the SDL event pump
// for the whole simulator and must keep owning it -- two pollers would split
// events between them. SDL_AddEventWatch observes events as they are queued
// without consuming them, so the harness sees finger events that HalGPIO simply
// ignores, and neither steals from the other.
//
// WHY NOT SDL_PushEvent. The pad used to inject by pushing synthetic
// SDL_EVENT_KEY_DOWN / _UP. Measured, not assumed: SDL_PushEvent delivers an
// event to the queue but does NOT update SDL's internal keyboard state array,
// which is written only on the real-input path. Edge reads (wasPressed /
// wasReleased) came off the dequeued event and worked; every level read
// (isPressed, getHeldTime, getPowerButtonHeldTime) consults
// SDL_GetKeyboardState() and stayed false, so nothing timed off a HELD button
// could fire -- long-press-to-sleep and the reader's font-family hold both died
// there. injectButtonDown/Up writes the press edge, the held level and the press
// timestamp together, so a hold expressed by a finger survives all the way down.

#include "CrossPointHarness.h"

#include <SDL3/SDL.h>

#include <cstdint>

#include "HalGPIO.h"
#include "SimulatorOverlay.h"

namespace {

// --- The X3's seven buttons ------------------------------------------------
//
// One control per HalGPIO::BTN_* index, and nothing else. There is no control
// for the simulator's own SLEEP (`S`): that is a harness command, not a button
// the hardware has. There is no HOME either -- hasHomeKey() is X4-Pro-only.
//
// PLACEMENT is specified, not derived. Nothing in this source tree encodes where
// the buttons physically sit on the X3 chassis -- the SDK describes them only
// electrically (six on a resistor ladder across two ADC pins, POWER on its own
// digital pin; BoardConfig InputPins, InputStyle::XteinkAdcLadder). See
// layoutPad() for the arrangement.
//
// SIZING follows Apple's Human Interface Guidelines: every control is at least
// 44x44 pt, targets are separated by >= 8 pt, and the bands are inset clear of
// the Dynamic Island at the top and the home indicator at the bottom.
//
// The controls are UNLABELLED -- no glyph, no text. The pad names nothing about
// what each button does, which puts the whole affordance on the pressed state;
// see the palette below for how that is paid for.
struct PadButton {
  uint8_t button;  // HalGPIO::BTN_*
  const char *name;
  SDL_FRect rect{};
  bool down = false;
  SDL_FingerID finger = 0;
};

PadButton g_pad[] = {
    {HalGPIO::BTN_BACK, "BACK"},
    {HalGPIO::BTN_POWER, "POWER"},
    {HalGPIO::BTN_UP, "UP"},
    {HalGPIO::BTN_LEFT, "LEFT"},
    {HalGPIO::BTN_CONFIRM, "CONFIRM"},
    {HalGPIO::BTN_RIGHT, "RIGHT"},
    {HalGPIO::BTN_DOWN, "DOWN"},
};
constexpr int kPadBack = 0, kPadPower = 1, kPadUp = 2, kPadLeft = 3,
              kPadConfirm = 4, kPadRight = 5, kPadDown = 6;
constexpr int kPadCount = 7;

bool g_padLaidOut = false;
float g_ptScale = 3.0f;
SDL_WindowID g_windowId = 0;

// The single translation point between the two layers. Called from the event
// watch, which runs inside SDL_PumpEvents inside HalGPIO::update() -- i.e. after
// beginFrame() has cleared the frame's edge latches and before the firmware
// reads them, exactly the window the SDL keyboard path writes in.
void injectButton(int padIndex, bool down) {
  if (down)
    gpio.injectButtonDown(g_pad[padIndex].button);
  else
    gpio.injectButtonUp(g_pad[padIndex].button);
}

// --- POWER: a tap sleeps, as well as a hold --------------------------------
//
// THE DEVICE SIDE IS A HOLD AND STAYS A HOLD. The firmware sleeps only once
// getPowerButtonHeldTime() passes SETTINGS.getPowerButtonDuration()
// (crosspoint-reader src/main.cpp:573-582). Nothing else on the device stops
// it: there is no power-off anywhere in the firmware or the SDK, because on an
// ESP32-C3 the only stop state is deep sleep (lib/hal/HalPowerManager.cpp:94)
// and waking from it is a chip reset.
//
// THE PHONE SIDE IS A TAP. A hardware key you can find by feel and lean on is
// not a rectangle on glass. A screen control that needs half a second of
// pressure before anything happens at all does not read as deliberate, it reads
// as broken -- there is no click, no travel and no detent to tell you it is
// working, and the pad is unlabelled, so nothing else offers to explain it.
//
// So the harness widens the GESTURE without inventing a device behaviour: on a
// tap the injected button STAYS DOWN until it has passed the firmware's own
// threshold, then lifts. HalGPIO sees one ordinary long press and the firmware
// runs its ordinary sleep path -- there is no second sleep mechanism here, and
// nothing in this file knows what sleep is. A real hold needs none of this: it
// passes the threshold before the finger lifts.
//
// kSleepHoldMs is read off the firmware rather than invented. 400ms is the
// largest value getPowerButtonDuration() can return (it is 400, or 10 when
// Settings > Controls > "Short power button" is set to Sleep --
// src/CrossPointSettings.h:309-311), and the firmware samples it once per
// loop() iteration, so the remaining 200ms is margin for a slow frame: a page
// render holds loop() far longer than its 1ms idle delay.
constexpr Uint32 kSleepHoldMs = 600;

Uint64 g_powerPressedAt = 0;
Uint32 g_deferredReleaseEvent = 0;  // SDL_RegisterEvents type; 0 until begin()
SDL_TimerID g_deferredReleaseTimer = 0;
Sint32 g_deferredReleaseGeneration = 0;

// Runs on SDL's timer thread, so it deliberately does NOT touch HalGPIO. Every
// other injection in this file happens on the thread that pumps events, and the
// three arrays behind injectButtonUp are not synchronised. Pushing an event is
// the thread-safe half; padWatch performs the injection when the event is
// pumped, on the same thread as everything else.
//
// The generation rides in the event rather than being read from the global here,
// so the timer thread reads nothing the event thread writes.
Uint32 SDLCALL deferredReleaseFired(void *userdata, SDL_TimerID, Uint32) {
  if (g_deferredReleaseEvent != 0) {
    SDL_Event e;
    SDL_zero(e);
    e.type = g_deferredReleaseEvent;
    e.user.code = static_cast<Sint32>(reinterpret_cast<intptr_t>(userdata));
    SDL_PushEvent(&e);
  }
  return 0;  // one-shot
}

void cancelDeferredRelease() {
  if (g_deferredReleaseTimer) {
    SDL_RemoveTimer(g_deferredReleaseTimer);
    g_deferredReleaseTimer = 0;
  }
  // Bumped unconditionally: a timer that has already fired cannot be removed,
  // and its event may still be sitting in the queue. The generation is what
  // stops that stale event from cutting short a press made since.
  g_deferredReleaseGeneration++;
}

bool anyOtherButtonDown(int except) {
  for (int i = 0; i < kPadCount; i++)
    if (i != except && g_pad[i].down) return true;
  return false;
}

// --- Layout ----------------------------------------------------------------
//
// All dimensions in points, converted once. HIG minimums are expressed in
// points, so laying out in pixels would silently shrink the targets on a device
// with a different scale factor.
// Two rows of five slots, bottom-aligned. Blank slots stay empty -- the grid is
// five wide so the occupied cells land where they do, not because there are ten
// controls:
//
//     Back    .     Power    .      Up
//     Left  Select   Right   .     Down
//
// The arrangement mirrors the chassis, not a gamepad: UP/DOWN are the X3's
// physical SIDE-BUTTON pair (firmware calls BTN_UP the "left side button";
// MappedInputManager keeps them fixed as page-turn/Up/Down), so they stack
// vertically on the right edge where a thumb rests -- Up above Down. The
// front-button group (Back, Confirm, Left, Right) fills the rest, with the
// Left/Select/Right nav run contiguous on the lower row.
//
// The lower row sits kHomeInset above the physical bottom edge: below that is
// the home indicator, and iOS both draws its swipe affordance there and eats
// touches near it -- taps that land short of a control read as false taps, so
// the pad keeps extra clearance beyond the bare safe-area minimum.
void layoutPad(int outW, int outH) {
  const float S = g_ptScale;
  const float W = static_cast<float>(outW) / S;
  const float H = static_cast<float>(outH) / S;

  constexpr float kMargin = 20.0f;     // side inset
  constexpr float kGap = 8.0f;         // minimum separation between targets
  constexpr float kRowGap = 16.0f;     // vertical separation between the rows
  constexpr float kRow = 46.0f;        // >= the 44 pt minimum target height
  constexpr float kHomeInset = 52.0f;  // clearance over the home indicator
  constexpr int kCols = 5;

  const float colW = (W - 2 * kMargin - (kCols - 1) * kGap) / kCols;
  float colX[kCols];
  for (int i = 0; i < kCols; i++) colX[i] = kMargin + i * (colW + kGap);

  const float lowerY = H - kHomeInset - kRow;
  const float upperY = lowerY - kRowGap - kRow;

  auto place = [&](int idx, int col, float y) {
    g_pad[idx].rect = {colX[col] * S, y * S, colW * S, kRow * S};
  };

  place(kPadBack, 0, upperY);
  place(kPadPower, 2, upperY);
  place(kPadUp, 4, upperY);

  place(kPadLeft, 0, lowerY);
  place(kPadConfirm, 1, lowerY);
  place(kPadRight, 2, lowerY);
  place(kPadDown, 4, lowerY);

  // Reserve the pad's band (plus breathing room) out of the panel's space, so
  // the panel -- and the button-hint bar the firmware draws along its bottom
  // edge -- always ends above the pad instead of underneath it.
  constexpr float kPadClearance = 12.0f;
  SimulatorOverlay::setBottomInset(
      static_cast<int>(static_cast<float>(outH) - (upperY - kPadClearance) * S));
}

// --- Appearance ------------------------------------------------------------
//
// Apple's system greys at the low-contrast end, so the chrome recedes and the
// e-ink panel stays the subject. Both appearances, published values:
// systemBackground field, systemGray6 face, systemGray5 hairline, systemGray4
// while held.
//
// THE PRESSED STATE CARRIES EVERYTHING. With the glyphs gone it is the only
// feedback a control has, so it moves TWO steps along the ramp (6 -> 4) rather
// than one, and it moves towards the foreground in each appearance -- darker in
// light, lighter in dark. One step (6 -> 5) is 13/255 in light mode and is not
// reliably visible on a phone; it is also exactly the hairline colour, which
// would flatten the whole control into one tone.
struct Palette {
  Uint8 field[3];     // behind the panel and the pad: systemBackground
  Uint8 hairline[3];  // button border: systemGray5
  Uint8 face[3];      // button face: systemGray6
  Uint8 faceDown[3];  // button face while held: systemGray4
};

// The field matches the panel's paper tone (HalDisplay's PanelPalette:
// 2D2D2D-on-FBFBF9 light, E0E0DE-on-121212 dark — change them together), so
// the page floats edgeless in the field in both appearances. The button greys
// stay Apple's system ramp.
constexpr Palette kLightPalette{{0xFB, 0xFB, 0xF9},
                                {0xE5, 0xE5, 0xEA},
                                {0xF2, 0xF2, 0xF7},
                                {0xD1, 0xD1, 0xD6}};
constexpr Palette kDarkPalette{{0x12, 0x12, 0x12},
                               {0x2C, 0x2C, 0x2E},
                               {0x1C, 0x1C, 0x1E},
                               {0x3A, 0x3A, 0x3C}};

bool g_dark = false;
const Palette &palette() { return g_dark ? kDarkPalette : kLightPalette; }

// THE FIELD AND THE PANEL BOTH FOLLOW THE APPEARANCE.
//
// In light mode the field is white because a blank e-ink page is white, so the
// panel edge disappears. In dark mode the field goes to systemBackground dark
// and the panel renders white-on-black: this app is a reading surface first
// and a simulator second, and a full-brightness white page inside a dark UI is
// exactly what dark appearance exists to prevent.
//
// The inversion is a HOST presentation choice layered on the device's output,
// not a device behaviour -- a fact worth keeping straight, both halves checked
// rather than assumed: no X3 can invert its panel, and nothing in the firmware
// or the SDK calls setInverted/toggleInverted/isInverted; the trio exists only
// in the simulator's HalDisplay, which applies the flip while converting the
// 1bpp framebuffer to pixels. The device layer keeps drawing black-on-white
// and cannot tell the difference, so no firmware path changes underneath us.
//
// Immediacy lives inside HalDisplay, not here. Conversion runs on the render
// task only when the firmware refreshes, so flipping the flag alone would not
// show until the next page render -- which on e-ink may be never. setInverted
// therefore posts an atomic reconvert request that presentIfNeeded (main
// thread) services from HalDisplay's cached last frame, so the new polarity
// lands on the very next present. SimulatorOverlay::setPanelDark is the single
// entry point; it also honours the CROSSPOINT_SIM_DARK override, which is what
// lets the headless desktop tests drive the exact mechanics this path uses.
//
// Known cost, accepted for now: inversion is polarity-blind, so book covers
// and other images render as negatives in dark mode.
void applyTheme() {
  g_dark = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
  const Palette &p = palette();
  SimulatorOverlay::setClearColor(p.field[0], p.field[1], p.field[2]);
  SimulatorOverlay::setPanelDark(g_dark);
  // The firmware presents only when it has new panel content, which on an e-ink
  // device is rare, so without this the new appearance would not appear until
  // the next page render. (setPanelDark's reconvert also raises a present, but
  // only when the polarity actually changed; the field colour must repaint
  // regardless.)
  SimulatorOverlay::requestPresent();
}

// A watch of its own, deliberately not a case inside padWatch: this is a
// painting concern and reads no input. SDL raises the theme change from UIKit's
// traitCollectionDidChange on the UI thread, and applyTheme only writes a flag
// and a handful of atomics -- no renderer call happens here; the reconvert and
// repaint both run later on the main thread inside presentIfNeeded.
bool SDLCALL themeWatch(void * /*userdata*/, SDL_Event *e) {
  if (e->type == SDL_EVENT_SYSTEM_THEME_CHANGED) applyTheme();
  return true;  // never filter anything out
}

// --- Painting --------------------------------------------------------------

void setRGB(SDL_Renderer *r, const Uint8 c[3]) {
  SDL_SetRenderDrawColor(r, c[0], c[1], c[2], 255);
}

void fillRect(SDL_Renderer *r, float x, float y, float w, float h) {
  const SDL_FRect rect{x, y, w, h};
  SDL_RenderFillRect(r, &rect);
}

void fillRoundRect(SDL_Renderer *r, const SDL_FRect &b, float rad) {
  const int h = static_cast<int>(b.h);
  for (int i = 0; i < h; i++) {
    const float y = static_cast<float>(i);
    float inset = 0.0f;
    if (y < rad) {
      const float d = rad - y;
      inset = rad - SDL_sqrtf(SDL_max(0.0f, rad * rad - d * d));
    } else if (y > b.h - rad) {
      const float d = y - (b.h - rad);
      inset = rad - SDL_sqrtf(SDL_max(0.0f, rad * rad - d * d));
    }
    fillRect(r, b.x + inset, b.y + y, b.w - 2 * inset, 1);
  }
}

void paintPad(SDL_Renderer *r, int outW, int outH) {
  if (!g_padLaidOut) {
    layoutPad(outW, outH);
    g_padLaidOut = true;
  }
  const Palette &p = palette();
  const float S = g_ptScale;
  const float radius = 12.0f * S;
  const float hairline = SDL_max(1.0f, S * 0.5f);

  // A hairline ring with the face inset inside it. The ring stays put while
  // held, so the face changing tone reads as the control moving rather than as
  // the control being redrawn.
  for (const PadButton &b : g_pad) {
    setRGB(r, p.hairline);
    fillRoundRect(r, b.rect, radius);

    const SDL_FRect inner{b.rect.x + hairline, b.rect.y + hairline,
                          b.rect.w - 2 * hairline, b.rect.h - 2 * hairline};
    setRGB(r, b.down ? p.faceDown : p.face);
    fillRoundRect(r, inner, radius - hairline);
  }
}

int padHitTest(float x, float y) {
  for (int i = 0; i < kPadCount; i++) {
    const SDL_FRect &r = g_pad[i].rect;
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return i;
  }
  return -1;
}

void releaseButton(int i) {
  if (!g_pad[i].down) return;
  // Whatever the reason for this release -- finger, drag-off, backgrounding or
  // the deferred timer itself -- any pending deferred release is now spent.
  if (i == kPadPower) cancelDeferredRelease();
  g_pad[i].down = false;
  injectButton(i, false);
  SimulatorOverlay::requestPresent();
  SDL_Log("[harness] %s up", g_pad[i].name);
}

// Called when the finger leaves POWER. Returns true if the lift was a tap and
// the button has been LEFT DOWN to finish becoming a hold; the caller must then
// not release it.
//
// The control keeps its pressed appearance for that window. That is not a
// cosmetic choice -- the device's button really is still down -- and it is the
// only feedback that a tap was taken at all, since the sleep screen does not
// appear until the firmware crosses its threshold.
//
// Every failure here falls back to releasing normally, so the worst case is the
// old hold-only behaviour rather than a button stuck down.
bool holdPowerForSleep() {
  if (g_deferredReleaseEvent == 0) return false;
  // A tap that is part of a chord must not be stretched. POWER+DOWN is the
  // firmware's screenshot combo (main.cpp:543-552), and it is the POWER RELEASE
  // that ends it (main.cpp:554-563); holding POWER past the finger would leave
  // the combo latched and suppress the release the firmware is waiting for.
  if (anyOtherButtonDown(kPadPower)) return false;
  const Uint64 held = SDL_GetTicks() - g_powerPressedAt;
  if (held >= kSleepHoldMs) return false;  // already a hold; nothing to add

  cancelDeferredRelease();  // also advances the generation used below
  g_deferredReleaseTimer = SDL_AddTimer(
      kSleepHoldMs - static_cast<Uint32>(held), deferredReleaseFired,
      reinterpret_cast<void *>(
          static_cast<intptr_t>(g_deferredReleaseGeneration)));
  if (!g_deferredReleaseTimer) {
    SDL_Log("[harness] SDL_AddTimer failed: %s", SDL_GetError());
    return false;
  }
  SDL_Log("[harness] POWER tap (%llums) held to %ums",
          static_cast<unsigned long long>(held), kSleepHoldMs);
  return true;
}

// Finger coordinates arrive normalised; the pad needs pixels, and the harness
// does not own the renderer, so it asks the window the event came from.
bool windowPixelSize(SDL_WindowID id, float *w, float *h) {
  SDL_Window *win = SDL_GetWindowFromID(id ? id : g_windowId);
  if (!win) return false;
  int pw = 0, ph = 0;
  if (!SDL_GetWindowSizeInPixels(win, &pw, &ph) || pw <= 0 || ph <= 0)
    return false;
  *w = static_cast<float>(pw);
  *h = static_cast<float>(ph);
  return true;
}

bool SDLCALL padWatch(void * /*userdata*/, SDL_Event *e) {
  float outW = 0, outH = 0;

  switch (e->type) {
    case SDL_EVENT_FINGER_DOWN: {
      if (!windowPixelSize(e->tfinger.windowID, &outW, &outH)) break;
      const int hit = padHitTest(e->tfinger.x * outW, e->tfinger.y * outH);
      if (hit < 0 || g_pad[hit].down) break;
      g_windowId = e->tfinger.windowID;
      g_pad[hit].down = true;
      g_pad[hit].finger = e->tfinger.fingerID;
      if (hit == kPadPower) g_powerPressedAt = SDL_GetTicks();
      injectButton(hit, true);
      SimulatorOverlay::requestPresent();
      SDL_Log("[harness] %s down", g_pad[hit].name);
      break;
    }

    case SDL_EVENT_FINGER_MOTION: {
      // Dragging off a control cancels it, matching how a system button behaves
      // and how a real key behaves when your thumb slides off it.
      if (!windowPixelSize(e->tfinger.windowID, &outW, &outH)) break;
      const float x = e->tfinger.x * outW;
      const float y = e->tfinger.y * outH;
      for (int i = 0; i < kPadCount; i++) {
        if (!g_pad[i].down || g_pad[i].finger != e->tfinger.fingerID) continue;
        const SDL_FRect &r = g_pad[i].rect;
        if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h)
          releaseButton(i);
        break;
      }
      break;
    }

    case SDL_EVENT_FINGER_UP: {
      for (int i = 0; i < kPadCount; i++) {
        if (!g_pad[i].down || g_pad[i].finger != e->tfinger.fingerID) continue;
        // POWER may outlive the finger; see holdPowerForSleep.
        if (i != kPadPower || !holdPowerForSleep()) releaseButton(i);
        break;
      }
      break;
    }

    // Backgrounding must not leave a key stuck down: the finger is gone, and a
    // stuck POWER would read as a long press.
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_WINDOW_FOCUS_LOST: {
      for (int i = 0; i < kPadCount; i++) releaseButton(i);
      break;
    }

    // The pad is laid out from the output size, so a size change invalidates it.
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
      g_padLaidOut = false;
      SimulatorOverlay::requestPresent();
      break;
    }

    // The end of a POWER tap's borrowed hold. Not a case label: the type is
    // assigned at runtime by SDL_RegisterEvents. The generation check discards
    // an event whose timer was overtaken by a release that already happened.
    default:
      if (g_deferredReleaseEvent != 0 && e->type == g_deferredReleaseEvent &&
          e->user.code == g_deferredReleaseGeneration) {
        releaseButton(kPadPower);
      }
      break;
  }
  return true;  // never filter anything out
}

}  // namespace

// --- Public entry points ---------------------------------------------------
//
// CrossPointHarness_prepareFilesystem lives in CrossPointFsPrep.cpp: it is
// plain POSIX and is compiled and exercised on a desktop host, which the
// SDL-facing code in this file cannot be.

void CrossPointHarness_begin() {
  // Touches must arrive as finger events only. Left on, SDL also synthesises
  // mouse events from the same touch, and HalGPIO consumes mouse events.
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");

  // Points-to-pixels, so HIG dimensions stay honest on any device scale.
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  if (windows && count > 0) {
    g_windowId = SDL_GetWindowID(windows[0]);
    int pw = 0, ph = 0, lw = 0, lh = 0;
    SDL_GetWindowSizeInPixels(windows[0], &pw, &ph);
    SDL_GetWindowSize(windows[0], &lw, &lh);
    if (lw > 0 && pw > 0) g_ptScale = static_cast<float>(pw) / lw;
    SDL_Log("[harness] window %dx%d pt, %dx%d px, scale %.2f", lw, lh, pw, ph,
            g_ptScale);
  }
  if (windows) SDL_free(windows);

  SimulatorOverlay::setDrawCallback(paintPad);

  // Appearance. SDL_Init has already run (HalDisplay::begin calls it), so the
  // theme is populated and can be read straight away; the watch keeps it current
  // if the user flips the system between light and dark while the app is up.
  applyTheme();
  if (!SDL_AddEventWatch(themeWatch, nullptr))
    SDL_Log("[harness] theme watch failed: %s", SDL_GetError());
  SDL_Log("[harness] appearance: %s", g_dark ? "dark" : "light");

  SimulatorOverlay::requestPresent();

  // The POWER tap's deferred release travels as a registered user event so the
  // injection lands on the pumping thread. Registered before the watch, since
  // the watch compares against it. 0 means registration failed, and every user
  // of it falls back to releasing POWER with the finger.
  g_deferredReleaseEvent = SDL_RegisterEvents(1);
  if (g_deferredReleaseEvent == 0 ||
      g_deferredReleaseEvent == static_cast<Uint32>(-1)) {
    g_deferredReleaseEvent = 0;
    SDL_Log("[harness] SDL_RegisterEvents failed; POWER tap-to-sleep disabled");
  }

  if (!SDL_AddEventWatch(padWatch, nullptr))
    SDL_Log("[harness] SDL_AddEventWatch failed: %s", SDL_GetError());
  else
    SDL_Log("[harness] button pad installed");
}
