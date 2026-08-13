#include "HalDisplay.h"

#include <GfxRenderer.h>
#include <SDL.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static SDL_Window *window = nullptr;
static SDL_Renderer *sdl_renderer = nullptr;
static SDL_Texture *texture = nullptr;
// Render the simulator at full panel size. The previous 0.5x window was too
// small. With 1:1 pixel mapping, the simulator can be used for testing fine
// details.
static constexpr int SIMULATOR_WINDOW_SCALE = 1;

// Pixel buffer written by the render task, read by the main thread for
// SDL_RenderPresent. On macOS, SDL calls must happen on the main thread.
static uint32_t
    pixelBuf[HalDisplay::DISPLAY_WIDTH * HalDisplay::DISPLAY_HEIGHT];
static std::mutex pixelBufMutex;
static std::atomic<bool> pendingPresent{false};
// Written by HalGPIO::update() (which owns SDL event polling); read by
// shouldQuit().
std::atomic<bool> quitRequested{false};

static int currentWindowWidth = 0;
static int currentWindowHeight = 0;

namespace {

struct GrayscalePreviewState {
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> bwBase{};
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> lsbPlane{};
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> msbPlane{};
  bool bwBaseValid = false;
  bool lsbValid = false;
  bool msbValid = false;
};

constexpr uint8_t kGrayWhite = 255;
constexpr uint8_t kGrayLight = 200;
constexpr uint8_t kGrayDark = 96;
constexpr uint8_t kGrayBlack = 0;

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
const std::thread::id simulatorMainThread = std::this_thread::get_id();

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
  if (SDL_GetRendererOutputSize(sdl_renderer, &width, &height) != 0 ||
      width <= 0 || height <= 0) {
    std::cerr << "[SIM] Cannot determine screenshot size: " << SDL_GetError()
              << std::endl;
    return false;
  }

  std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
  if (SDL_RenderReadPixels(sdl_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                           pixels.data(), width * sizeof(uint32_t)) != 0) {
    std::cerr << "[SIM] Cannot read screenshot pixels: " << SDL_GetError()
              << std::endl;
    return false;
  }

  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
      pixels.data(), width, height, 32, width * sizeof(uint32_t),
      SDL_PIXELFORMAT_ARGB8888);
  if (!surface) {
    std::cerr << "[SIM] Cannot create screenshot surface: " << SDL_GetError()
              << std::endl;
    return false;
  }

  const bool saved = SDL_SaveBMP(surface, path.c_str()) == 0;
  if (!saved) {
    std::cerr << "[SIM] Cannot save screenshot " << path << ": "
              << SDL_GetError() << std::endl;
  } else {
    std::cerr << "[SIM] Saved screenshot: " << path << std::endl;
  }
  SDL_FreeSurface(surface);
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

uint32_t argbGray(uint8_t level) {
  return 0xFF000000u | (static_cast<uint32_t>(level) << 16) |
         (static_cast<uint32_t>(level) << 8) | level;
}

bool getBit(const uint8_t *buffer, int x, int y) {
  const int byteIdx = (y * HalDisplay::DISPLAY_WIDTH + x) / 8;
  const int bitIdx = 7 - (x % 8);
  return (buffer[byteIdx] & (1 << bitIdx)) != 0;
}

void renderBwPixels(const uint8_t *fb) {
  const std::lock_guard<std::mutex> lock(pixelBufMutex);
  const bool invert = display.isInverted();
  for (int y = 0; y < HalDisplay::DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < HalDisplay::DISPLAY_WIDTH; x++) {
      const bool white = getBit(fb, x, y);
      pixelBuf[y * HalDisplay::DISPLAY_WIDTH + x] =
          (white != invert) ? 0xFFFFFFFFu : 0xFF000000u;
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
  const std::lock_guard<std::mutex> lock(pixelBufMutex);
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

      uint8_t level = kGrayWhite;
      if (!baseWhite) {
        if (msbActive) {
          level = lsbActive ? kGrayDark : kGrayLight;
        } else if (lsbActive) {
          level = kGrayDark;
        } else {
          level = kGrayBlack;
        }
      }

      if (display.isInverted())
        level = static_cast<uint8_t>(255 - level);
      pixelBuf[y * HalDisplay::DISPLAY_WIDTH + x] = argbGray(level);
    }
  }
  pendingPresent.store(true);
}

} // namespace

static bool isPortraitOrientation(GfxRenderer::Orientation orientation) {
  return orientation == GfxRenderer::Portrait ||
         orientation == GfxRenderer::PortraitInverted;
}

static void getLogicalWindowSize(GfxRenderer::Orientation orientation,
                                 int *width, int *height) {
  const bool isPortrait = isPortraitOrientation(orientation);
  *width =
      (isPortrait ? HalDisplay::DISPLAY_HEIGHT : HalDisplay::DISPLAY_WIDTH) *
      SIMULATOR_WINDOW_SCALE;
  *height =
      (isPortrait ? HalDisplay::DISPLAY_WIDTH : HalDisplay::DISPLAY_HEIGHT) *
      SIMULATOR_WINDOW_SCALE;
}

static void applyWindowGeometryIfNeeded(GfxRenderer::Orientation orientation) {
  if (!window || !sdl_renderer)
    return;

  int winW = 0;
  int winH = 0;
  getLogicalWindowSize(orientation, &winW, &winH);
  if (winW == currentWindowWidth && winH == currentWindowHeight)
    return;

  SDL_SetWindowSize(window, winW, winH);
  SDL_RenderSetLogicalSize(sdl_renderer, winW, winH);
  currentWindowWidth = winW;
  currentWindowHeight = winH;
}

HalDisplay::HalDisplay() {}
HalDisplay::~HalDisplay() {}

#if defined(SIMULATOR_DISPLAY_UC8179)
#define SIMULATOR_CONTROLLER_TITLE "UC8179"
#elif defined(SIMULATOR_DISPLAY_UC8279)
#define SIMULATOR_CONTROLLER_TITLE "UC8279"
#else
#define SIMULATOR_CONTROLLER_TITLE "SSD1677"
#endif

#if defined(SIMULATOR_DEVICE_STICKY)
static constexpr const char *WINDOW_TITLE =
    "Simulator - Seeed Sticky (SSD1677)";
#elif defined(SIMULATOR_DEVICE_X4_PRO)
static constexpr const char *WINDOW_TITLE =
    "Simulator - XTEINK X4 Pro (" SIMULATOR_CONTROLLER_TITLE ")";
#elif defined(SIMULATOR_DEVICE_X3)
#if defined(SIMULATOR_DISPLAY_UC8279)
static constexpr const char *WINDOW_TITLE = "Simulator - XTEINK X3 (UC8279d)";
#else
static constexpr const char *WINDOW_TITLE = "Simulator - XTEINK X3 (UC8253)";
#endif
#else
static constexpr const char *WINDOW_TITLE =
    "Simulator - XTEINK X4 (" SIMULATOR_CONTROLLER_TITLE ")";
#endif

#undef SIMULATOR_CONTROLLER_TITLE

void HalDisplay::begin() {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError()
              << std::endl;
    return;
  }

  int winW = 0;
  int winH = 0;
  extern GfxRenderer renderer;
  getLogicalWindowSize(renderer.getOrientation(), &winW, &winH);

  // SDL_WINDOW_ALLOW_HIGHDPI lets the renderer use full Retina/HiDPI pixels on
  // macOS so we get crisp 1:1 rendering instead of a blurry upscale.
  window = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, winW, winH,
                            SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
  sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

  // Keep all rendering logic in logical (winW×winH) coordinates; SDL maps to
  // drawable pixels.
  SDL_RenderSetLogicalSize(sdl_renderer, winW, winH);
  currentWindowWidth = winW;
  currentWindowHeight = winH;

  // Linear filtering: Bayer-dithered pixels average to correct gray at scaled
  // sizes rather than showing harsh black/white patterns.
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH,
                              DISPLAY_HEIGHT);
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
    const uint16_t destOffset = destY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t srcOffset = row * imageWidthBytes;
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
    const uint16_t destOffset = destY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t srcOffset = row * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES)
        break;
      fb[destOffset + col] &= imageData[srcOffset + col];
    }
  }
}

void HalDisplay::setInverted(bool value) { inverted = value; }

bool HalDisplay::toggleInverted() {
  inverted = !inverted;
  return inverted;
}

bool HalDisplay::isInverted() const { return inverted; }

void HalDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
  refreshDisplay(mode, turnOffScreen);
  if (std::this_thread::get_id() == simulatorMainThread) {
    presentIfNeeded();
  }
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
  const bool screenshotDue = hasDueScreenshot();
  if (!pendingPresent.exchange(false) && !screenshotDue)
    return;

  if (!texture || !sdl_renderer)
    return;

  extern GfxRenderer renderer;
  const GfxRenderer::Orientation orientation = renderer.getOrientation();
  applyWindowGeometryIfNeeded(orientation);

  {
    const std::lock_guard<std::mutex> lock(pixelBufMutex);
    SDL_UpdateTexture(texture, nullptr, pixelBuf,
                      DISPLAY_WIDTH * sizeof(uint32_t));
  }
  SDL_RenderClear(sdl_renderer);

  // For portrait modes the landscape panel texture must be rotated to fill the
  // portrait window. SDL_RenderCopyEx rotates around the centre of dst, so dst
  // must stay landscape-oriented and be offset so its centre coincides with the
  // window centre. After rotation the result fills the portrait window.
  //
  // Portrait rotateCoordinates stores content rotated 90° CCW in the physical
  // buffer, so we rotate +90° CW here to undo it. PortraitInverted stores
  // content rotated 90° CW → undo with -90°.
  switch (orientation) {
  case GfxRenderer::Portrait: {
    // dst centre = window centre, landscape-sized panel texture.
    SDL_Rect dst = {(DISPLAY_HEIGHT - DISPLAY_WIDTH) / 2,
                    DISPLAY_WIDTH / 2 - DISPLAY_HEIGHT / 2, DISPLAY_WIDTH,
                    DISPLAY_HEIGHT};
    SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, 90.0, nullptr,
                     SDL_FLIP_NONE);
    break;
  }
  case GfxRenderer::PortraitInverted: {
    SDL_Rect dst = {(DISPLAY_HEIGHT - DISPLAY_WIDTH) / 2,
                    DISPLAY_WIDTH / 2 - DISPLAY_HEIGHT / 2, DISPLAY_WIDTH,
                    DISPLAY_HEIGHT};
    SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, -90.0, nullptr,
                     SDL_FLIP_NONE);
    break;
  }
  case GfxRenderer::LandscapeClockwise: {
    SDL_Rect dst = {0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT};
    SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, 180.0, nullptr,
                     SDL_FLIP_NONE);
    break;
  }
  default: {
    SDL_Rect dst = {0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT};
    SDL_RenderCopy(sdl_renderer, texture, nullptr, &dst);
    break;
  }
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
