#include "HalDisplay.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <SDL3/SDL.h>

#include "GrayscalePreview.h"
#include "SimulatorOverlay.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

static SDL_Window *window = nullptr;
static SDL_Renderer *sdl_renderer = nullptr;
static SDL_Texture *texture = nullptr;
// Render the simulator at full panel size. The previous 0.5x window was too
// small. With 1:1 pixel mapping, the simulator can be used for testing fine
// details.
//
// CROSSPOINT_SIM_WINDOW_SCALE overrides it (1-4). The window is sized in
// LOGICAL panel pixels, so at CROSSPOINT_RENDER_SCALE > 1 a headless run
// (SDL_VIDEODRIVER=dummy, no Retina backing store) presents the larger
// framebuffer into a panel-sized surface and CROSSPOINT_SIM_SCREENSHOTS
// captures it downsampled. Setting this to RENDER_SCALE makes the capture
// 1:1 with the framebuffer, which is what a rasterisation comparison needs.
static int simulatorWindowScale() {
  static const int scale = [] {
    const char *env = std::getenv("CROSSPOINT_SIM_WINDOW_SCALE");
    if (!env || env[0] == '\0')
      return 1;
    const int v = std::atoi(env);
    return (v >= 1 && v <= 4) ? v : 1;
  }();
  return scale;
}

// CROSSPOINT_SIM_DEVICE_PIXELS=1: size the window so one panel pixel lands on
// one DEVICE pixel. A window sized in logical points shows each panel pixel
// across contentScale^2 device pixels on a HiDPI display -- physically about
// twice the panel's real size on a Retina Mac, which reads as "zoomed 2x"
// next to the hardware. Dividing the point size by the display content scale
// undoes that; CROSSPOINT_SIM_WINDOW_SCALE then multiplies on top, so
// SCALE=2 + DEVICE_PIXELS=1 is an exact 2x-device-pixel presentation.
//
// Opt-in, not the default: the full-panel-size window is a deliberate desktop
// choice (see the comment above simulatorWindowScale) and headless capture
// geometry depends on it. The Mac app bundles set this via LSEnvironment.
static bool simulatorWantsDevicePixels() {
  static const bool wants = [] {
    const char *env = std::getenv("CROSSPOINT_SIM_DEVICE_PIXELS");
    return env && env[0] == '1';
  }();
  return wants;
}

// Pixel buffer written by the render task, read by the main thread for
// SDL_RenderPresent. On macOS, SDL calls must happen on the main thread.
static uint32_t
    pixelBuf[HalDisplay::DISPLAY_WIDTH * HalDisplay::DISPLAY_HEIGHT];
static std::atomic<bool> pendingPresent{false};
// Set when the inversion flag changes so presentIfNeeded (main thread) re-runs
// the framebuffer-to-pixel conversion from the cached last frame. Without it a
// polarity flip would wait for the next firmware refresh -- which on an e-ink
// device may never come.
static std::atomic<bool> pendingReconvert{false};
// Written by HalGPIO::update() (which owns SDL event polling); read by
// shouldQuit().
std::atomic<bool> quitRequested{false};

static int currentWindowWidth = 0;
static int currentWindowHeight = 0;

// Presentation policy.
//
// The desktop window is 1:1 with the panel, so letterbox + linear filtering is
// right there: Bayer-dithered pixels average to a correct-looking grey, which is
// what the e-ink panel actually reads like to the eye.
//
// CROSSPOINT_SIM_PIXEL_EXACT flips both. When the panel is scaled up (the phone
// presents it at 2x), a fractional scale or a linear filter greys the dither and
// every rendering judgement made against it is a lie. Integer scale plus
// nearest-neighbour keeps one framebuffer pixel exactly N screen pixels.
//
// Keyed on the intent, not on the platform, so a desktop build can ask for exact
// pixels too.
#if defined(CROSSPOINT_SIM_PIXEL_EXACT) && CROSSPOINT_SIM_PIXEL_EXACT
static constexpr SDL_RendererLogicalPresentation kLogicalPresentation =
    SDL_LOGICAL_PRESENTATION_INTEGER_SCALE;
static constexpr SDL_ScaleMode kPanelScaleMode = SDL_SCALEMODE_NEAREST;
#else
static constexpr SDL_RendererLogicalPresentation kLogicalPresentation =
    SDL_LOGICAL_PRESENTATION_LETTERBOX;
static constexpr SDL_ScaleMode kPanelScaleMode = SDL_SCALEMODE_LINEAR;
#endif

namespace {

struct GrayscalePreviewState {
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> bwBase{};
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> lsbPlane{};
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> msbPlane{};
  bool bwBaseValid = false;
  bool lsbValid = false;
  bool msbValid = false;
};

GrayscalePreviewState grayscalePreviewState;
std::array<uint8_t, HalDisplay::BUFFER_SIZE> frameBufferStorage{};
bool frameBufferLent = false;

struct ScreenshotEvent {
  unsigned long atMs;
  std::string path;
  bool handled = false;
};

std::vector<ScreenshotEvent> screenshotEvents;
bool screenshotEventsInitialized = false;

void initializeScreenshotEvents() {
  if (screenshotEventsInitialized)
    return;
  screenshotEventsInitialized = true;

  const char *schedule = std::getenv("CROSSPOINT_SIM_SCREENSHOTS");
  if (!schedule || schedule[0] == '\0')
    return;

  const std::string spec(schedule);
  size_t start = 0;
  while (start < spec.size()) {
    const size_t end = spec.find(';', start);
    const std::string item = spec.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    const size_t colon = item.find(':');
    if (colon != std::string::npos && colon + 1 < item.size()) {
      screenshotEvents.push_back(
          {std::strtoul(item.substr(0, colon).c_str(), nullptr, 10),
           item.substr(colon + 1)});
    }
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
}

bool hasDueScreenshot() {
  initializeScreenshotEvents();
  const unsigned long now = millis();
  for (const auto &event : screenshotEvents) {
    if (!event.handled && event.atMs <= now)
      return true;
  }
  return false;
}

bool saveRendererBmp(const std::string &path) {
  int width = 0;
  int height = 0;
  if (!SDL_GetCurrentRenderOutputSize(sdl_renderer, &width, &height) ||
      width <= 0 || height <= 0) {
    std::cerr << "[SIM] Cannot determine screenshot size: " << SDL_GetError()
              << std::endl;
    return false;
  }

  // SDL3's SDL_RenderReadPixels returns a new surface rather than filling a
  // caller-provided buffer, so the intermediate vector and the
  // CreateRGBSurfaceWithFormatFrom wrapper the SDL2 path needed are both gone.
  SDL_Surface *surface = SDL_RenderReadPixels(sdl_renderer, nullptr);
  if (!surface) {
    std::cerr << "[SIM] Cannot create screenshot surface: " << SDL_GetError()
              << std::endl;
    return false;
  }

  // SDL3 returns true on success where SDL2 returned 0.
  const bool saved = SDL_SaveBMP(surface, path.c_str());
  if (!saved) {
    std::cerr << "[SIM] Cannot save screenshot " << path << ": "
              << SDL_GetError() << std::endl;
  } else {
    std::cerr << "[SIM] Saved screenshot: " << path << std::endl;
  }
  SDL_DestroySurface(surface);
  return saved;
}

void captureDueScreenshots() {
  const unsigned long now = millis();
  for (auto &event : screenshotEvents) {
    if (event.handled || event.atMs > now)
      continue;
    event.handled = true;
    saveRendererBmp(event.path);
  }
}

// Panel palette: the 1bpp/AA framebuffer is presented as tinted ink on tinted
// paper rather than raw #000-on-#FFF. One palette per polarity; "inverted"
// (dark mode) swaps to light ink on dark paper, so the grayscale ramp needs no
// separate 255-level flip -- the ink->paper lerp direction IS the inversion.
// Values are shared with the iOS harness field colours (CrossPointIOSShim.cpp);
// change them together.
struct PanelPalette {
  uint8_t ink[3];    // a fully-black source pixel
  uint8_t paper[3];  // a fully-white source pixel
};
constexpr PanelPalette kPanelLight{{0x2D, 0x2D, 0x2D}, {0xFB, 0xFB, 0xF9}};
constexpr PanelPalette kPanelDark{{0xE0, 0xE0, 0xDE}, {0x12, 0x12, 0x12}};

// level: 0 = ink, 255 = paper (the pre-inversion grayscale convention).
uint32_t panelColor(uint8_t level, const PanelPalette &p) {
  uint32_t argb = 0xFF000000u;
  for (int c = 0; c < 3; c++) {
    const uint8_t v = static_cast<uint8_t>(
        p.ink[c] + (static_cast<int>(p.paper[c]) - p.ink[c]) * level / 255);
    argb |= static_cast<uint32_t>(v) << (16 - 8 * c);
  }
  return argb;
}

bool getBit(const uint8_t *buffer, int x, int y) {
  const int byteIdx = (y * HalDisplay::DISPLAY_WIDTH + x) / 8;
  const int bitIdx = 7 - (x % 8);
  return (buffer[byteIdx] & (1 << bitIdx)) != 0;
}

void renderBwPixels(const uint8_t *fb) {
  const PanelPalette &pal = display.isInverted() ? kPanelDark : kPanelLight;
  const uint32_t ink = panelColor(0, pal);
  const uint32_t paper = panelColor(255, pal);
  for (int y = 0; y < HalDisplay::DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < HalDisplay::DISPLAY_WIDTH; x++) {
      const bool white = getBit(fb, x, y);
      pixelBuf[y * HalDisplay::DISPLAY_WIDTH + x] = white ? paper : ink;
    }
  }
  pendingPresent.store(true);
}

void clearGrayscalePlanes() {
  grayscalePreviewState.lsbPlane.fill(0);
  grayscalePreviewState.msbPlane.fill(0);
  grayscalePreviewState.lsbValid = false;
  grayscalePreviewState.msbValid = false;
}

void snapshotBwBase(const uint8_t *fb) {
  memcpy(grayscalePreviewState.bwBase.data(), fb, HalDisplay::BUFFER_SIZE);
  grayscalePreviewState.bwBaseValid = true;
  clearGrayscalePlanes();
}

void copyPlane(std::array<uint8_t, HalDisplay::BUFFER_SIZE> &dst,
               const uint8_t *src, bool &valid) {
  if (!src) {
    valid = false;
    dst.fill(0);
    return;
  }
  memcpy(dst.data(), src, HalDisplay::BUFFER_SIZE);
  valid = true;
}

void composeGrayscalePreview() {
  const PanelPalette &pal = display.isInverted() ? kPanelDark : kPanelLight;
  const uint8_t *bwBase = grayscalePreviewState.bwBaseValid
                              ? grayscalePreviewState.bwBase.data()
                              : display.getFrameBuffer();
  for (int y = 0; y < HalDisplay::DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < HalDisplay::DISPLAY_WIDTH; x++) {
      const bool baseWhite = getBit(bwBase, x, y);
      const bool lsbActive =
          grayscalePreviewState.lsbValid &&
          getBit(grayscalePreviewState.lsbPlane.data(), x, y);
      const bool msbActive =
          grayscalePreviewState.msbValid &&
          getBit(grayscalePreviewState.msbPlane.data(), x, y);

      const uint8_t level =
          GrayscalePreview::previewLevel(baseWhite, msbActive, lsbActive);

      // No 255-level flip for inversion: the dark palette's ink->paper
      // direction already runs light-on-dark (see PanelPalette above).
      pixelBuf[y * HalDisplay::DISPLAY_WIDTH + x] = panelColor(level, pal);
    }
  }
  pendingPresent.store(true);
}

// Re-run the last framebuffer-to-pixel conversion after an inversion change,
// from the cached copies snapshotted by the render task. Runs on the main
// thread (presentIfNeeded); safe because the pixel/state buffers already
// tolerate the render task racing a present -- worst case is one torn frame
// that the next real render replaces.
//
// If grayscale AA planes were composited after the BW base, they are still
// cached alongside it (snapshotBwBase clears them on every fresh BW frame), so
// the reconversion keeps the AA gray levels instead of degrading to the BW
// base. If the render task happens to be mid-way through writing new planes,
// the recompose may briefly show them partially applied; the render task's own
// compose lands right after and corrects it.
void reconvertLastFrame() {
  if (!grayscalePreviewState.bwBaseValid)
    return; // nothing presented yet; the first real render reads the new flag
  if (grayscalePreviewState.lsbValid || grayscalePreviewState.msbValid)
    composeGrayscalePreview();
  else
    renderBwPixels(grayscalePreviewState.bwBase.data());
}

} // namespace

static bool isPortraitOrientation(GfxRenderer::Orientation orientation) {
  return orientation == GfxRenderer::Portrait ||
         orientation == GfxRenderer::PortraitInverted;
}

// The desktop window is sized in LOGICAL panel pixels, not framebuffer pixels.
// At RENDER_SCALE > 1 the framebuffer is bigger than the panel; sizing the
// window from it would give a window RENDER_SCALE times too large. Keeping the
// window at the logical size and letting SDL map the larger texture into it is
// also exactly what makes the extra detail visible: with
// SDL_WINDOW_HIGH_PIXEL_DENSITY a 528x792-point window has a 1056x1584-pixel
// backing store on a Retina display, so a 2x framebuffer lands 1:1 on screen.
static void getLogicalWindowSize(GfxRenderer::Orientation orientation,
                                 int *width, int *height) {
  const bool isPortrait = isPortraitOrientation(orientation);
  const int panelW = HalDisplay::DISPLAY_WIDTH / HalDisplay::RENDER_SCALE;
  const int panelH = HalDisplay::DISPLAY_HEIGHT / HalDisplay::RENDER_SCALE;
  float w = static_cast<float>((isPortrait ? panelH : panelW) *
                               simulatorWindowScale());
  float h = static_cast<float>((isPortrait ? panelW : panelH) *
                               simulatorWindowScale());
  if (simulatorWantsDevicePixels()) {
    // One panel pixel -> one device pixel: shrink the point size by the
    // display's content scale (2.0 on Retina, 1.0 elsewhere). Read from the
    // primary display because the window may not exist yet on the first call.
    const float s = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (s > 1.0f) {
      w /= s;
      h /= s;
    }
  }
  *width = static_cast<int>(SDL_roundf(w));
  *height = static_cast<int>(SDL_roundf(h));
}

// The logical presentation space is the FRAMEBUFFER's, not the window's. The
// dst rects in presentIfNeeded are in framebuffer pixels (kW/kH include
// RENDER_SCALE), so the logical size handed to SDL must match them and only
// the WINDOW size may track CROSSPOINT_SIM_WINDOW_SCALE / _DEVICE_PIXELS.
// Passing the window size here instead is the 2x-zoom bug: at RENDER_SCALE=2
// with a 1x window the texture was drawn double-size into a panel-sized
// logical space and only its top-left quarter was visible. The two sizes
// coincide at RENDER_SCALE=1 and at WINDOW_SCALE==RENDER_SCALE, which is why
// the plain desktop build and the phone/2x presentations never showed it.
static void getLogicalPresentationSize(GfxRenderer::Orientation orientation,
                                       int *width, int *height) {
  const bool isPortrait = isPortraitOrientation(orientation);
  *width = isPortrait ? HalDisplay::DISPLAY_HEIGHT : HalDisplay::DISPLAY_WIDTH;
  *height = isPortrait ? HalDisplay::DISPLAY_WIDTH : HalDisplay::DISPLAY_HEIGHT;
}

static void applyWindowGeometryIfNeeded(GfxRenderer::Orientation orientation) {
  if (!window || !sdl_renderer)
    return;

  int winW = 0;
  int winH = 0;
  getLogicalWindowSize(orientation, &winW, &winH);
  if (winW == currentWindowWidth && winH == currentWindowHeight)
    return;

  int logW = 0;
  int logH = 0;
  getLogicalPresentationSize(orientation, &logW, &logH);
  SDL_SetWindowSize(window, winW, winH);
  SDL_SetRenderLogicalPresentation(sdl_renderer, logW, logH,
                                   kLogicalPresentation);
  currentWindowWidth = winW;
  currentWindowHeight = winH;
}

namespace SimulatorOverlay {
static DrawFn overlayDraw = nullptr;
// Packed 0xRRGGBB. Defaults to the light panel's paper tone, so a host that
// never calls setClearColor (every desktop build) shows the panel seamlessly
// against its field.
static std::atomic<uint32_t> clearColor{0xFBFBF9u};
// Bottom band (device px) reserved for overlay chrome; see SimulatorOverlay.h.
static std::atomic<int> bottomInset{0};
// Top band (device px) reserved for host furniture -- status bar / Dynamic
// Island; see SimulatorOverlay.h.
static std::atomic<int> topInset{0};
void setDrawCallback(DrawFn fn) { overlayDraw = fn; }
void setClearColor(unsigned char r, unsigned char g, unsigned char b) {
  clearColor.store((static_cast<uint32_t>(r) << 16) |
                   (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b));
}
void setBottomInset(int px) {
  if (bottomInset.exchange(px > 0 ? px : 0) != px)
    requestPresent();
}
void setTopInset(int px) {
  const int v = px > 0 ? px : 0;
  if (topInset.exchange(v) != v)
    requestPresent();
}
// Written by presentIfNeeded (main thread) on the manual-placement path.
static std::atomic<int> panelBottom{0};
static std::atomic<int> panelHeight{0};
int panelBottomPx() { return panelBottom.load(); }
int panelHeightPx() { return panelHeight.load(); }
void requestPresent() { pendingPresent.store(true); }
// The single entry point for panel polarity (see SimulatorOverlay.h). The env
// override is applied here, on every call, so a forced polarity survives any
// number of platform theme changes, and so the headless env path and the iOS
// theme path exercise identical mechanics from this line down.
void setPanelDark(bool dark) {
  if (const char *forced = std::getenv("CROSSPOINT_SIM_DARK")) {
    if (forced[0] == '1' && forced[1] == '\0')
      dark = true;
    else if (forced[0] == '0' && forced[1] == '\0')
      dark = false;
    // Anything else (including empty) is treated as unset: follow the caller.
  }
  // The field follows the panel's paper tone, so the page has no visible edge
  // in either polarity. Hosts that call setClearColor themselves (the iOS
  // harness) use the same values, so the double write is idempotent.
  const PanelPalette &pal = dark ? kPanelDark : kPanelLight;
  setClearColor(pal.paper[0], pal.paper[1], pal.paper[2]);
  display.setInverted(dark);
}
} // namespace SimulatorOverlay

HalDisplay::HalDisplay() {}
HalDisplay::~HalDisplay() {}

#if defined(SIMULATOR_DEVICE_X4_PRO)
static constexpr const char *WINDOW_TITLE = "Simulator - XTEINK X4 Pro";
#elif defined(SIMULATOR_DEVICE_X3)
static constexpr const char *WINDOW_TITLE = "Simulator - XTEINK X3";
#else
static constexpr const char *WINDOW_TITLE = "Simulator - XTEINK X4";
#endif

void HalDisplay::begin() {
  // Idempotent, because setup() can run more than once in one process.
  //
  // A deep-sleep wake is a chip reset on hardware and a process relaunch on
  // desktop, so begin() would normally see a clean machine. Where the simulator
  // instead re-enters setup() in-process (iOS, which cannot exec -- see
  // SimulatorLifecycle.h), a second call would create a SECOND window and
  // renderer, leaving the visible one orphaned and the panel frozen. Reuse what
  // already exists; presentIfNeeded() re-applies the window geometry every
  // present, so orientation still self-corrects after a wake.
  if (window && sdl_renderer && texture) {
    return;
  }

  // Boot geometry, once. This line exists because the render scale was twice
  // believed shipped while the framebuffer silently stayed 1x (a define that
  // never reached this TU); the compiled truth must be observable at runtime.
  LOG_INF("DISP", "Framebuffer %dx%d, render scale %d", DISPLAY_WIDTH, DISPLAY_HEIGHT,
          static_cast<int>(RENDER_SCALE));

  // SDL3 returns true on success where SDL2 returned 0.
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError()
              << std::endl;
    return;
  }

  int winW = 0;
  int winH = 0;
  extern GfxRenderer renderer;
  getLogicalWindowSize(renderer.getOrientation(), &winW, &winH);

  // SDL_WINDOW_HIGH_PIXEL_DENSITY lets the renderer use full Retina/HiDPI pixels
  // on macOS so we get crisp 1:1 rendering instead of a blurry upscale.
  // SDL3 drops the x/y arguments and shows windows by default.
  window = SDL_CreateWindow(WINDOW_TITLE, winW, winH,
                            SDL_WINDOW_HIGH_PIXEL_DENSITY);
  sdl_renderer = SDL_CreateRenderer(window, nullptr);

  // Rendering logic runs in FRAMEBUFFER coordinates (see
  // getLogicalPresentationSize); SDL maps that space into the window's
  // drawable pixels, whatever size the window chose above.
  int logW = 0;
  int logH = 0;
  getLogicalPresentationSize(renderer.getOrientation(), &logW, &logH);
  SDL_SetRenderLogicalPresentation(sdl_renderer, logW, logH,
                                   kLogicalPresentation);
  currentWindowWidth = winW;
  currentWindowHeight = winH;

  texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH,
                              DISPLAY_HEIGHT);

  // SDL3 replaced the global SDL_HINT_RENDER_SCALE_QUALITY hint with a
  // per-texture setting, which must therefore come after the texture exists.
  // See kPanelScaleMode above for why the choice is not unconditional.
  SDL_SetTextureScaleMode(texture, kPanelScaleMode);

  // Default appearance is light, so a desktop build stays byte-identical to
  // what it always rendered; CROSSPOINT_SIM_DARK is applied inside
  // setPanelDark, which is what lets a headless run force either polarity. A
  // host with a real appearance to follow (the iOS harness) calls
  // setPanelDark again with it once the harness installs.
  SimulatorOverlay::setPanelDark(false);
}

void HalDisplay::begin(bool /*seamless*/) { begin(); }

void HalDisplay::clearScreen(uint8_t color) const {
  memset(getFrameBuffer(), color, BUFFER_SIZE);
}

void HalDisplay::drawImage(const uint8_t *imageData, uint16_t x, uint16_t y,
                           uint16_t w, uint16_t h, bool) const {
  uint8_t *fb = getFrameBuffer();
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT)
      break;
    const uint32_t destOffset =
        static_cast<uint32_t>(destY) * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint32_t srcOffset = static_cast<uint32_t>(row) * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES)
        break;
      fb[destOffset + col] = imageData[srcOffset + col];
    }
  }
}

void HalDisplay::drawImageTransparent(const uint8_t *imageData, uint16_t x,
                                      uint16_t y, uint16_t w, uint16_t h,
                                      bool) const {
  uint8_t *fb = getFrameBuffer();
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT)
      break;
    const uint32_t destOffset =
        static_cast<uint32_t>(destY) * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint32_t srcOffset = static_cast<uint32_t>(row) * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES)
        break;
      fb[destOffset + col] &= imageData[srcOffset + col];
    }
  }
}

void HalDisplay::setInverted(bool value) {
  if (inverted.exchange(value) == value)
    return;
  // Inversion is applied at conversion time, so already-presented pixels keep
  // the old polarity until reconverted. Ask the main thread to redo the
  // conversion from the cached frame so the change is visible immediately.
  pendingReconvert.store(true);
}

bool HalDisplay::toggleInverted() {
  const bool value = !inverted.load();
  setInverted(value);
  return value;
}

bool HalDisplay::isInverted() const { return inverted.load(); }

void HalDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
  refreshDisplay(mode, turnOffScreen);
}

void HalDisplay::displayBufferAsync(RefreshMode mode) {
  // SDL presentation is already handed off to the main thread. The framebuffer
  // conversion itself remains synchronous, so advertise no genuine overlap.
  refreshDisplay(mode, false);
}

void HalDisplay::waitRefreshComplete() {}

bool HalDisplay::supportsAsyncRefresh() const { return false; }

void HalDisplay::displayWindow(int, int, int, int) {
  refreshDisplay(RefreshMode::FAST_REFRESH, false);
}

// Called from the render task (background thread): convert framebuffer to
// pixels and flag for present.
void HalDisplay::refreshDisplay(RefreshMode /*mode*/, bool /*turnOffScreen*/) {
  const uint8_t *fb = getFrameBuffer();
  snapshotBwBase(fb);
  renderBwPixels(fb);
}

// Called from the main thread (simulator_main.cpp) to push pixels to SDL.
void HalDisplay::presentIfNeeded() {
  // Service inversion changes first: reconverting sets pendingPresent, so the
  // repolarized pixels ride the present below instead of waiting for the
  // firmware to refresh.
  if (pendingReconvert.exchange(false))
    reconvertLastFrame();

  const bool screenshotDue = hasDueScreenshot();
  if (!pendingPresent.load() && !screenshotDue)
    return;
  pendingPresent.store(false);

  if (!texture || !sdl_renderer)
    return;

  extern GfxRenderer renderer;
  const GfxRenderer::Orientation orientation = renderer.getOrientation();
  applyWindowGeometryIfNeeded(orientation);

  SDL_UpdateTexture(texture, nullptr, pixelBuf,
                    DISPLAY_WIDTH * sizeof(uint32_t));
  // Clear to the field colour, not the default black. On desktop the window is
  // exactly panel-sized so this never shows, but wherever the panel is
  // letterboxed (the phone presents it at 2x inside a taller screen) it is the
  // surround. It defaults to white, matching a blank e-ink page so the panel
  // edge is invisible; SimulatorOverlay::setClearColor lets a host that has an
  // appearance to follow say otherwise.
  const uint32_t field = SimulatorOverlay::clearColor.load();
  SDL_SetRenderDrawColor(sdl_renderer, static_cast<Uint8>(field >> 16),
                         static_cast<Uint8>(field >> 8),
                         static_cast<Uint8>(field), 255);
  SDL_RenderClear(sdl_renderer);

  // For portrait modes the landscape panel texture must be rotated to fill the
  // portrait window. SDL_RenderTextureRotated rotates around the centre of dst,
  // so dst must stay landscape-oriented and be offset so its centre coincides
  // with the window centre. After rotation the result fills the portrait window.
  //
  // Portrait rotateCoordinates stores content rotated 90° CCW in the physical
  // buffer, so we rotate +90° CW here to undo it. PortraitInverted stores
  // content rotated 90° CW → undo with -90°.
  //
  // SDL3 renamed RenderCopy/RenderCopyEx to RenderTexture/RenderTextureRotated
  // and takes float rects; the arithmetic is unchanged.
  constexpr float kW = static_cast<float>(DISPLAY_WIDTH);
  constexpr float kH = static_cast<float>(DISPLAY_HEIGHT);
  SDL_FRect portraitDst = {(kH - kW) / 2.0f, kW / 2.0f - kH / 2.0f, kW, kH};
  SDL_FRect landscapeDst = {0.0f, 0.0f, kW, kH};

  // With a reserved bottom band (SimulatorOverlay::setBottomInset), SDL's
  // letterbox cannot be used -- it always centres in the WHOLE output -- so
  // logical presentation is dropped and the panel is fitted manually into the
  // space above the band, in device pixels. The dst rect is landscape-shaped
  // and centred on the panel's display centre in both orientations: for
  // landscape it IS the display area, for portrait the rotation about its
  // centre turns it into one.
  const int inset = SimulatorOverlay::bottomInset.load();
  const int topBand = SimulatorOverlay::topInset.load();
  const bool manualPlacement = inset > 0 || topBand > 0;
  if (manualPlacement) {
    SDL_SetRenderLogicalPresentation(sdl_renderer, 0, 0,
                                     SDL_LOGICAL_PRESENTATION_DISABLED);
    int outW = 0, outH = 0;
    SDL_GetCurrentRenderOutputSize(sdl_renderer, &outW, &outH);
    const bool portrait = isPortraitOrientation(orientation);
    const float logW = portrait ? kH : kW;
    const float logH = portrait ? kW : kH;
    const float availH =
        SDL_max(1.0f, static_cast<float>(outH - inset - topBand));
    float scale = SDL_min(static_cast<float>(outW) / logW, availH / logH);
    // Keep the pixel-exact policy honest on this path too.
    if (kLogicalPresentation == SDL_LOGICAL_PRESENTATION_INTEGER_SCALE &&
        scale >= 1.0f)
      scale = SDL_floorf(scale);
    // TOP-ALIGNED, not centred: the pad sits directly under the panel's
    // bottom edge (published below), so slack space goes under the pad
    // instead of splitting above and below the page. The alignment is to the
    // BOTTOM of the reserved top band, never to y=0 -- on a phone that band is
    // the status bar and the Island, and the page must start below it.
    const float topMargin =
        topBand + SDL_min(16.0f, (availH - logH * scale) / 2.0f);
    const float cx = static_cast<float>(outW) / 2.0f;
    const float cy = topMargin + logH * scale / 2.0f;
    portraitDst = {cx - kW * scale / 2.0f, cy - kH * scale / 2.0f, kW * scale,
                   kH * scale};
    landscapeDst = portraitDst;
    SimulatorOverlay::panelBottom.store(
        static_cast<int>(topMargin + logH * scale));
    SimulatorOverlay::panelHeight.store(static_cast<int>(logH * scale));
  }

  switch (orientation) {
  case GfxRenderer::Portrait:
    // dst centre = window centre, landscape-sized panel texture.
    SDL_RenderTextureRotated(sdl_renderer, texture, nullptr, &portraitDst, 90.0,
                             nullptr, SDL_FLIP_NONE);
    break;
  case GfxRenderer::PortraitInverted:
    SDL_RenderTextureRotated(sdl_renderer, texture, nullptr, &portraitDst,
                             -90.0, nullptr, SDL_FLIP_NONE);
    break;
  case GfxRenderer::LandscapeClockwise:
    SDL_RenderTextureRotated(sdl_renderer, texture, nullptr, &landscapeDst,
                             180.0, nullptr, SDL_FLIP_NONE);
    break;
  default:
    SDL_RenderTexture(sdl_renderer, texture, nullptr, &landscapeDst);
    break;
  }

  // Overlay chrome lives in the letterboxed margins, which the panel's logical
  // coordinate space cannot address -- so drop logical presentation, hand the
  // painter real pixels, then restore it for the next frame.
  if (SimulatorOverlay::overlayDraw) {
    SDL_SetRenderLogicalPresentation(sdl_renderer, 0, 0,
                                     SDL_LOGICAL_PRESENTATION_DISABLED);
    int outW = 0, outH = 0;
    if (SDL_GetCurrentRenderOutputSize(sdl_renderer, &outW, &outH) && outW > 0)
      SimulatorOverlay::overlayDraw(sdl_renderer, outW, outH);
    int logW = 0, logH = 0;
    getLogicalPresentationSize(orientation, &logW, &logH);
    SDL_SetRenderLogicalPresentation(sdl_renderer, logW, logH,
                                     kLogicalPresentation);
  }

  if (screenshotDue) {
    captureDueScreenshots();
  }
  SDL_RenderPresent(sdl_renderer);
}

bool HalDisplay::shouldQuit() const { return quitRequested.load(); }

void HalDisplay::deepSleep() { presentIfNeeded(); }

uint8_t *HalDisplay::getFrameBuffer() const {
  if (frameBufferLent) {
    return nullptr;
  }
  return frameBufferStorage.data();
}

uint8_t *HalDisplay::lendFrameBufferStorage(uint32_t *sizeOut) {
  if (sizeOut) {
    *sizeOut = frameBufferLent ? 0 : BUFFER_SIZE;
  }
  if (frameBufferLent) {
    return nullptr;
  }
  frameBufferLent = true;
  return frameBufferStorage.data();
}

void HalDisplay::returnFrameBufferStorage() {
  if (!frameBufferLent) {
    return;
  }
  frameBufferStorage.fill(0xFF);
  frameBufferLent = false;
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t *lsbBuffer,
                                      const uint8_t *msbBuffer) {
  copyGrayscaleLsbBuffers(lsbBuffer);
  copyGrayscaleMsbBuffers(msbBuffer);
}
void HalDisplay::displayGrayscaleBase(RefreshMode fallback,
                                      bool turnOffScreen) {
  displayBuffer(fallback, turnOffScreen);
}
void HalDisplay::preconditionGrayscale() {}
void HalDisplay::preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {
}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t *lsbBuffer) {
  copyPlane(grayscalePreviewState.lsbPlane, lsbBuffer,
            grayscalePreviewState.lsbValid);
}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t *msbBuffer) {
  copyPlane(grayscalePreviewState.msbPlane, msbBuffer,
            grayscalePreviewState.msbValid);
}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t *bwBuffer) {
  if (bwBuffer) {
    snapshotBwBase(bwBuffer);
  } else {
    grayscalePreviewState.bwBaseValid = false;
    grayscalePreviewState.bwBase.fill(0);
    clearGrayscalePlanes();
  }
}
void HalDisplay::displayGrayBuffer(bool, const unsigned char *, bool) {
  composeGrayscalePreview();
}

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t *rows,
                                          uint16_t yStart, uint16_t numRows) {
  if (!rows || numRows == 0 || yStart >= DISPLAY_HEIGHT) {
    return;
  }

  const uint16_t rowsToCopy =
      (yStart + numRows > DISPLAY_HEIGHT) ? (DISPLAY_HEIGHT - yStart) : numRows;
  const size_t offset = static_cast<size_t>(yStart) * DISPLAY_WIDTH_BYTES;
  const size_t byteCount =
      static_cast<size_t>(rowsToCopy) * DISPLAY_WIDTH_BYTES;
  auto &plane = lsbPlane ? grayscalePreviewState.lsbPlane
                         : grayscalePreviewState.msbPlane;
  memcpy(plane.data() + offset, rows, byteCount);
  if (lsbPlane) {
    grayscalePreviewState.lsbValid = true;
  } else {
    grayscalePreviewState.msbValid = true;
  }
}
bool HalDisplay::supportsStripGrayscale() const { return true; }

uint16_t HalDisplay::getDisplayWidth() const { return DISPLAY_WIDTH; }
uint16_t HalDisplay::getDisplayHeight() const { return DISPLAY_HEIGHT; }
uint16_t HalDisplay::getDisplayWidthBytes() const {
  return DISPLAY_WIDTH_BYTES;
}
uint32_t HalDisplay::getBufferSize() const { return BUFFER_SIZE; }

HalDisplay display;
