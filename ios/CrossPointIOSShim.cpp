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
// There are no timed gestures on this surface (the movable-pad grabber, once
// the only one, was removed 2026-08-02). If a future chrome gesture needs a
// timer, it belongs out here in the harness -- never in PadCore.
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
// SIZING: full-height controls are 60-96 pt squares, comfortably over the HIG
// 44 pt minimum. The bottom row is a deliberate exception -- half-height
// (34-40 pt), approved as-shown 2026-08-02; see the trade-off note above
// layoutPad(). Bands stay inset clear of the Dynamic Island at the top and the
// home indicator at the bottom.
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

// The grabber (a drag handle that let the rows follow the finger) is GONE —
// owner-approved layout 2026-08-02 removed it along with its hold timer and
// offset machinery. The pad is fixed: top row hugs the panel, bottom row is
// anchored at the screen's bottom edge.

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
// Owner-approved layout 2026-08-02 (mockups: pad_layout_review artifact):
//
//     [Back|Select]      [Left|Right]        <- front rockers, full squares,
//                                               hugging the panel's bottom edge
//     [Power]              [Up|Down]         <- half-height row, anchored at
//                                               the screen bottom, clear of the
//                                               home indicator
//
// UP/DOWN are the X3's SIDE buttons (MappedInputManager keeps them fixed as
// page-turn/Up/Down; main.cpp calls BTN_UP the "left side button") — fused
// into one rocker at the right. BACK/SELECT/LEFT/RIGHT are the FRONT buttons
// (the remappable frontButton* set), fused pairs as on the chassis. Fused
// pairs share an edge -- no gap inside a pair, a wide gap between pairs.
//
// KNOWN TRADE-OFF, approved as-shown: the half-height row's targets run
// 34-40pt tall, under the 44pt HIG minimum. If page-turns feel cramped on
// device, the agreed fallback is invisible hit-slop extending the bottom-row
// targets upward -- visuals unchanged.
//
// The top row hugs the panel (SimulatorOverlay::panelBottomPx) so thumbs rest
// at the page; before the first present it falls back to sitting just above
// the bottom row.
void layoutPad(int outW, int outH) {
  const float S = g_ptScale;
  const float W = static_cast<float>(outW) / S;
  const float H = static_cast<float>(outH) / S;

  constexpr float kMargin = 20.0f;      // side inset
  constexpr float kOptimalSquare = 60.0f;  // owner-picked target size
  constexpr float kGap = 16.0f;
  // Panel -> top row gap MATCHES THE CHASSIS (owner ruling 2026-08-02): on the
  // X3 the front buttons' top edge sits 11.6 mm below the panel window, 14.8%
  // of the 78.2 mm panel height (measured from Xteink's straight-on product
  // renders; the firmware repo's docs/hardware-dimensions.md holds the full
  // table and methodology). Expressed as a ratio of the PRESENTED panel height
  // so the proportion holds at any device scale; kGap is the pre-first-present
  // fallback only.
  constexpr float kPanelGapRatio = 11.6f / 78.2f;
  constexpr float kRowClear = kGap;     // top row keeps at least this above the bottom row
  constexpr float kHomeInsetFallback = 34.0f;  // when the safe area is unreadable
  constexpr float kHomeInsetMin = 16.0f;       // floor for home-button devices (safe area 0)

  // STRICT SQUARE GRID, cell constrained to the optimum (owner ruling
  // 2026-08-02): the cell no longer stretches with device width. The COLUMN
  // COUNT absorbs the width instead -- cols is the integer count whose square
  // lands closest to kOptimalSquare, never fewer than the five the top row
  // needs. Cells touch and run flush margin to margin, so wider devices gain
  // empty middle columns rather than fatter buttons (55.8pt cell / 6 cols on
  // SE and 13 mini, 58.8/6 on 16, 57.1/7 on 16 Pro Max).
  //
  // Rocker/button column assignments count from the grid's ends, so they hold
  // for any cols >= 5: top row Back|Select in columns 0-1 and Left|Right in
  // cols-2..cols-1; bottom row Power in column 0, Up|Down in cols-2..cols-1
  // at half height.
  //
  // The cell sets the band height (below), the band comes out of the panel's
  // space, and the panel scale is floored to an integer
  // (CROSSPOINT_SIM_PIXEL_EXACT). The build is iPhone-only, portrait-only
  // (TARGETED_DEVICE_FAMILY 1, UISupportedInterfaceOrientations = Portrait).
  // If iPad (family 2) is ever enabled, the fix is not a different cell -- it
  // is to put the pad BESIDE the panel in a landscape window, where the spare
  // width is.
  const float usable = W - 2 * kMargin;
  const int cols =
      SDL_max(5, static_cast<int>(SDL_roundf(usable / kOptimalSquare)));
  const float kSquare = usable / static_cast<float>(cols);
  const float kHalf = kSquare / 2.0f;

  // Bottom inset from the system safe area (home indicator), with a fallback
  // when the window is unreadable and a floor so home-button devices (safe
  // area 0) still keep a margin off the screen edge.
  float bottomInset = kHomeInsetFallback;
  if (SDL_Window *win = SDL_GetWindowFromID(g_windowId)) {
    int lw = 0, lh = 0;
    SDL_Rect safe{};
    if (SDL_GetWindowSize(win, &lw, &lh) && lh > 0 &&
        SDL_GetWindowSafeArea(win, &safe))
      bottomInset = static_cast<float>(lh - (safe.y + safe.h));
  }
  bottomInset = SDL_max(bottomInset, kHomeInsetMin);

  // Bottom row: half-height, anchored at the bottom of the screen.
  const float lowerY = H - bottomInset - kHalf;

  // Top row hugs the panel at the chassis-matched gap, clamped clear of the
  // bottom row.
  const float maxUpper = lowerY - kRowClear - kSquare;
  const int panelBottomPx = SimulatorOverlay::panelBottomPx();
  const int panelHeightPx = SimulatorOverlay::panelHeightPx();
  const float panelGap =
      panelHeightPx > 0 ? (static_cast<float>(panelHeightPx) / S) * kPanelGapRatio : kGap;
  float upperY;
  if (panelBottomPx > 0) {
    upperY = static_cast<float>(panelBottomPx) / S + panelGap;
    if (upperY > maxUpper) upperY = maxUpper;
  } else {
    upperY = maxUpper;
  }

  auto place = [&](int idx, float x, float y, float w, float h) {
    g_pad[idx].rect = {x * S, y * S, w * S, h * S};
  };

  const auto colX = [&](int c) { return kMargin + c * kSquare; };

  // Top row: the two fused front rockers at the grid's ends.
  place(kPadBack, colX(0), upperY, kSquare, kSquare);
  place(kPadConfirm, colX(1), upperY, kSquare, kSquare);
  place(kPadLeft, colX(cols - 2), upperY, kSquare, kSquare);
  place(kPadRight, colX(cols - 1), upperY, kSquare, kSquare);

  // Bottom row: POWER in the first column, the fused side rocker at the end.
  place(kPadPower, colX(0), lowerY, kSquare, kHalf);
  place(kPadUp, colX(cols - 2), lowerY, kSquare, kHalf);
  place(kPadDown, colX(cols - 1), lowerY, kSquare, kHalf);

  // Reserve the pad's band out of the panel's space: the chassis-ratio gap
  // plus both rows and the home inset. The gap term makes this DERIVED from
  // the presented panel height, which the band itself influences -- but the
  // loop converges: the panel scale is floored to an integer, so the band can
  // only move the panel between discrete scales, and one extra relayout after
  // the first present settles it (panelBottom-change already triggers that
  // relayout in paintPad). Before the first present the kGap fallback keeps
  // the band close to its old constant value.
  SimulatorOverlay::setBottomInset(static_cast<int>(
      (panelGap + kSquare + kRowClear + kHalf + bottomInset) * S));

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
// HOLLOW CONTROLS. Each control is a one-device-pixel stroke around nothing:
// the face IS the field, so at rest the pad is seven outlines on the same tone
// as the page's paper. A press lays a WASH inside the outline -- the fill
// changes and the stroke never moves.
//
// This reverses what the pad used to do, and the reversal is the point. The old
// arrangement put the face AWAY from the field (lighter than the paper in light
// appearance, darker than the background in dark) and spent the contrast on a
// ring. Two ceilings killed that: the field sits 4 levels below white in light
// and 18 above black in dark, so in light the face had almost nowhere to go and
// in dark the ring had to shout to clear the field. Both tones now step TOWARD
// mid-grey -- darker than the paper, lighter than 121212 -- which is the roomy
// direction in both appearances. One rule where there used to be two mirrored
// ones. (Apple's system ramp is still no help here and is still not used: it
// runs the wrong way in dark, systemGray6 1C1C1E being lighter than
// systemBackground 121212, and it is blue-tinted at every step against this
// app's warm paper.)
//
// THE PRESS IS THE ONLY FEEDBACK, the controls being unlabelled, and it is a
// large-area change rather than a loud one. The wash is 14/255 against a 34/255
// stroke, but it covers a 58.8 pt cell where the stroke covers a line, and area
// is what makes it read. If it ever feels soft on device the knob is one step:
// E9E9E7 / 242424 takes the wash to 18 and still leaves 15 clear of the stroke.
//
// Every pair that must be told apart is >= 13/255, the floor below which a step
// is not reliably visible on a phone. Verified on the values below:
//   light  stroke/field 34, wash/field 14, stroke/wash 20
//   dark   stroke/field 33, wash/field 14, stroke/wash 19
//
// NOT ACCESSIBLE, and knowingly so. WCAG 1.4.11 asks 3:1 against the adjacent
// colour for the visual information that identifies a control, and the
// exemption it grants a button with a label does not apply to a pad that has
// none. These strokes measure 1.36:1 in light and 1.48:1 in dark; 3:1 against
// these fields would be about 929292 and 616161, i.e. 105/255 and 79/255. The
// pad has never met that bar -- the ring this replaces measured 1.33:1 and
// 1.60:1 -- so this is a difference of degree rather than of kind, but it is a
// choice made with the number in hand, not an oversight. The mitigation, if one
// is ever wanted, is behavioural rather than tonal: paint the pad a step
// stronger for the first few launches and settle to these values after.
struct Palette {
  Uint8 field[3];     // behind the panel and the pad
  Uint8 hairline[3];  // the stroke: control outlines and pair ticks
  Uint8 face[3];      // control interior at rest: EQUAL to the field, i.e. hollow
  Uint8 faceDown[3];  // the wash, laid inside the stroke while held
};

// The field matches the panel's paper tone (HalDisplay's PanelPalette:
// 2D2D2D-on-FBFBF9 light, E0E0DE-on-121212 dark — change them together), so
// the page floats edgeless in the field in both appearances. `face` repeats it
// on purpose: paintPad's existing stroke-then-face draw paints a hollow control
// when the two are equal, so nothing structural is needed to get an outline.
constexpr Palette kLightPalette{{0xFB, 0xFB, 0xF9},
                                {0xD9, 0xD9, 0xD7},
                                {0xFB, 0xFB, 0xF9},
                                {0xED, 0xED, 0xEB}};
constexpr Palette kDarkPalette{{0x12, 0x12, 0x12},
                               {0x33, 0x33, 0x33},
                               {0x12, 0x12, 0x12},
                               {0x20, 0x20, 0x20}};

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

  // ONE DEVICE PIXEL, not half a point. `S * 0.5f` is 1.5 px on a 3x phone,
  // which cannot land on the pixel grid: the stroke is antialiased across two
  // rows and arrives lighter and softer than the palette above says, so the
  // constant stops describing what is on the glass. A whole pixel is exact at
  // every scale, and it is the rule UIKit itself uses for separators.
  const float hairline = 1.0f;

  // A stroke with the face inset inside it. The stroke stays put while held, so
  // the face changing tone reads as the control filling rather than as the
  // control being redrawn. Pressed paint comes straight from PadCore's finger
  // state -- the moment no finger holds a control, it paints released.

  // Fill one half of a rocker: rounded on the outer end, SQUARE on the shared
  // edge -- a rounded fill then a square patch over the inner end's corners.
  auto fillHalf = [&](const SDL_FRect &half, bool leftHalf, float rad) {
    fillRoundRect(r, half, rad);
    const SDL_FRect patch{leftHalf ? half.x + half.w - rad : half.x, half.y,
                          rad, half.h};
    SDL_RenderFillRect(r, &patch);
  };

  // The three fused pairs paint as ONE capsule each -- a single hairline
  // ring around the union with only the outer corners rounded (no pinched
  // notch where two rounded squares would meet), a hairline divider marking
  // the two targets, and the pressed half shading independently. Up|Down is
  // half-height; the same union/divider math holds because a pair shares y/h.
  const int pairs[3][2] = {
      {kPadBack, kPadConfirm}, {kPadLeft, kPadRight}, {kPadUp, kPadDown}};
  bool inPair[kPadCount] = {};
  for (const auto &pr : pairs) {
    const SDL_FRect &a = g_pad[pr[0]].rect;
    const SDL_FRect &b = g_pad[pr[1]].rect;
    inPair[pr[0]] = inPair[pr[1]] = true;

    const SDL_FRect uni{a.x, a.y, (b.x + b.w) - a.x, a.h};
    setRGB(r, p.hairline);
    fillRoundRect(r, uni, radius);

    // The two halves MEET at the seam, with no stroke left showing between
    // them. They used to stop a half-hairline short on that edge, so the
    // capsule underneath painted a full-height divider whether one was wanted
    // or not -- which is why simply shortening the divider rect below did
    // nothing. The tick is now the only thing marking the seam.
    const float innerR = radius - hairline;
    const SDL_FRect innerL{a.x + hairline, a.y + hairline, a.w - hairline,
                           a.h - 2 * hairline};
    const SDL_FRect innerRt{b.x, b.y + hairline, b.w - hairline,
                            b.h - 2 * hairline};
    setRGB(r, g_core.isDown(pr[0]) ? p.faceDown : p.face);
    fillHalf(innerL, /*leftHalf=*/true, innerR);
    setRGB(r, g_core.isDown(pr[1]) ? p.faceDown : p.face);
    fillHalf(innerRt, /*leftHalf=*/false, innerR);

    // Divider between the two targets, same tone as the stroke -- a CENTRE TICK
    // over 34% of the pair's inner height, not the full span. The seam only has
    // to say "two targets here"; a full-height rule doubles the ink in the
    // middle of the pad, which is exactly where the eye already is.
    setRGB(r, p.hairline);
    const float innerH = a.h - 2 * hairline;
    const float tickH = innerH * 0.34f;
    const SDL_FRect div{b.x - hairline / 2, a.y + hairline + (innerH - tickH) / 2,
                        hairline, tickH};
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

      const int hit = padHitTest(fx, fy);
      if (hit >= 0) g_windowId = e->tfinger.windowID;
      applyActions(g_core.fingerDown(hit >= 0 ? hit : PadCore::kNoSlot,
                                     e->tfinger.fingerID));
      break;
    }

    case SDL_EVENT_FINGER_MOTION: {
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
      applyActions(g_core.fingerUp(e->tfinger.fingerID));
      break;

    // Backgrounding must not leave a key stuck down: the finger is gone, and a
    // stuck POWER would read as a long press.
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
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
