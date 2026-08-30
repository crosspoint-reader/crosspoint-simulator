
#include <SDL.h>
#include <unistd.h>

#include "Arduino.h"
#include "HalDisplay.h"
#include "HalGPIO.h"
#include "SimulatorLifecycle.h"
#if defined(CROSSPOINT_SIM_HEAP_ARENA)
#include "SimulatorHeap.h"
#endif

extern void setup();
extern void loop();
extern HalDisplay display; // defined in main.cpp

int main(int argc, char **argv) {
  SimulatorLifecycle::initProcessArgs(argv);
  setup();
#if defined(CROSSPOINT_SIM_HEAP_ARENA)
  // Start with the configured post-setup budget so SDL and host scaffolding do
  // not consume the emulated firmware heap.
  SimulatorHeap::activateFromEnv();
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
#if defined(CROSSPOINT_SIM_HEAP_ARENA)
    {
      SimulatorHeap::HostHeapScope hostHeapScope;
      display.presentIfNeeded();
    }
#else
    display.presentIfNeeded();
#endif
    // Yield to the OS so macOS delivers pending keyboard/window events to SDL.
    // Without this, the tight spin-loop starves the Cocoa event system and key
    // presses are only picked up sporadically. 1 ms also caps the loop at ~1
    // kHz, which matches realistic device behaviour (the real ESP32-C3 is
    // limited by FreeRTOS tick rate and e-ink refresh time).
    SDL_Delay(1);
  }
  SDL_Quit();
  // Use _exit() instead of return/exit() to bypass C++ global destructors.
  // `activityManager` (and other globals in main.cpp) are constructed before
  // the render task thread starts, and the render task runs a [[noreturn]]
  // infinite loop.  If normal exit() runs global destructors while the render
  // thread is mid-render, the destructor races with the thread → SIGABRT/
  // SIGSEGV → "quit unexpectedly" dialog.  SDL is already torn down above, so
  // calling _exit(0) here is safe.
  _exit(0);
}
