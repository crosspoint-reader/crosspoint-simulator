#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalHaptics.h>
#include <HalTiltSensor.h>
#include <SDL.h>
#include <SimulatorLifecycle.h>

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

GfxRenderer renderer;
std::atomic<bool> quitRequested{false};
namespace SimulatorLifecycle {
WakeReason consumeWakeReason() { return WakeReason::None; }
[[noreturn]] void rebootAsPowerWake() { std::abort(); }
} // namespace SimulatorLifecycle

static int homeTaps = 0, homeLongPresses = 0, coverEdges = 0;
static int buttonEdges[7] = {};
static int releaseEdges[7] = {};
static void frame() {
  gpio.beginFrame();
  gpio.update();
  for (int i = 0; i < 7; ++i)
    if (gpio.wasPressed(i))
      ++buttonEdges[i];
  for (int i = 0; i < 7; ++i)
    if (gpio.wasReleased(i))
      ++releaseEdges[i];
  if (gpio.wasHomeKeyTapped())
    ++homeTaps;
  if (gpio.wasHomeKeyLongPressed())
    ++homeLongPresses;
  if (gpio.wasCapacitivePagePressed()) {
    ++coverEdges;
    gpio.update();
    assert(gpio.wasCapacitivePagePressed());
  }
}
static void until(unsigned long time) {
  while (millis() < time) {
    SDL_Delay(1);
    frame();
  }
}
static void mouse(int type, int x, int y) {
  SDL_Event event{};
  event.type = type;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.x = x;
  event.button.y = y;
  assert(SDL_PushEvent(&event) == 1);
  frame();
}
static bool near(float a, float b) { return std::abs(a - b) < 0.003f; }

int main() {
  assert(SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER) == 0);
  setenv("CROSSPOINT_SIM_INPUT_SCRIPT",
         "20:BACK;120:LEFT;220:RIGHT;320:ENTER;440:UP;560:DOWN;"
         "680:PREV:140;850:NEXT:100;980:HOME:80;1100:HOME:800;2000:POWER;2200:"
         "UP:200;2250:PREV:200;2500:NEXT:200;2550:DOWN:200",
         1);
  gpio.begin();
  halClock.begin();
  halTiltSensor.begin();
  assert(BoardConfig::isMetalioEInk4());
  assert(std::strcmp(BoardConfig::ACTIVE.name, "metalio_eink4") == 0);
  assert(FREEINK_DEVICE_METALIO_EINK4 == 1 && FREEINK_MCU_C3 == 0);
  assert(FREEINK_CAP_TOUCH == 1 && FREEINK_CAP_FRONTLIGHT == 0 &&
         FREEINK_CAP_HAPTIC == 1);
  assert(BoardConfig::ACTIVE.displayController ==
         BoardConfig::DisplayController::SSD1677);
  assert(BoardConfig::ACTIVE.touch.controller ==
         BoardConfig::TouchController::Cst816s);
  assert(HalDisplay::DISPLAY_WIDTH == 800 && HalDisplay::DISPLAY_HEIGHT == 480);
  assert(HalDisplay::BUFFER_SIZE == 48000);
  assert(BoardConfig::ACTIVE.input.up == -1 &&
         BoardConfig::ACTIVE.input.down == -1);
  const auto insets = BoardConfig::ACTIVE.viewableInsets;
  assert(insets.top == 9 && insets.right == 3 && insets.bottom == 3 &&
         insets.left == 3);
  assert(gpio.hasTouch() && gpio.hasHomeKey() && !gpio.isXteinkDevice() &&
         !gpio.hasEdgeSideButtons());
  assert(!HalFrontlight::getInstance().present());
  assert(halClock.isAvailable());
  uint8_t hour, minute;
  assert(halClock.getTime(hour, minute) && hour < 24 && minute < 60);
  assert(halTiltSensor.isAvailable() && halTiltSensor.wake() &&
         halTiltSensor.deepSleep());
  HalHaptics::feedback(true, true, 2);
  HalHaptics::longPress(true, 2);
  frame();
  until(350);
  assert(gpio.isPressed(HalGPIO::BTN_CONFIRM));
  assert(!gpio.wasCapacitivePagePressed());
  until(460);
  assert(gpio.isPressed(HalGPIO::BTN_UP) &&
         !gpio.isCapacitivePagePressed(HalGPIO::BTN_UP));
  until(580);
  assert(gpio.isPressed(HalGPIO::BTN_DOWN) &&
         !gpio.isCapacitivePagePressed(HalGPIO::BTN_DOWN));
  until(700);
  assert(gpio.isPressed(HalGPIO::BTN_UP) &&
         gpio.isCapacitivePagePressed(HalGPIO::BTN_UP));
  until(790);
  assert(gpio.getHeldTime() >= 100);
  until(830);
  assert(!gpio.isPressed(HalGPIO::BTN_UP));
  until(870);
  assert(gpio.isCapacitivePagePressed(HalGPIO::BTN_DOWN));
  until(970);
  assert(!gpio.isCapacitivePagePressed(HalGPIO::BTN_DOWN));
  until(1090);
  until(1900);
  assert(homeTaps == 1 && homeLongPresses == 1 && coverEdges == 2);
  frame();
  assert(!gpio.wasHomeKeyLongPressed());
  until(2020);
  assert(gpio.isPressed(HalGPIO::BTN_POWER));
  until(2110);
  assert(!gpio.isPressed(HalGPIO::BTN_POWER));

  assert(buttonEdges[HalGPIO::BTN_BACK] == 0 &&
         buttonEdges[HalGPIO::BTN_LEFT] == 0 &&
         buttonEdges[HalGPIO::BTN_RIGHT] == 0);
  assert(buttonEdges[HalGPIO::BTN_CONFIRM] == 1 &&
         buttonEdges[HalGPIO::BTN_POWER] == 1);
  assert(buttonEdges[HalGPIO::BTN_UP] == 2 &&
         buttonEdges[HalGPIO::BTN_DOWN] == 2);

  // Physical and capacitive sources share an aggregate logical button. A
  // second source must not reset its held time or produce another press; the
  // first source's release must not release the still-held logical button.
  until(2220);
  const auto upStartedAt = millis() - gpio.getHeldTime();
  until(2270);
  assert(gpio.isPressed(HalGPIO::BTN_UP) &&
         gpio.isCapacitivePagePressed(HalGPIO::BTN_UP));
  assert(std::abs(static_cast<long>(millis() - gpio.getHeldTime()) -
                  static_cast<long>(upStartedAt)) < 10);
  until(2410);
  assert(gpio.isPressed(HalGPIO::BTN_UP));
  assert(releaseEdges[HalGPIO::BTN_UP] == 2);
  until(2470);
  assert(!gpio.isPressed(HalGPIO::BTN_UP));
  assert(buttonEdges[HalGPIO::BTN_UP] == 3 &&
         releaseEdges[HalGPIO::BTN_UP] == 3);
  until(2520);
  assert(gpio.isCapacitivePagePressed(HalGPIO::BTN_DOWN));
  until(2710);
  assert(gpio.isPressed(HalGPIO::BTN_DOWN) &&
         !gpio.isCapacitivePagePressed(HalGPIO::BTN_DOWN));
  assert(releaseEdges[HalGPIO::BTN_DOWN] == 2);
  until(2770);
  assert(!gpio.isPressed(HalGPIO::BTN_DOWN));
  assert(buttonEdges[HalGPIO::BTN_DOWN] == 3 &&
         releaseEdges[HalGPIO::BTN_DOWN] == 3);

  // Independent expectations for each orientation's top-left and bottom-right
  // physical panel coordinates, including edge pixels.
  const float corners[][4] = {
      {0, 1, 1, 0}, {1, 1, 0, 0}, {1, 0, 0, 1}, {0, 0, 1, 1}};
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>(orientation));
    for (int corner = 0; corner < 2; ++corner) {
      const int x = corner ? renderer.getScreenWidth() - 1 : 0;
      const int y = corner ? renderer.getScreenHeight() - 1 : 0;
      float nx, ny;
      mouse(SDL_MOUSEBUTTONDOWN, x, y);
      assert(gpio.wasTouchDown(nx, ny));
      assert(near(nx, corners[orientation][corner * 2]));
      assert(near(ny, corners[orientation][corner * 2 + 1]));
      mouse(SDL_MOUSEBUTTONUP, x, y);
      assert(gpio.wasTouchTap(nx, ny));
      assert(!gpio.wasSwipe(nx, ny, nx, ny));
      assert(!gpio.wasHomeKeyTapped());
    }
    float x1, y1, x2, y2;
    mouse(SDL_MOUSEBUTTONDOWN, 10, 100);
    mouse(SDL_MOUSEBUTTONUP, renderer.getScreenWidth() - 10, 100);
    assert(gpio.wasSwipe(x1, y1, x2, y2));
    assert(!gpio.wasTouchTap(x1, y1));
  }
  // Real SDL cover-key events follow the same capacitive HAL surface.
  for (auto code : {SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEDOWN}) {
    const auto button =
        code == SDL_SCANCODE_PAGEUP ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    event.key.keysym.scancode = code;
    assert(SDL_PushEvent(&event) == 1);
    frame();
    assert(gpio.wasCapacitivePagePressed() && gpio.wasPressed(button));
    assert(gpio.isPressed(button) && gpio.isCapacitivePagePressed(button));
    frame();
    assert(!gpio.wasCapacitivePagePressed() &&
           gpio.isCapacitivePagePressed(button));
    event.type = SDL_KEYUP;
    assert(SDL_PushEvent(&event) == 1);
    frame();
    assert(gpio.wasReleased(button) && !gpio.isCapacitivePagePressed(button));
  }
  // Desktop-only buttons must produce neither edges nor held state.
  for (auto code :
       {SDL_SCANCODE_ESCAPE, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT}) {
    SDL_Event e{};
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = code;
    assert(SDL_PushEvent(&e) == 1);
    frame();
    assert(!gpio.wasAnyPressed());
    e.type = SDL_KEYUP;
    assert(SDL_PushEvent(&e) == 1);
    frame();
    assert(!gpio.wasAnyReleased());
  }
  for (int button : {HalGPIO::BTN_BACK, HalGPIO::BTN_LEFT, HalGPIO::BTN_RIGHT})
    assert(!gpio.isPressed(button));
  SDL_Quit();
  std::puts("Metalio identity, capabilities, physical/cover inputs, Home, "
            "clock, tilt, and touch orientations passed");
}
