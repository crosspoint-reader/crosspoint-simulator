
#include <SDL3/SDL.h>
#include <unistd.h>

#include "Arduino.h"
#include "HalDisplay.h"
#include "HalGPIO.h"
#include "SimulatorLifecycle.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// On iOS, SDL renames main() and calls it from UIApplicationMain, so the entry
// point below is reached the same way as on desktop -- but the working
// directory, the input source and the process lifetime all differ. Those three
// are the only iOS-specific lines in the simulator; HalGPIO and the firmware are
// untouched.
#if defined(__APPLE__) && TARGET_OS_IPHONE
#define CROSSPOINT_SIM_IOS 1
#include <SDL3/SDL_main.h>

#include "CrossPointHarness.h"
#else
#define CROSSPOINT_SIM_IOS 0
#endif

extern void setup();
extern void loop();
extern HalDisplay display; // defined in main.cpp

int main(int argc, char **argv) {
  SimulatorLifecycle::initProcessArgs(argv);
#if CROSSPOINT_SIM_IOS
  // HalStorage's ./fs_ prefix relies on the CWD, which on iOS is the read-only
  // bundle. Must happen before setup() touches storage.
  CrossPointHarness_prepareFilesystem();
#endif
  setup();
#if CROSSPOINT_SIM_IOS
  // After setup(), because installing the gesture event watch needs SDL
  // initialised and HalDisplay::begin() is what calls SDL_Init.
  CrossPointHarness_begin();
#endif
  while (!display.shouldQuit()) {
    // Clear input edge latches once per frame. update() may be called many
    // times within loop(); edges must survive across those calls and only
    // reset here at the frame boundary.
    gpio.beginFrame();
    loop();
    // SDL must be driven from the main thread on macOS.
    // The render task writes pixels and sets pendingPresent; we flush them
    // here.
    display.presentIfNeeded();
    // Yield to the OS so macOS delivers pending keyboard/window events to SDL.
    // Without this, the tight spin-loop starves the Cocoa event system and key
    // presses are only picked up sporadically. 1 ms also caps the loop at ~1
    // kHz, which matches realistic device behaviour (the real ESP32-C3 is
    // limited by FreeRTOS tick rate and e-ink refresh time).
    SDL_Delay(1);
  }
  SDL_Quit();
#if CROSSPOINT_SIM_IOS
  // iOS treats _exit() as a crash, and an app that kills its own process is
  // reported as one. Return normally instead.
  return 0;
#else
  // Use _exit() instead of return/exit() to bypass C++ global destructors.
  // `activityManager` (and other globals in main.cpp) are constructed before
  // the render task thread starts, and the render task runs a [[noreturn]]
  // infinite loop.  If normal exit() runs global destructors while the render
  // thread is mid-render, the destructor races with the thread → SIGABRT/
  // SIGSEGV → "quit unexpectedly" dialog.  SDL is already torn down above, so
  // calling _exit(0) here is safe.
  _exit(0);
#endif
}
