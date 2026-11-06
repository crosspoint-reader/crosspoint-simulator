#ifdef NDEBUG
#undef NDEBUG
#endif

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
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

static int presses[7] = {};
static void frame() {
  gpio.beginFrame();
  gpio.update();
  for (int i = 0; i < 7; ++i)
    presses[i] += gpio.wasPressed(i);
  assert(!gpio.wasCapacitivePagePressed());
  assert(!gpio.wasHomeKeyPressed() && !gpio.wasHomeKeyTapped() &&
         !gpio.wasHomeKeyLongPressed());
}
static void until(unsigned long time) {
  while (millis() < time) {
    SDL_Delay(1);
    frame();
  }
}
static void key(SDL_Scancode code, bool down, bool repeat = false) {
  SDL_Event event{};
  event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
  event.key.keysym.scancode = code;
  event.key.repeat = repeat;
  assert(SDL_PushEvent(&event) == 1);
  frame();
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

int main(int argc, char **argv) {
  assert(SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER) == 0);
  SimulatorLifecycle::initProcessArgs(argv);
  if (SimulatorLifecycle::consumeWakeReason() ==
      SimulatorLifecycle::WakeReason::PowerButton) {
    gpio.begin();
    assert(gpio.hasToggleSwitch() && gpio.isToggleSwitchOn());
    SDL_Quit();
    std::puts("Picco switch position survived sleep/relaunch; input and "
              "profile checks passed");
    return 0;
  }
  setenv("CROSSPOINT_SIM_SLIDE_SWITCH", "off", 1);
  setenv("CROSSPOINT_SIM_INPUT_SCRIPT",
         "20:SWITCH_ON;140:SWITCH_OFF;240:ENTER;360:UP;480:DOWN;600:POWER;"
         "720:BACK;840:LEFT;960:RIGHT;1080:HOME;1200:PREV;1320:NEXT;2000:POWER",
         1);
  gpio.begin();
  assert(BoardConfig::isPicco() && FREEINK_DEVICE_PICCO == 1);
  assert(!FREEINK_MCU_C3 && !FREEINK_CAP_HAPTIC);
  assert(FREEINK_CAP_TOUCH && FREEINK_CAP_FRONTLIGHT && FREEINK_CAP_WARMLIGHT);
  assert(BoardConfig::ACTIVE.displayController ==
         BoardConfig::DisplayController::SSD1677);
  assert(BoardConfig::ACTIVE.touch.controller ==
         BoardConfig::TouchController::Tma525c);
  assert(HalDisplay::DISPLAY_WIDTH == 800 && HalDisplay::DISPLAY_HEIGHT == 480);
  assert(HalDisplay::BUFFER_SIZE == 48000);
  assert(BoardConfig::ACTIVE.displayWidth == HalDisplay::DISPLAY_WIDTH &&
         BoardConfig::ACTIVE.displayHeight == HalDisplay::DISPLAY_HEIGHT);
  assert(BoardConfig::ACTIVE.input.up == 21 &&
         BoardConfig::ACTIVE.input.down == 9 &&
         BoardConfig::ACTIVE.input.toggleSwitch == 6);
  assert(gpio.hasTouch() && !gpio.hasHomeKey() && gpio.hasToggleSwitch());
  assert(!gpio.isXteinkDevice() && !gpio.hasEdgeSideButtons());
  assert(BoardConfig::hasI2cFrontlight() && !BoardConfig::hasPwmFrontlight());
  assert(!powerManager.holdWhileUsbPowered(
      gpio, [](bool, uint16_t) { std::abort(); }));
  auto &light = HalFrontlight::getInstance();
  assert(light.present() && light.hasColorTemperature());
  light.begin(65, 75, true);
  assert(light.isOn() && light.brightness() == 65 && light.warmth() == 75);
  light.setBrightness(255);
  light.setWarmth(255);
  assert(light.brightness() == 100 && light.warmth() == 100);
  light.setOn(false);
  assert(!light.isOn());
  halClock.begin();
  uint8_t hour, minute;
  assert(halClock.isAvailable() && halClock.getTime(hour, minute));
  assert(hour < 24 && minute < 60);
  halTiltSensor.begin();
  assert(halTiltSensor.isAvailable() && halTiltSensor.wake());
  assert(!halTiltSensor.wasTiltedForward() && !halTiltSensor.wasTiltedBack());

  assert(!gpio.isToggleSwitchOn());
  until(60);
  assert(gpio.isToggleSwitchOn());
  gpio.update();
  assert(gpio.isToggleSwitchOn());
  until(180);
  assert(!gpio.isToggleSwitchOn());
  until(1460);
  for (int button : {HalGPIO::BTN_CONFIRM, HalGPIO::BTN_UP, HalGPIO::BTN_DOWN,
                     HalGPIO::BTN_POWER})
    assert(presses[button] == 1 && !gpio.isPressed(button));
  for (int button :
       {HalGPIO::BTN_BACK, HalGPIO::BTN_LEFT, HalGPIO::BTN_RIGHT}) {
    assert(presses[button] == 0 && !gpio.isPressed(button));
  }

  key(SDL_SCANCODE_T, true);
  assert(gpio.isToggleSwitchOn() && !gpio.wasAnyPressed());
  key(SDL_SCANCODE_T, true, true);
  assert(gpio.isToggleSwitchOn());
  key(SDL_SCANCODE_T, false);
  assert(gpio.isToggleSwitchOn() && !gpio.wasAnyReleased());
  key(SDL_SCANCODE_T, true);
  assert(!gpio.isToggleSwitchOn());
  key(SDL_SCANCODE_T, false);

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
    }
    float x1, y1, x2, y2;
    mouse(SDL_MOUSEBUTTONDOWN, 10, 100);
    mouse(SDL_MOUSEBUTTONUP, renderer.getScreenWidth() - 10, 100);
    assert(gpio.wasSwipe(x1, y1, x2, y2));
    assert(!gpio.wasTouchTap(x1, y1));
  }
  for (auto code :
       {SDL_SCANCODE_ESCAPE, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT}) {
    key(code, true);
    assert(!gpio.wasAnyPressed());
    key(code, false);
    assert(!gpio.wasAnyReleased());
  }
  const bool heldWake = argc > 1 && std::strcmp(argv[1], "held") == 0;
  if (heldWake) {
    key(SDL_SCANCODE_T, true);
    powerManager.startDeepSleep(gpio, true);
  } else {
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    event.key.keysym.scancode = SDL_SCANCODE_T;
    assert(SDL_PushEvent(&event) == 1);
    // Moving the switch while asleep must retain its position without waking;
    // the scheduled Power press wakes into the assertion above.
    powerManager.startDeepSleep(gpio);
  }
  assert(false && "Power wake should relaunch the process");
}
