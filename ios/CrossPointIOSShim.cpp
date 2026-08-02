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
// PURE PASSTHROUGH, enforced by construction: the finger->button decisions
// live in PadCore (ios/PadCore.h), whose API cannot express time, so no
// gesture is ever widened, stretched, or delayed. (A POWER "tap-to-sleep"
// stretch used to live here; it kept the injected button down 600 ms past the
// finger, which read as a stuck control and desynced the pad from the device.
// Sleep is a real 400 ms hold now, exactly like the hardware.)
//
// The grabber is the one timed gesture on this surface, and it does not dent
// that rule: it moves the pad's own chrome, injects nothing, and its finger is
// claimed in padWatch before PadCore is consulted, so PadCore stays clock-free
// and every BUTTON remains a straight passthrough. If a future gesture needs a
// timer, it belongs out here for the same reason -- never in PadCore.
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

#include "CrossPointAppearance.h"
#include "HalGPIO.h"
#include "PadCore.h"
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

// All finger->button decisions. The SDL adapter below owns NO button state:
// PadCore decides, applyActions() injects. Unit-tested in
// tests/pad_core_test.cpp.
PadCore g_core(kPadCount);

// --- The grabber -----------------------------------------------------------
//
// A drag handle in the empty column right of POWER. Tap-hold it and both button
// rows follow the finger down the field, so the pad can be moved under the
// thumb without moving the page.
//
// IT IS NOT AN EIGHTH BUTTON. It never reaches applyActions(), never injects a
// GPIO event, and PadCore never sees its finger -- padWatch claims the touch
// before PadCore is consulted. That separation is what makes the hold legal at
// all: PadCore is clock-free by construction so no BUTTON gesture can be
// widened or delayed (a POWER tap-stretch once was, and read as a stuck
// control). The grabber is chrome, so its timer lives out here and PadCore's
// guarantee is untouched.
//
// THE PANEL DOES NOT MOVE, and neither does the reserved band -- the panel's
// size is computed from that band, so a band that slid with the pad would
// resize the page mid-drag. The rows just travel within the space they already
// have: offset 0 is hugging the panel, and the maximum is where the lower row
// reaches the home-indicator inset. layoutPad owns that clamp, because it is
// the only place that knows where the panel ended up.
SDL_FRect g_gripRect{};       // device px; hit-tested and painted
float g_padOffsetY = 0.0f;    // points below the hug position
float g_padOffsetMax = 0.0f;  // points, recomputed every layout
SDL_FingerID g_gripFinger = 0;
bool g_gripHeld = false;      // a finger is on the grabber
bool g_gripDragging = false;  // the hold landed; rows are following
Uint64 g_gripDownAtMs = 0;
float g_gripStartYPx = 0.0f;
float g_gripStartOffset = 0.0f;

// Long enough that brushing the grabber on the way to DOWN cannot move the pad,
// short enough that it does not feel stuck.
constexpr Uint64 kGripHoldMs = 350;

// The single translation point between the two layers. Called from the event
// watch, which runs inside SDL_PumpEvents inside HalGPIO::update() -- i.e. after
// beginFrame() has cleared the frame's edge latches and before the firmware
// reads them, exactly the window the SDL keyboard path writes in.
void applyActions(const std::vector<PadCore::Action> &actions) {
  for (const PadCore::Action &a : actions) {
    if (a.type == PadCore::Action::Press)
      gpio.injectButtonDown(g_pad[a.slot].button);
    else
      gpio.injectButtonUp(g_pad[a.slot].button);
    SDL_Log("[harness] %s %s", g_pad[a.slot].name,
            a.type == PadCore::Action::Press ? "down" : "up");
  }
  if (!actions.empty()) SimulatorOverlay::requestPresent();
}

// --- Layout ----------------------------------------------------------------
//
// All dimensions in points, converted once. HIG minimums are expressed in
// points, so laying out in pixels would silently shrink the targets on a device
// with a different scale factor.
// Two rows of 60 pt squares, anchored directly under the panel's bottom edge
// (owner-approved mockup A + fused pairs, 2026-08-01):
//
//     [Up]      [Power][ :: ][Down]          <- side pair + power, plus the
//     [Back|Select]  [Left|Right]               grabber in the empty fourth
//                                               column; front buttons are two
//                                               fused rockers, as on the chassis
//
// Columns touch, so the grabber's column sits flush between POWER and DOWN --
// it is drawn as bare dots with no ring precisely so that run of three cells
// still reads as two buttons with a handle between them, not three buttons.
//
// UP/DOWN are the X3's SIDE buttons (MappedInputManager keeps them fixed as
// page-turn/Up/Down; main.cpp calls BTN_UP the "left side button").
// BACK/SELECT/LEFT/RIGHT are the FRONT buttons (the remappable frontButton*
// set). On the device each front pair is one rocker, so here each pair shares
// an edge -- no gap inside a pair, a wide gap between pairs.
//
// The pad hugs the panel (SimulatorOverlay::panelBottomPx) so thumbs rest at
// the page, not at the screen's bottom edge; before the first present it
// falls back to bottom-anchoring clear of the home indicator.
void layoutPad(int outW, int outH) {
  const float S = g_ptScale;
  const float W = static_cast<float>(outW) / S;
  const float H = static_cast<float>(outH) / S;

  constexpr float kMargin = 20.0f;      // side inset
  constexpr float kMinSquare = 60.0f;   // owner-picked target size, the floor
  constexpr float kMaxSquare = 96.0f;   // guard only, see note below
  // ONE vertical gap, used both under the panel and between the two rows, so
  // the pad reads as evenly spaced stack rather than three different rhythms.
  // These were 12 and 16; keep them equal.
  constexpr float kGap = 16.0f;
  constexpr float kRowGap = kGap;       // row -> row
  constexpr float kPanelGap = kGap;     // panel -> first row
  constexpr float kHomeInset = 34.0f;   // never sink into the home indicator
  constexpr float kBelowPad = 24.0f;    // slack under the lower row

  // The pad is a 5x2 grid of SQUARE cells: three singles on the upper row
  // (columns 1/3/5) and two fused rockers on the lower one (columns 1-2 and
  // 4-5), so a rocker is two cells wide. The cell is the largest square that
  // fits five across the row, which is what makes the grid real rather than
  // implied -- at a fixed 60 the row was edge-anchored and ~80pt of slack
  // pooled in the middle gaps.
  //
  // The cell sets the band height (below), the band comes out of the panel's
  // space, and the panel scale is floored to an integer
  // (CROSSPOINT_SIM_PIXEL_EXACT) -- so a cell too big costs the page a whole
  // scale step. kMaxSquare guards that, but on this app it never binds: the
  // build is iPhone-only and portrait-only (TARGETED_DEVICE_FAMILY 1,
  // UISupportedInterfaceOrientations = Portrait), so the column width runs
  // 67pt (SE / 13 mini) to 80pt (16 Pro Max), and a sweep to 160pt regressed
  // none of them.
  //
  // It exists because the margin is NOT universal. The panel is portrait
  // whatever the device does -- the firmware has no landscape -- so a landscape
  // window is wider but SHORTER, and the portrait page has less room, not more.
  // On iPad that bites: with this band stacked below the panel, every iPad in
  // landscape falls to 1x (a 264x396pt page), and only the 13" Pro can hold 2x,
  // and only at a cell of 82 or less. If iPad (family 2) is ever enabled, the
  // fix is not a smaller cell -- it is to put the pad BESIDE the panel in a
  // landscape window, where the spare width is, so the band stops costing
  // height at all.
  const float kSquare =
      SDL_max(kMinSquare, SDL_min((W - 2 * kMargin) / 5.0f, kMaxSquare));

  // The pad must stay fully above the home-indicator zone, grabbed or not.
  const float maxUpper = H - kHomeInset - 2 * kSquare - kRowGap;

  const int panelBottomPx = SimulatorOverlay::panelBottomPx();
  float upperY;
  if (panelBottomPx > 0) {
    upperY = static_cast<float>(panelBottomPx) / S + kPanelGap;
    if (upperY > maxUpper) upperY = maxUpper;
  } else {
    upperY = H - kHomeInset - 2 * kSquare - kRowGap - kSquare / 2.0f;
  }
  // Apply the grabber's travel. Offset 0 is the hug position computed above;
  // the ceiling is the home-indicator clamp, so the rows can slide down into
  // the field's slack and no further. Clamped here rather than in the drag
  // handler because this is the only place that knows where the panel landed.
  g_padOffsetMax = SDL_max(0.0f, maxUpper - upperY);
  g_padOffsetY = SDL_clamp(g_padOffsetY, 0.0f, g_padOffsetMax);
  upperY += g_padOffsetY;

  const float lowerY = upperY + kSquare + kRowGap;

  auto place = [&](int idx, float x, float y) {
    g_pad[idx].rect = {x * S, y * S, kSquare * S, kSquare * S};
  };

  // Upper row: side pair at the edges, power centred.
  place(kPadUp, kMargin, upperY);
  place(kPadPower, (W - kSquare) / 2.0f, upperY);
  place(kPadDown, W - kMargin - kSquare, upperY);

  // The grabber takes the empty fourth column, between POWER and DOWN. A full
  // cell so the target clears the 44pt HIG minimum even though the dots drawn
  // in it are small.
  g_gripRect = {(kMargin + 3 * kSquare) * S, upperY * S, kSquare * S,
                kSquare * S};

  // Lower row: two fused rockers, flush left and right.
  place(kPadBack, kMargin, lowerY);
  place(kPadConfirm, kMargin + kSquare, lowerY);
  place(kPadLeft, W - kMargin - 2 * kSquare, lowerY);
  place(kPadRight, W - kMargin - kSquare, lowerY);

  // Reserve a fixed band for the pad out of the panel's space. Fixed (not
  // derived from upperY) because the panel's own placement depends on this
  // inset -- a constant keeps the two from chasing each other.
  SimulatorOverlay::setBottomInset(static_cast<int>(
      (kPanelGap + 2 * kSquare + kRowGap + kBelowPad) * S));

  // Keep the page clear of the status bar and the Dynamic Island. The panel's
  // manual fit is top-aligned, so without a top band it starts at the very top
  // of the screen and the first lines render under the cut-out. Taken from the
  // system rather than a constant like kHomeInset because the top inset is what
  // varies most across devices (Island vs notch vs neither). SDL reports the
  // safe area in window (point) coordinates, hence the * S into device pixels.
  int topInsetPx = 0;
  if (SDL_Window *win = SDL_GetWindowFromID(g_windowId)) {
    SDL_Rect safe{};
    if (SDL_GetWindowSafeArea(win, &safe) && safe.y > 0)
      topInsetPx = static_cast<int>(safe.y * S);
  }
  SimulatorOverlay::setTopInset(topInsetPx);
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
// The live appearance. UIKit first, SDL only as a fallback -- SDL's theme is a
// cache refreshed from a deprecated callback that an SDL app does not reliably
// receive, so it is stale exactly when it matters. See CrossPointAppearance.h.
bool systemIsDark() {
  const int uikit = CrossPointAppearance_isDark();
  if (uikit >= 0) return uikit != 0;
  return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
}

// What applyTheme last published. -1 = nothing yet, so the first call always
// applies. Read by pollAppearance to stay edge-triggered; kept here rather than
// as a local static in the poll so that applyTheme's other callers (startup and
// the SDL theme watch) also satisfy the edge and cannot cause a double apply.
int g_appliedDark = -1;

void applyTheme() {
  g_dark = systemIsDark();
  g_appliedDark = g_dark ? 1 : 0;
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

// When the app last returned to the foreground, on the SDL_GetTicks clock, or 0
// once the settle window below has elapsed. See presentationWatch.
Uint64 g_foregroundAt = 0;

// A watch of its own, deliberately not a case inside padWatch: everything here
// is a painting concern and none of it reads input. Both cases only write a
// flag and a handful of atomics -- no renderer call happens on this thread; the
// reconvert and the repaint run later on the main thread inside
// presentIfNeeded.
bool SDLCALL presentationWatch(void * /*userdata*/, SDL_Event *e) {
  switch (e->type) {
  case SDL_EVENT_SYSTEM_THEME_CHANGED:
    // SDL raises this from UIKit's traitCollectionDidChange. Kept because it
    // costs nothing and is the correct mechanism, but it is not load-bearing
    // any more -- pollAppearance below reads UIKit directly every frame.
    applyTheme();
    break;
  case SDL_EVENT_DID_ENTER_FOREGROUND:
    g_foregroundAt = SDL_GetTicks();
    SimulatorOverlay::requestPresent();
    break;
  default:
    break;
  }
  return true;  // never filter anything out
}

// BELT AND BRACES for the theme case of the watch above.
//
// SDL raises SDL_EVENT_SYSTEM_THEME_CHANGED from UIKit's traitCollectionDidChange
// on its own view controller, which is deprecated as of iOS 17 (Apple's
// replacement is registerForTraitChanges:withHandler:) and which UIKit only
// delivers as part of a view update pass -- something an SDL app, drawing
// straight through Metal, has no reason to run.
//
// HONEST ABOUT WHAT WAS MEASURED, because the comment is worth more than the
// theory: on iOS 26.5 that callback still fires, and SDL's cached theme was
// never once observed disagreeing with UIKit (sampled at 1 Hz across repeated
// flips, including flips made while backgrounded). So this poll is not
// correcting a wrong answer today -- it is removing the dependency on a
// deprecated callback, and it applies the change within one frame of the app
// resuming rather than up to a second later, which is when SDL's event arrived.
// The watch stays installed for the same reason in reverse: it costs nothing and
// it is the right mechanism if SDL ever adopts registerForTraitChanges.
//
// EDGE-TRIGGERED, and it has to be. applyTheme writes atomics and calls
// requestPresent(); running it every frame would force a present every frame on
// a panel whose whole presentation model assumes it presents rarely. The steady
// state here is one UIKit read and an integer compare.
//
// Main thread only -- it is called from main()'s loop alongside presentIfNeeded()
// for the same reason that one is. It reads no SDL events, so HalGPIO keeps sole
// ownership of the pump, and it holds no timer, so nothing here can drift into
// PadCore's clock-free territory.
void pollAppearance() {
  const int want = systemIsDark() ? 1 : 0;
  if (want == g_appliedDark) return;
  applyTheme();
  SDL_Log("[harness] appearance -> %s", g_dark ? "dark" : "light");
}

// THE FIRST FRAMES AFTER A FOREGROUND RETURN ARE THROWN AWAY, so keep asking.
//
// This is the half of the stale-appearance bug that detection alone does not
// fix, and it is not appearance-specific: iOS suspends the process while the app
// is backgrounded and shows a snapshot of the last frame during the return
// transition, and a frame presented into that transition never reaches the
// glass. Measured on iOS 26.5, with the presentation path instrumented:
// SDL_RenderPresent returns success (driver=metal) at resume+65 ms and the
// screen keeps the pre-background image; the same present a second or so later
// lands. An app that redraws continuously never notices, because its next frame
// is a sixtieth of a second away. This one presents ONLY when the panel changes,
// so the discarded frame is the only frame there will be, and the stale image
// stands until the user touches a control -- which is exactly the reported
// symptom.
//
// So: after SDL_EVENT_DID_ENTER_FOREGROUND, re-ask for a present on a slow
// cadence until the window has settled. Each one re-uploads a cached frame on an
// otherwise idle screen; a dozen of them, once per foreground return, is not a
// cost worth optimising. The window is bounded and clears itself.
//
// The constants are measured, not guessed: repaints at 200 ms intervals were
// logged against timed screenshots, the screen had caught up by the +1 s
// screenshot, and 2 s leaves margin for hardware slower than the Simulator.
// Do not tighten these to the measured minimum -- the failure mode is silent and
// only visible to the user.
constexpr Uint64 kForegroundSettleMs = 2000;
constexpr Uint64 kForegroundRepaintMs = 200;

void repaintAfterForeground() {
  if (g_foregroundAt == 0) return;  // steady state: one load and a compare
  const Uint64 now = SDL_GetTicks();
  if (now - g_foregroundAt > kForegroundSettleMs) {
    g_foregroundAt = 0;
    return;
  }
  static Uint64 lastRepaint = 0;
  if (now - lastRepaint < kForegroundRepaintMs) return;
  lastRepaint = now;
  SimulatorOverlay::requestPresent();
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
  // Relayout when the panel's published bottom edge moves (first present,
  // orientation change) as well as on size changes.
  static int s_layoutPanelBottom = -1;
  const int panelBottom = SimulatorOverlay::panelBottomPx();
  if (!g_padLaidOut || panelBottom != s_layoutPanelBottom) {
    layoutPad(outW, outH);
    g_padLaidOut = true;
    s_layoutPanelBottom = panelBottom;
  }
  const Palette &p = palette();
  const float S = g_ptScale;
  const float radius = 12.0f * S;
  const float hairline = SDL_max(1.0f, S * 0.5f);

  // A hairline ring with the face inset inside it. The ring stays put while
  // held, so the face changing tone reads as the control moving rather than as
  // the control being redrawn. Pressed paint comes straight from PadCore's
  // finger state -- the moment no finger holds a control, it paints released.

  // Fill one half of a rocker: rounded on the outer end, SQUARE on the shared
  // edge -- a rounded fill then a square patch over the inner end's corners.
  auto fillHalf = [&](const SDL_FRect &half, bool leftHalf, float rad) {
    fillRoundRect(r, half, rad);
    const SDL_FRect patch{leftHalf ? half.x + half.w - rad : half.x, half.y,
                          rad, half.h};
    SDL_RenderFillRect(r, &patch);
  };

  // The two front-button pairs paint as ONE capsule each -- a single hairline
  // ring around the union with only the outer corners rounded (no pinched
  // notch where two rounded squares would meet), a hairline divider marking
  // the two targets, and the pressed half shading independently.
  const int pairs[2][2] = {{kPadBack, kPadConfirm}, {kPadLeft, kPadRight}};
  bool inPair[kPadCount] = {};
  for (const auto &pr : pairs) {
    const SDL_FRect &a = g_pad[pr[0]].rect;
    const SDL_FRect &b = g_pad[pr[1]].rect;
    inPair[pr[0]] = inPair[pr[1]] = true;

    const SDL_FRect uni{a.x, a.y, (b.x + b.w) - a.x, a.h};
    setRGB(r, p.hairline);
    fillRoundRect(r, uni, radius);

    const float innerR = radius - hairline;
    const SDL_FRect innerL{a.x + hairline, a.y + hairline,
                           a.w - hairline - hairline / 2, a.h - 2 * hairline};
    const SDL_FRect innerRt{b.x + hairline / 2, b.y + hairline,
                            b.w - hairline - hairline / 2, b.h - 2 * hairline};
    setRGB(r, g_core.isDown(pr[0]) ? p.faceDown : p.face);
    fillHalf(innerL, /*leftHalf=*/true, innerR);
    setRGB(r, g_core.isDown(pr[1]) ? p.faceDown : p.face);
    fillHalf(innerRt, /*leftHalf=*/false, innerR);

    // Divider between the two targets, same tone as the ring.
    setRGB(r, p.hairline);
    const SDL_FRect div{b.x - hairline / 2, a.y + hairline, hairline,
                        a.h - 2 * hairline};
    SDL_RenderFillRect(r, &div);
  }

  for (int i = 0; i < kPadCount; i++) {
    if (inPair[i]) continue;
    const PadButton &b = g_pad[i];
    setRGB(r, p.hairline);
    fillRoundRect(r, b.rect, radius);

    const SDL_FRect inner{b.rect.x + hairline, b.rect.y + hairline,
                          b.rect.w - 2 * hairline, b.rect.h - 2 * hairline};
    setRGB(r, g_core.isDown(i) ? p.faceDown : p.face);
    fillRoundRect(r, inner, radius - hairline);
  }

  // The grabber. Six dots and no ring, deliberately: every real control here is
  // a ringed face, so a ringless handle cannot be misread as an eighth button.
  // While dragging it takes a face behind the dots -- the same "moved towards
  // the foreground" cue the buttons use for pressed, since the pad is the only
  // thing on screen that can acknowledge the gesture.
  if (g_gripRect.w > 0) {
    if (g_gripDragging) {
      setRGB(r, p.face);
      fillRoundRect(r, g_gripRect, radius);
    }
    setRGB(r, p.faceDown);
    const float dot = SDL_max(2.0f, 3.5f * S);
    const float step = dot * 2.4f;
    const float cx = g_gripRect.x + g_gripRect.w / 2.0f;
    const float cy = g_gripRect.y + g_gripRect.h / 2.0f;
    for (int row = -1; row <= 1; row++)
      for (int col = 0; col < 2; col++) {
        const SDL_FRect d{cx + (col ? step : -step) / 2.0f - dot / 2.0f,
                          cy + row * step - dot / 2.0f, dot, dot};
        fillRoundRect(r, d, dot / 2.0f);
      }
  }
}

int padHitTest(float x, float y) {
  for (int i = 0; i < kPadCount; i++) {
    const SDL_FRect &r = g_pad[i].rect;
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return i;
  }
  return -1;
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
      const float fx = e->tfinger.x * outW, fy = e->tfinger.y * outH;

      // The grabber claims its touch BEFORE PadCore is consulted, so PadCore
      // never learns this finger exists and stays a pure button machine. No
      // action is produced here: nothing is injected, nothing is held.
      if (g_gripRect.w > 0 && fx >= g_gripRect.x &&
          fx < g_gripRect.x + g_gripRect.w && fy >= g_gripRect.y &&
          fy < g_gripRect.y + g_gripRect.h) {
        g_windowId = e->tfinger.windowID;
        g_gripFinger = e->tfinger.fingerID;
        g_gripHeld = true;
        g_gripDragging = false;
        g_gripDownAtMs = SDL_GetTicks();
        g_gripStartYPx = fy;
        g_gripStartOffset = g_padOffsetY;
        break;
      }

      const int hit = padHitTest(fx, fy);
      if (hit >= 0) g_windowId = e->tfinger.windowID;
      applyActions(g_core.fingerDown(hit >= 0 ? hit : PadCore::kNoSlot,
                                     e->tfinger.fingerID));
      break;
    }

    case SDL_EVENT_FINGER_MOTION: {
      if (g_gripHeld && e->tfinger.fingerID == g_gripFinger) {
        if (!windowPixelSize(e->tfinger.windowID, &outW, &outH)) break;
        const float y = e->tfinger.y * outH;
        if (!g_gripDragging) {
          if (SDL_GetTicks() - g_gripDownAtMs < kGripHoldMs) break;
          // Take the drag from where the finger is NOW. Baselining at
          // finger-down instead would make the rows jump by however far it
          // drifted while the hold was being satisfied.
          g_gripDragging = true;
          g_gripStartYPx = y;
          g_gripStartOffset = g_padOffsetY;
        }
        g_padOffsetY = g_gripStartOffset + (y - g_gripStartYPx) / g_ptScale;
        g_padLaidOut = false;  // layoutPad re-runs on the next paint, and clamps
        SimulatorOverlay::requestPresent();
        break;
      }

      // Dragging off a control cancels it, matching how a system button behaves
      // and how a real key behaves when your thumb slides off it.
      const int slot = g_core.heldSlot(e->tfinger.fingerID);
      if (slot == PadCore::kNoSlot) break;
      if (!windowPixelSize(e->tfinger.windowID, &outW, &outH)) break;
      const float x = e->tfinger.x * outW;
      const float y = e->tfinger.y * outH;
      const SDL_FRect &r = g_pad[slot].rect;
      if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h)
        applyActions(g_core.fingerLeftSlot(e->tfinger.fingerID));
      break;
    }

    // CANCELED alongside UP: iOS cancels touches for its own gestures (home
    // indicator swipe, Control Center pull, an incoming call). Without this the
    // slot stays latched down forever, and PadCore ignores every later press on
    // it — a second, permanent way for POWER to stop working until force quit.
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
      if (g_gripHeld && e->tfinger.fingerID == g_gripFinger) {
        // Drop. The rows stay where they were left; PadCore has nothing to
        // release because it never saw this finger.
        g_gripHeld = g_gripDragging = false;
        SimulatorOverlay::requestPresent();
        break;
      }
      applyActions(g_core.fingerUp(e->tfinger.fingerID));
      break;

    // Backgrounding must not leave a key stuck down: the finger is gone, and a
    // stuck POWER would read as a long press.
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      // The grabber goes with it: iOS will not deliver the UP, so a held
      // grabber would resume dragging on the next stray motion event.
      g_gripHeld = g_gripDragging = false;
      applyActions(g_core.reset());
      break;

    // The pad is laid out from the output size, so a size change invalidates it.
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
      g_padLaidOut = false;
      SimulatorOverlay::requestPresent();
      break;
    }

    default:
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
  // IDEMPOTENT ACROSS WAKES. On iOS a deep-sleep wake longjmps back through
  // setup() (SimulatorLifecycle, CROSSPOINT_SIM_REBOOT_IN_PROCESS), which
  // calls this again. Event watches must be registered exactly once: each
  // SDL_AddEventWatch call stacks another live callback, so N wakes would run
  // every finger event through N watches. State refreshes (theme, layout,
  // released buttons) re-run every call; registrations do not.
  static bool s_watchesInstalled = false;

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

  // A wake begins with no fingers on glass; drop any state a pre-sleep touch
  // left behind, and relayout against the (possibly rotated) window.
  applyActions(g_core.reset());
  g_padLaidOut = false;

  // Appearance. SDL_Init has already run (HalDisplay::begin calls it), so the
  // theme is populated and can be read straight away; the watch keeps it current
  // if the user flips the system between light and dark while the app is up.
  applyTheme();
  SDL_Log("[harness] appearance: %s", g_dark ? "dark" : "light");

  SimulatorOverlay::requestPresent();

  if (!s_watchesInstalled) {
    if (!SDL_AddEventWatch(presentationWatch, nullptr))
      SDL_Log("[harness] presentation watch failed: %s", SDL_GetError());
    if (!SDL_AddEventWatch(padWatch, nullptr))
      SDL_Log("[harness] SDL_AddEventWatch failed: %s", SDL_GetError());
    else
      SDL_Log("[harness] button pad installed");
    s_watchesInstalled = true;
  }
}

void CrossPointHarness_perFrame() {
  pollAppearance();
  repaintAfterForeground();
}
