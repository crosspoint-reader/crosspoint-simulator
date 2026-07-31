// CrossPoint X3 -> iPhone harness.
//
// THE MODEL. The iPhone impersonates X3 peripherals; it is not a new CrossPoint
// board. There are two surfaces and exactly one translation point between them:
//
//   harness layer (this file)  draws an on-screen button pad and reads touches
//                              on it. Lives outside the simulated device.
//   device layer (HalGPIO)     sees only the X3's seven GPIO buttons, as SDL
//                              keyboard scancodes.
//
// The harness translates the first into the second by pushing synthetic
// SDL_EVENT_KEY_DOWN / _UP onto SDL's event queue. Nothing below SDL knows the
// difference, so no #if TARGET_OS_IPHONE appears in HalGPIO or the firmware.
//
// hasTouch() stays false for X3 and iPhone touches become BUTTON events, never
// touch events. Hit-testing happens HERE, above SDL; no coordinate is ever handed
// to the firmware. Letting one reach the hasTouch() branch would make the
// firmware take X4-Pro-only paths and we would be testing a device that does not
// exist.
//
// ONE CONTROL PER PHYSICAL BUTTON, and nothing else. Each is down-on-touch and
// up-on-lift, so it expresses a genuine hold -- which is what page-turn
// autorepeat and long-press power-off need.
//
// WHY AN EVENT WATCH, NOT A POLL LOOP. HalGPIO::update() owns the SDL event pump
// for the whole simulator and must keep owning it -- two pollers would split
// events between them. SDL_AddEventWatch observes events as they are queued
// without consuming them, so the harness sees finger events that HalGPIO simply
// ignores, and neither steals from the other.
//
// A MEASURED LIMIT, verified not assumed: SDL_PushEvent delivers an event to the
// queue but does NOT update SDL's internal keyboard state, so
// SDL_GetKeyboardState() stays clear for injected keys. Edge reads
// (wasPressed/wasReleased) therefore work; level reads (isPressed /
// anyButtonHeld / powerHoldDuration) do not. See ios/README.md.

#include "CrossPointHarness.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <string>
#include <unistd.h>

#include "SimulatorOverlay.h"

namespace {

// --- The X3's seven buttons ------------------------------------------------
//
// Scancodes mirror HalGPIO.cpp's buttonScancode[] exactly. There is no control
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
enum class Glyph {
  Back,  // chevron against a bar, so BACK cannot be mistaken for LEFT
  ChevronLeft,
  ChevronUp,
  ChevronDown,
  ChevronRight,
  Check,
  Power
};

struct PadButton {
  SDL_Scancode scancode;
  SDL_Keycode keycode;
  const char *name;
  Glyph glyph;
  SDL_FRect rect{};
  bool down = false;
  SDL_FingerID finger = 0;
};

PadButton g_pad[] = {
    {SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, "BACK", Glyph::Back},
    {SDL_SCANCODE_P, SDLK_P, "POWER", Glyph::Power},
    {SDL_SCANCODE_UP, SDLK_UP, "UP", Glyph::ChevronUp},
    {SDL_SCANCODE_LEFT, SDLK_LEFT, "LEFT", Glyph::ChevronLeft},
    {SDL_SCANCODE_RETURN, SDLK_RETURN, "CONFIRM", Glyph::Check},
    {SDL_SCANCODE_RIGHT, SDLK_RIGHT, "RIGHT", Glyph::ChevronRight},
    {SDL_SCANCODE_DOWN, SDLK_DOWN, "DOWN", Glyph::ChevronDown},
};
constexpr int kPadBack = 0, kPadPower = 1, kPadUp = 2, kPadLeft = 3,
              kPadConfirm = 4, kPadRight = 5, kPadDown = 6;
constexpr int kPadCount = 7;

bool g_padLaidOut = false;
float g_ptScale = 3.0f;
SDL_WindowID g_windowId = 0;

void pushSyntheticKey(SDL_Scancode sc, SDL_Keycode key, bool down) {
  SDL_Event e;
  SDL_zero(e);
  e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  e.key.timestamp = SDL_GetTicksNS();
  e.key.windowID = g_windowId;
  e.key.which = 0;
  e.key.scancode = sc;
  e.key.key = key;
  e.key.mod = SDL_KMOD_NONE;
  e.key.down = down;
  e.key.repeat = false;
  if (!SDL_PushEvent(&e))
    SDL_Log("[harness] SDL_PushEvent failed: %s", SDL_GetError());
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
//     Left    .     Power    .     Right
//     Back  Select    .     Up     Down
//
// The lower row sits on the bottom of the safe area rather than the physical
// bottom edge: below that line is the home indicator, and a control there would
// fight the system's own swipe.
void layoutPad(int outW, int outH) {
  const float S = g_ptScale;
  const float W = static_cast<float>(outW) / S;
  const float H = static_cast<float>(outH) / S;

  constexpr float kMargin = 20.0f;     // side inset
  constexpr float kGap = 8.0f;         // minimum separation between targets
  constexpr float kRow = 46.0f;        // >= the 44 pt minimum target height
  constexpr float kHomeInset = 34.0f;  // home-indicator safe area
  constexpr int kCols = 5;

  const float colW = (W - 2 * kMargin - (kCols - 1) * kGap) / kCols;
  float colX[kCols];
  for (int i = 0; i < kCols; i++) colX[i] = kMargin + i * (colW + kGap);

  const float lowerY = H - kHomeInset - kRow;
  const float upperY = lowerY - kGap - kRow;

  auto place = [&](int idx, int col, float y) {
    g_pad[idx].rect = {colX[col] * S, y * S, colW * S, kRow * S};
  };

  place(kPadLeft, 0, upperY);
  place(kPadPower, 2, upperY);
  place(kPadRight, 4, upperY);

  place(kPadBack, 0, lowerY);
  place(kPadConfirm, 1, lowerY);
  place(kPadUp, 3, lowerY);
  place(kPadDown, 4, lowerY);
}

// --- Painting --------------------------------------------------------------
//
// Apple system greys at the low-contrast end, so the chrome recedes and the
// e-ink panel stays the subject: systemGray6 fill, systemGray5 hairline,
// systemGray glyph, each one step darker while held.
void setRGB(SDL_Renderer *r, int rr, int gg, int bb) {
  SDL_SetRenderDrawColor(r, static_cast<Uint8>(rr), static_cast<Uint8>(gg),
                         static_cast<Uint8>(bb), 255);
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

void strokeLine(SDL_Renderer *r, float x1, float y1, float x2, float y2,
                float t) {
  const float dx = x2 - x1, dy = y2 - y1;
  const int steps = static_cast<int>(SDL_max(SDL_fabsf(dx), SDL_fabsf(dy))) + 1;
  for (int i = 0; i <= steps; i++) {
    const float f = static_cast<float>(i) / static_cast<float>(steps);
    fillRect(r, x1 + dx * f - t / 2, y1 + dy * f - t / 2, t, t);
  }
}

void drawGlyph(SDL_Renderer *r, Glyph g, float cx, float cy, float size,
               float t) {
  const float e = size / 2;
  switch (g) {
    case Glyph::Back:
      // Chevron pointing into a bar. LEFT is a bare chevron, so the two read as
      // different controls at a glance rather than as the same arrow twice.
      strokeLine(r, cx + e * 0.9f, cy - e, cx - e * 0.1f, cy, t);
      strokeLine(r, cx - e * 0.1f, cy, cx + e * 0.9f, cy + e, t);
      strokeLine(r, cx - e * 0.75f, cy - e * 0.85f, cx - e * 0.75f,
                 cy + e * 0.85f, t);
      break;
    case Glyph::ChevronLeft:
      strokeLine(r, cx + e * 0.5f, cy - e, cx - e * 0.5f, cy, t);
      strokeLine(r, cx - e * 0.5f, cy, cx + e * 0.5f, cy + e, t);
      break;
    case Glyph::ChevronRight:
      strokeLine(r, cx - e * 0.5f, cy - e, cx + e * 0.5f, cy, t);
      strokeLine(r, cx + e * 0.5f, cy, cx - e * 0.5f, cy + e, t);
      break;
    case Glyph::ChevronUp:
      strokeLine(r, cx - e, cy + e * 0.5f, cx, cy - e * 0.5f, t);
      strokeLine(r, cx, cy - e * 0.5f, cx + e, cy + e * 0.5f, t);
      break;
    case Glyph::ChevronDown:
      strokeLine(r, cx - e, cy - e * 0.5f, cx, cy + e * 0.5f, t);
      strokeLine(r, cx, cy + e * 0.5f, cx + e, cy - e * 0.5f, t);
      break;
    case Glyph::Check:
      strokeLine(r, cx - e, cy + e * 0.05f, cx - e * 0.25f, cy + e * 0.7f, t);
      strokeLine(r, cx - e * 0.25f, cy + e * 0.7f, cx + e, cy - e * 0.7f, t);
      break;
    case Glyph::Power: {
      // IEC power mark: a broken ring with an upright bar through the gap.
      const float rad = e * 0.85f;
      for (int a = 40; a <= 320; a += 2) {
        const float rads = static_cast<float>(a) * 3.14159265f / 180.0f;
        fillRect(r, cx + SDL_sinf(rads) * rad - t / 2,
                 cy + SDL_cosf(rads) * rad - t / 2, t, t);
      }
      strokeLine(r, cx, cy - rad * 1.05f, cx, cy - rad * 0.1f, t);
      break;
    }
  }
}

void paintPad(SDL_Renderer *r, int outW, int outH) {
  if (!g_padLaidOut) {
    layoutPad(outW, outH);
    g_padLaidOut = true;
  }
  const float S = g_ptScale;
  const float radius = 12.0f * S;
  const float hairline = SDL_max(1.0f, S * 0.5f);

  for (const PadButton &b : g_pad) {
    setRGB(r, 0xE5, 0xE5, 0xEA);  // systemGray5 hairline
    fillRoundRect(r, b.rect, radius);

    const SDL_FRect inner{b.rect.x + hairline, b.rect.y + hairline,
                          b.rect.w - 2 * hairline, b.rect.h - 2 * hairline};
    if (b.down)
      setRGB(r, 0xE5, 0xE5, 0xEA);
    else
      setRGB(r, 0xF2, 0xF2, 0xF7);  // systemGray6
    fillRoundRect(r, inner, radius - hairline);

    if (b.down)
      setRGB(r, 0x6C, 0x6C, 0x70);
    else
      setRGB(r, 0x8E, 0x8E, 0x93);  // systemGray
    drawGlyph(r, b.glyph, b.rect.x + b.rect.w / 2, b.rect.y + b.rect.h / 2,
              20.0f * S, SDL_max(2.0f, 2.5f * S));
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
  g_pad[i].down = false;
  pushSyntheticKey(g_pad[i].scancode, g_pad[i].keycode, false);
  SimulatorOverlay::requestPresent();
  SDL_Log("[harness] %s up", g_pad[i].name);
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
      pushSyntheticKey(g_pad[hit].scancode, g_pad[hit].keycode, true);
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
        if (g_pad[i].down && g_pad[i].finger == e->tfinger.fingerID) {
          releaseButton(i);
          break;
        }
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

    default:
      break;
  }
  return true;  // never filter anything out
}

}  // namespace

// --- Public entry points ---------------------------------------------------

void CrossPointHarness_prepareFilesystem() {
  // HalStorage prefixes every path with ./fs_, which relies on the process CWD.
  // On iOS the CWD is the (read-only) bundle, so point it at the app's Documents
  // directory, which is writable and visible in Files for sideloading books.
  const char *home = std::getenv("HOME");
  if (!home) {
    SDL_Log("[harness] HOME unset; leaving CWD alone");
    return;
  }
  const std::string docs = std::string(home) + "/Documents";
  if (chdir(docs.c_str()) != 0) {
    SDL_Log("[harness] chdir(%s) failed", docs.c_str());
    return;
  }
  SDL_Log("[harness] cwd -> %s", docs.c_str());
}

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
  SimulatorOverlay::requestPresent();

  if (!SDL_AddEventWatch(padWatch, nullptr))
    SDL_Log("[harness] SDL_AddEventWatch failed: %s", SDL_GetError());
  else
    SDL_Log("[harness] button pad installed");
}
