
#include <SDL3/SDL.h>
#include <unistd.h>

#include "Arduino.h"
#include "CrossPointSettings.h"
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
#include "CrossPointPrefs.h"
#else
#define CROSSPOINT_SIM_IOS 0
#endif

extern void setup();
extern void loop();
extern HalDisplay display; // defined in main.cpp

// Apply Settings > System > Keep Screen Awake to the host's idle timer.
//
// Mechanism, on iOS: SDL_DisableScreenSaver() sets the video device's
// suspend_screensaver flag and calls the driver's SuspendScreenSaver hook
// (SDL_video.c). The UIKit driver installs UIKit_SuspendScreenSaver for that
// hook (src/video/uikit/SDL_uikitvideo.m), whose entire body is
// `app.idleTimerDisabled = (_this->suspend_screensaver != false)` on
// [UIApplication sharedApplication]. So this IS the iOS API, reached through
// SDL -- no Objective-C and no new iOS surface in this repo. On macOS the same
// two entry points route to the Cocoa driver instead, so the desktop build gets
// the equivalent behaviour for free.
//
// IMPORTANT: SDL_VideoInit disables the screensaver *by default* unless
// SDL_HINT_VIDEO_ALLOW_SCREENSAVER is set -- SDL's rationale is that most SDL
// programs are games or media players. Nothing in this repo sets that hint, so
// before this setting existed the app unconditionally held the screen awake.
// Honouring a default-OFF setting is therefore a genuine behaviour change on
// the first frame, not a no-op.
//
// Edge-triggered by construction: the per-frame cost is a byte load and an
// integer compare, and SDL is only entered when the owner actually flips the
// row. The -1 sentinel guarantees the first call after startup always applies,
// which is what makes this cover "at startup" as well as "on change".
//
// Main thread only. It is called from main()'s loop alongside presentIfNeeded()
// for the same reason that one is: UIKit/Cocoa must not be touched off-thread.
// It is not in PadCore (which is pure and SDL-free) and it does not read the
// SDL event queue, so HalGPIO keeps sole ownership of the event pump.
// ON iOS THE SOURCE OF TRUTH IS Settings > CrossPoint X3, not the firmware row.
// Sleep behaviour is a property of the phone, not of the reader, and it is also
// power-state dependent there — the owner sets it separately for battery and
// for charging, which a single firmware boolean cannot express.
//
// The firmware's "Keep Screen Awake" row is deliberately left alone for now and
// therefore has no effect on iOS. That is a known, temporary overlap: two
// controls for one behaviour, pending a ruling on what the firmware row should
// become. Do not "fix" it by deleting either side without that ruling. On the
// desktop simulator the firmware row is still the only control and still works.
static void applyKeepScreenAwake() {
  static int8_t applied = -1;  // -1 = nothing applied yet
#if CROSSPOINT_SIM_IOS
  const int8_t want = CrossPointPrefs_wantsScreenAwake() ? 1 : 0;
#else
  const int8_t want = SETTINGS.keepScreenAwake ? 1 : 0;
#endif
  if (want == applied) return;
  applied = want;
  if (want) {
    SDL_DisableScreenSaver();  // keep the host display on
  } else {
    SDL_EnableScreenSaver();   // let the host dim/lock normally
  }
}

int main(int argc, char **argv) {
  SimulatorLifecycle::initProcessArgs(argv);
#if CROSSPOINT_SIM_IOS
  // HalStorage's ./fs_ prefix relies on the CWD, which on iOS is the read-only
  // bundle. Must happen before setup() touches storage.
  CrossPointHarness_prepareFilesystem();

  // Where a deep-sleep wake lands. On desktop the wake is a process relaunch;
  // iOS cannot exec, so rebootAsPowerWake() jumps back here and setup() runs
  // again against the still-live process. Everything setup() touches has to
  // tolerate that -- HalDisplay::begin() reuses its window, xTaskCreate()
  // dedupes by task name.
  setjmp(SimulatorLifecycle::rebootJumpBuffer());
  SimulatorLifecycle::armRebootJump();
#endif
  setup();
#if CROSSPOINT_SIM_IOS
  // After setup(), because installing the gesture event watch needs SDL
  // initialised and HalDisplay::begin() is what calls SDL_Init.
  //
  // CALLED ON EVERY WAKE, deliberately not gated here. The function splits its
  // own work: registrations happen once (its s_watchesInstalled guard, so N
  // wakes cannot stack N event watches), while the state refreshes -- pad
  // reset, relayout, window scale, and the appearance -- re-run every call
  // because a wake is exactly when they have gone stale. A caller-side gate
  // used to skip the whole function after the first setup(), which made that
  // internal guard unreachable and meant a wake landed with the pre-sleep
  // appearance and layout still applied.
  CrossPointHarness_begin();
#endif
  // Startup application. setup() has loaded settings.json by now, and SDL is
  // initialised (HalDisplay::begin calls SDL_Init). Also covers the iOS wake
  // longjmp, which re-runs setup() and lands back here.
  applyKeepScreenAwake();

  while (!display.shouldQuit()) {
    // Clear input edge latches once per frame. update() may be called many
    // times within loop(); edges must survive across those calls and only
    // reset here at the frame boundary.
    gpio.beginFrame();
    loop();
    // Pick up a mid-run toggle from the Settings screen. No-op unless the value
    // actually changed; see applyKeepScreenAwake().
    applyKeepScreenAwake();
#if CROSSPOINT_SIM_IOS
    // Follow the system light/dark appearance, and repaint after a foreground
    // return (iOS throws away frames presented into that transition, and this
    // app presents too rarely to make another by itself). Same shape as
    // applyKeepScreenAwake above and here for the same reason: edge-triggered
    // inside, main thread only, no present forced unless something changed.
    CrossPointHarness_perFrame();
#endif
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
