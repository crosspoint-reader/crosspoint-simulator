#ifdef NDEBUG
#undef NDEBUG
#endif

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <SDL.h>
#include <SimulatorLifecycle.h>

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

GfxRenderer renderer;
std::atomic<bool> quitRequested{false};
namespace SimulatorLifecycle {
WakeReason consumeWakeReason() { return WakeReason::None; }
[[noreturn]] void rebootAsPowerWake() { std::abort(); }
} // namespace SimulatorLifecycle

static int presses[7] = {};
static int homeTaps = 0, homeHolds = 0;
static void frame() {
  gpio.beginFrame();
  gpio.update();
  for (int i = 0; i < 7; ++i)
    presses[i] += gpio.wasPressed(i);
  homeTaps += gpio.wasHomeKeyTapped();
  homeHolds += gpio.wasHomeKeyLongPressed();
  assert(!gpio.wasCapacitivePagePressed());
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
         "680:PREV;800:NEXT;920:HOME:80;1040:HOME:800;1940:POWER",
         1);
  gpio.begin();
  assert(!BatteryMonitor::loadDesignCapacity());
  halClock.begin();
  halTiltSensor.begin();
  assert(BoardConfig::isEegoA4() && FREEINK_DEVICE_EEGO_A4 == 1);
  assert(!FREEINK_MCU_C3 && !FREEINK_CAP_HAPTIC);
  assert(FREEINK_CAP_TOUCH && FREEINK_CAP_FRONTLIGHT && FREEINK_CAP_WARMLIGHT);
  assert(BoardConfig::ACTIVE.displayController ==
         BoardConfig::DisplayController::UC8279C);
  assert(BoardConfig::ACTIVE.touch.controller ==
         BoardConfig::TouchController::Gslx680);
  assert(HalDisplay::DISPLAY_WIDTH == 768 && HalDisplay::DISPLAY_HEIGHT == 552);
  assert(HalDisplay::BUFFER_SIZE == 52992);
  assert(BoardConfig::ACTIVE.displayWidth == HalDisplay::DISPLAY_WIDTH);
  assert(BoardConfig::ACTIVE.displayHeight == HalDisplay::DISPLAY_HEIGHT);
  const auto insets = BoardConfig::ACTIVE.viewableInsets;
  assert(insets.top == 24 && insets.right == 24 && insets.bottom == 8 &&
         insets.left == 24);
  assert(BoardConfig::ACTIVE.input.up == 5 &&
         BoardConfig::ACTIVE.input.down == 7);
  assert(gpio.hasTouch() && gpio.hasHomeKey());
  assert(!gpio.isXteinkDevice() && !gpio.hasEdgeSideButtons());
  assert(BoardConfig::hasI2cFrontlight() && !BoardConfig::hasPwmFrontlight());
  auto &light = HalFrontlight::getInstance();
#if defined(SIMULATOR_EEGO_NO_FRONTLIGHT)
  assert(!light.present() && !light.hasColorTemperature());
  light.begin(65, 75, true);
  light.setOn(true);
  assert(!light.isOn());
#else
  assert(light.present() && light.hasColorTemperature());
  light.begin(65, 75, true);
  assert(light.isOn() && light.brightness() == 65 && light.warmth() == 75);
  light.setBrightness(255);
  light.setWarmth(255);
  assert(light.brightness() == 100 && light.warmth() == 100);
  light.setOn(false);
  assert(!light.isOn());
#endif
  uint8_t hour, minute;
  assert(halClock.isAvailable() && halClock.getTime(hour, minute));
  assert(hour < 24 && minute < 60);
  assert(!halTiltSensor.isAvailable() && !halTiltSensor.wake());
  frame();
  until(460);
  assert(gpio.isPressed(HalGPIO::BTN_UP));
  until(580);
  assert(gpio.isPressed(HalGPIO::BTN_DOWN));
  until(1960);
  assert(gpio.isPressed(HalGPIO::BTN_POWER));
  until(2070);
  assert(!gpio.isPressed(HalGPIO::BTN_POWER));
  assert(presses[HalGPIO::BTN_UP] == 1 && presses[HalGPIO::BTN_DOWN] == 1);
  assert(presses[HalGPIO::BTN_POWER] == 1 && homeTaps == 1 && homeHolds == 1);
  for (int button : {HalGPIO::BTN_BACK, HalGPIO::BTN_CONFIRM, HalGPIO::BTN_LEFT,
                     HalGPIO::BTN_RIGHT})
    assert(presses[button] == 0 && !gpio.isPressed(button));

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
    }
    float x1, y1, x2, y2;
    mouse(SDL_MOUSEBUTTONDOWN, 10, 100);
    mouse(SDL_MOUSEBUTTONUP, renderer.getScreenWidth() - 10, 100);
    assert(gpio.wasSwipe(x1, y1, x2, y2));
    assert(!gpio.wasTouchTap(x1, y1));
  }
  for (auto code : {SDL_SCANCODE_ESCAPE, SDL_SCANCODE_RETURN, SDL_SCANCODE_LEFT,
                    SDL_SCANCODE_RIGHT}) {
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    event.key.keysym.scancode = code;
    assert(SDL_PushEvent(&event) == 1);
    frame();
    assert(!gpio.wasAnyPressed());
    event.type = SDL_KEYUP;
    assert(SDL_PushEvent(&event) == 1);
    frame();
    assert(!gpio.wasAnyReleased());
  }
  SDL_Quit();
  std::puts("EEGO geometry, capabilities, inputs, clock, frontlight and touch "
            "orientations passed");
}
