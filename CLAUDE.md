# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A simulator for [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) firmware. On the desktop it is **not** a standalone app: it ships as a PlatformIO library that downstream firmware adds as a `lib_dep` (named `simulator`) and builds with `platform = native` and `-DSIMULATOR`. The result is the firmware compiled as a host binary, with the e-ink display rendered into an SDL3 window.

There is no desktop build target inside this repo. Desktop build and run happen in the consuming firmware project. See [README.md](README.md) for end-user setup, and [.claude/CONTEXT-sim-notes.md](.claude/CONTEXT-sim-notes.md) for the deep architecture notes and bug-fix history (read this before non-trivial changes).

**There is also a native iOS target**, driven by CMake over the same source set — see [ios/README.md](ios/README.md). It compiles the firmware plus this HAL for `arm64-apple-ios` and presents the panel on an iPhone with an on-screen button pad. One source set, two toolchains; the desktop PlatformIO build stays the canary, so keep it green.

**SDL3, not SDL2.** Both toolchains build against SDL3 so the shared sources need no per-platform SDL shim. The desktop env gets its flags from `!pkg-config --cflags --libs sdl3`.

## Build and run (from the consuming firmware repo)

```bash
pio run -e simulator -t run_simulator   # build + launch
pio run -e simulator                    # build only, then .pio/build/simulator/program
rm -rf ./fs_/.crosspoint/               # clear stale on-disk caches after storage/cache changes
```

**Render scale is an env var, not a build flag.** [src/HalDisplay.h](src/HalDisplay.h)'s `CROSSPOINT_RENDER_SCALE` defaults to 1 (mirror the device) and consumers opt in to supersampling. CrossPoint's desktop simulator envs keep that default — a plain `pio run -e simulator_x3` is device-exact — and `scripts/sim_render_scale.py` reads the env var to opt in:

```bash
CROSSPOINT_RENDER_SCALE=2 pio run -e simulator_x3 -t run_simulator
```

It must be an env var rather than `PLATFORMIO_BUILD_FLAGS="-DCROSSPOINT_RENDER_SCALE=1"`, because PlatformIO *appends* that variable: with a `-D` in `platformio.ini` the compiler saw both definitions and warned `-Wmacro-redefined` in every translation unit, which buried the real warnings. Prepending `-UCROSSPOINT_RENDER_SCALE` does not help — SCons parks unrecognised flags in `CCFLAGS`, and `CCCOM` expands `$CCFLAGS` *before* `$_CPPDEFFLAGS`, so the undef is consumed before either `-D` is seen. The pre-script picks the value once, so exactly one definition ever reaches the command line. An explicit `-D` still wins if you pass one. iOS is unaffected: [ios/CMakeLists.txt](ios/CMakeLists.txt) sets the macro directly.

To confirm which scale a binary actually got, count hi-res font loads (1 = 2x, 0 = 1x) — do not use the screenshot dimensions, since the window is sized in *logical* panel pixels and tracks `CROSSPOINT_SIM_WINDOW_SCALE`, not the render scale:

```bash
CROSSPOINT_SIM_INPUT_SCRIPT='5000:QUIT' SDL_VIDEODRIVER=dummy .pio/build/simulator_x3/program 2>&1 | grep -c 'hi-res'
```

For local dev against this repo, the firmware's `platformio.ini` should reference it as `simulator=symlink://../crosspoint-simulator` instead of the git URL.

There is no linter and no per-file build commands; most changes are "tested" by running the simulator and exercising the affected feature. Two real tests do exist in `tests/`, run them when touching input or sleep paths:

```bash
c++ -std=c++17 -Iios tests/pad_core_test.cpp ios/PadCore.cpp -o /tmp/pad_core_test && /tmp/pad_core_test
tests/test_sleep_wake.sh <firmware-checkout>   # needs the desktop binary built
```

`pad_core_test` covers the iOS pad's finger→button passthrough (PadCore is pure and clock-free by design — do not add timers to it). `test_sleep_wake.sh` pins the deep-sleep wake edge-latch: a 1 ms synthetic POWER tap during sleep must relaunch the process (the sleep loop consumes an edge set by `injectButtonDown`, because a fast tap's down and up can both land in one pump burst and leave no level to poll).

## Architecture

The simulator is a collection of host-side reimplementations of the firmware's hardware abstraction layer (HAL) and its Arduino/ESP-IDF dependencies. Each `Hal*.cpp/.h` here corresponds to a `Hal*` class in the firmware's `lib/hal/`, and **must keep the same public surface** or the firmware will not link.

**The HAL stub rule.** When the firmware adds a new method to a HAL class and calls it, the simulator fails to link until a matching stub is added to the corresponding `Hal*.cpp` here. Most additions are one-line no-ops. This is the single most common reason a simulator build breaks after pulling firmware updates.

**Why the simulator's design has the shape it does** (the non-obvious parts):

- **SDL on main thread.** macOS requires all SDL calls to come from the main thread, but firmware drives rendering from a FreeRTOS render task. The split lives in [src/HalDisplay.cpp](src/HalDisplay.cpp): `refreshDisplay` (background thread) converts the 1bpp framebuffer to ARGB and sets an atomic `pendingPresent` flag. `presentIfNeeded` (called from `simulator_main` on the main thread) does the actual SDL upload and present. Do not call SDL render functions from anywhere else.
- **Orientation rotation lives in two places.** The firmware's renderer rotates content into the landscape framebuffer (90 CCW for `Portrait`). The simulator undoes that with `SDL_RenderTextureRotated`. If you change one, change the other. The dst rect is landscape-shaped and centre-offset because the rotation happens around the dst centre.
- **HiDPI / dithering.** `SDL_WINDOW_HIGH_PIXEL_DENSITY` plus `SDL_SetRenderLogicalPresentation`, and a scale mode set on the texture (SDL3 replaced the global `SDL_HINT_RENDER_SCALE_QUALITY` hint with a per-texture setting, so it must come *after* `SDL_CreateTexture`). Without these, Bayer-dithered grays render as harsh black/white stripes on Retina.
- **Presentation policy is keyed on intent, not platform.** `CROSSPOINT_SIM_PIXEL_EXACT` selects `INTEGER_SCALE` + `SCALEMODE_NEAREST`; without it the build gets letterbox + linear filtering. Linear is right at 1:1, where Bayer dither averaging to grey is what e-ink actually looks like; exact pixels are right wherever the panel is scaled up (the phone presents it at 2x), because a fractional scale or a linear filter greys the dither and every rendering judgement made against it is a lie.
- **`SimulatorOverlay` for chrome outside the panel.** [src/SimulatorOverlay.h](src/SimulatorOverlay.h) is a free hook `presentIfNeeded` calls, deliberately *not* a `HalDisplay` method — the HAL's public surface must mirror the firmware's, and on-screen chrome has no analog on real hardware. The callback runs with logical presentation disabled and receives the real output size, so it can paint the letterboxed margins the panel's logical space cannot reach. `requestPresent()` exists because an e-ink firmware presents rarely, so overlay state changes would otherwise not appear until the next page render.
- **Panel palette + reserved pad band.** The 1bpp/AA framebuffer presents as tinted ink on tinted paper (`PanelPalette` in [src/HalDisplay.cpp](src/HalDisplay.cpp): 2D2D2D-on-FBFBF9 light, E0E0DE-on-121212 dark; the dark ramp's ink→paper direction IS the inversion — no separate 255−level flip). `SimulatorOverlay::setBottomInset` reserves a bottom band for chrome: the panel then fits TOP-ALIGNED above the band (manual placement, integer scale preserved under `CROSSPOINT_SIM_PIXEL_EXACT`) and publishes its bottom edge via `panelBottomPx()` — the iOS pad anchors to it. Desktop keeps inset 0 and the plain letterbox path.
- **Inversion changes re-present from a cached frame.** `setInverted` posts an atomic reconvert request that `presentIfNeeded` (main thread) services from the cached BW base and grayscale AA planes — inversion applies at 1bpp→ARGB conversion time, and e-ink firmware refreshes rarely, so without this a dark-mode flip would wait for the next page turn. `SimulatorOverlay::setPanelDark` is the single polarity entry point: the iOS harness follows the system appearance through it, and `CROSSPOINT_SIM_DARK=1/0` forces either state for headless runs. Book covers/images render as negatives in dark mode; accepted for now.
- **POSIX fds, not std::fstream, in [src/HalStorage.cpp](src/HalStorage.cpp).** This was a deliberate rewrite. fstream's separate get/put pointers, eofbit-blocks-seek behaviour, and write-only seek restrictions caused several silent-corruption bugs. Do not reintroduce fstream here. All paths are prefixed with `./fs_` so the simulated filesystem stays sandboxed under the binary's working directory; `/books/` on the SD card maps to `./fs_/books/`. Directory iteration skips entries starting with `.`.
- **FreeRTOS shim.** [src/freertos/](src/freertos/) maps `xTaskCreate` to `std::thread`, task notifies to a condvar + counter, and `SemaphoreHandle_t` to `std::recursive_mutex`. A `thread_local SimTaskHandle*` lets each task thread find its own handle.
- **`_exit(0)` not `return 0`, on desktop.** [src/simulator_main.cpp](src/simulator_main.cpp) ends with `_exit(0)` after `SDL_Quit()` to skip C++ global destructors. The render task is `[[noreturn]]`, so running destructors while it is mid-render races and produces a "quit unexpectedly" dialog. Keep this. iOS is the one exception — it reports a self-terminating process as a crash, so that build returns normally.
- **Time uses `steady_clock`.** `millis()` / `micros()` in [src/Arduino.h](src/Arduino.h) deliberately use `steady_clock`, not `system_clock`, so wall-clock changes do not perturb timing.

**Host-specific code paths:**

- MD5: [src/MD5Builder.h](src/MD5Builder.h) is a thin dispatcher that auto-selects the implementation via `#ifdef __APPLE__` / `#elif __linux__`. [src/MD5Builder_mac.h](src/MD5Builder_mac.h) uses CommonCrypto; [src/MD5Builder_linux.h](src/MD5Builder_linux.h) uses OpenSSL. No downstream swapping is needed - just include `MD5Builder.h`.
- Web server shims: [src/WebServer.cpp](src/WebServer.cpp), [src/WebSocketsServer.cpp](src/WebSocketsServer.cpp), and [src/NetworkClient.cpp](src/NetworkClient.cpp) expose firmware port 80 as `http://127.0.0.1:8080/` and port 81 WebSockets as `ws://127.0.0.1:8081/`. `CROSSPOINT_SIM_HTTP_PORT` moves the pair together when either port is occupied. Current CrossPoint builds compile their firmware-owned `CrossPointWebServer.cpp` and `WebDAVHandler.cpp` against these shims; `CROSSPOINT_SIMULATOR_PROJECT_WEBSERVER` disables only the legacy reduced substitute in this library.
- Build flags: macOS gets architecture-correct SDL compiler and linker flags
  from `pkg-config --cflags --libs sdl3`, so the same sample works on Intel and
  Apple Silicon.
  Linux/WSL additionally links OpenSSL with
  `-lssl -lcrypto -Wno-deprecated-declarations` (OpenSSL 3.x deprecates
  `MD5_*`). See
  [sample-platformio-macos.ini](sample-platformio-macos.ini) and
  [sample-platformio-linux-wsl.ini](sample-platformio-linux-wsl.ini). Keep
  both in sync when build flags change. Native Windows is not supported, WSL
  is.
- Mac App Store packaging: [packaging/macos/Info.plist.in](packaging/macos/Info.plist.in)
  is the single source of truth for the bundle's privacy purpose strings, and
  [packaging/macos/package_macos_app.py](packaging/macos/package_macos_app.py)
  builds, patches, and verifies bundles against it (also exposed as the
  `package_macos_app` PlatformIO target). The `NS*UsageDescription` keys are
  required even though the simulator only calls `SDL_Init(SDL_INIT_VIDEO)`:
  Apple's static scan sees the camera and Bluetooth APIs referenced by the
  linked SDL2 library and rejects the upload with ITMS-90683. Removing them
  breaks the next App Store submission. If a future upload flags another key,
  add it to the template and to `REQUIRED_PRIVACY_KEYS` so `verify` catches it.
- TestFlight deploys: [packaging/macos/deploy.sh](packaging/macos/deploy.sh)
  runs build → bundle → verify → embed dylibs → sign → `productbuild` →
  `altool` → tag, and must run on macOS from a GUI Terminal session.
  `codesign` needs the login keychain, so firing it from a sandboxed agent
  shell fails with `errSecInternalComponent` — route through
  [packaging/macos/deploy.applescript](packaging/macos/deploy.applescript),
  which hands the command to Terminal.app. Build numbers auto-bump from the
  last `macos-build-N` tag because Apple silently rejects a duplicate. A
  sandboxed build cannot spawn `/usr/bin/curl`, so `SimHttpFetch`-backed
  network flows do not work on TestFlight.
- A `.app` launched from Finder starts with its working directory at `/`, so
  [src/HalStorage.cpp](src/HalStorage.cpp) resolves the simulated SD card to
  `~/Library/Application Support/<AppName>/fs_` when the executable sits inside
  a bundle. Command-line dev builds are unaffected and keep using `./fs_`.
- Linker stubs: [src/firmware_link_stubs.cpp](src/firmware_link_stubs.cpp) provides symbols the firmware expects from other translation units (uzlib checksums, HWCDC Serial shim, LUT stubs). When the firmware adds a new global-extern symbol with no simulator counterpart, add its stub here.

## Device profiles and input mapping

[src/BoardConfig.h](src/BoardConfig.h) selects X4 by default,
`SIMULATOR_DEVICE_X3` for X3, and `SIMULATOR_DEVICE_X4_PRO` for X4 Pro. Keep
the reported board capabilities aligned with the firmware SDK. X4 Pro uses the
same 800x480 display geometry as X4 but adds touch, a capacitive Home key,
frontlight state, inversion, and an RTC.

`HalGPIO::update` owns the SDL event pump for the whole simulator, do not poll SDL events elsewhere. If another layer needs to observe events (the iOS harness does), use `SDL_AddEventWatch` — it sees events as they are queued without consuming them, so neither side steals from the other. Scancodes map to button indices `BTN_BACK=0` through `BTN_POWER=6`. `SDL_EVENT_QUIT` sets the `quitRequested` atomic that `HalDisplay::shouldQuit()` reads.

**`SDL_PushEvent` cannot drive `SDL_GetKeyboardState`** — measured, not assumed. A pushed key event reaches the queue, so edge reads (`wasPressed`/`wasReleased`, which `update()` sets straight from the event) work; but SDL's internal keyboard state array is only written on the real-input path, so level reads (`isPressed`, `anyButtonHeld`, `powerHoldDuration`) stay false for injected keys. `powerHoldDuration()` returns 0 at its early exit, so long-press power-off never fires. Anything driving the simulator synthetically must either use the `CROSSPOINT_SIM_INPUT_SCRIPT` path (which writes `syntheticButtonDown[]` directly) or extend `HalGPIO` with a live injection API. See [ios/README.md](ios/README.md).

For repeatable QA, `CROSSPOINT_SIM_INPUT_SCRIPT` schedules synthetic key
and X4 Pro touch edges through the same `HalGPIO` state as real SDL input, and
`CROSSPOINT_SIM_SCREENSHOTS` captures renderer output on the SDL main thread.
Keep synthetic held-time timestamps on the `SDL_GetTicks()` clock used by real
keyboard events; the firmware's `millis()` clock has a different origin. The
deep-sleep loop must also process synthetic input. Process relaunch promotes
the optional `*_AFTER_WAKE` schedules and clears the pre-sleep schedules so
automation cannot enter an infinite sleep/relaunch cycle.

**Navigating to a screen from a headless script.** Work these out by watching
`[ACT] Entering activity:` log lines — that is the reliable way to confirm where
a script actually landed, because a screenshot of the wrong screen looks a lot
like a screenshot of a screen that never changed.

- The app boots into **Home**, not the reader. Under the Lyra Six theme Home
  renders the current book's page, so a startup screenshot looks exactly like
  the reader. Do not read that as "the reader is open."
- **But it does not always boot into Home**, and this is the single biggest
  time-sink in scripting these runs. Depending on the state the previous run
  left behind, a launch either lands on Home or resumes straight into the book
  (`Boot -> Reader -> EpubReader`, all within ~5 ms, before any input). The two
  need opposite openings: from Home the first key must be a `DOWN`, while from
  the reader it must be a `BACK` to get to Home first — and a `BACK` sent while
  already on Home *opens* the most recent book instead, landing you in the wrong
  place with a script that looks correct. Do not write the script blind and
  trust it: run it, grep `[ACT] Entering activity:`, and only believe the
  screenshot once the log shows the activity you meant to reach. Four
  consecutive runs were burned on this on 2026-08-03.
- **Open every script with `2000:HOME`, not `BACK`.** This is the fix for the
  above, found on 2026-08-04 after four more wasted runs. `HOME` reaches Home
  from either starting state and is a no-op when already there, so the rest of
  the script can assume Home. `BACK` cannot: it means "up" from the reader and
  "open the last book" from Home, so with a boot path that alternates run to
  run it lands you somewhere different every other time.
- `rm -rf ./fs_/.crosspoint/` does **not** force the Home path — an earlier
  version of this file claimed it did. Verified 2026-08-04: with the directory
  deleted the sim still went `Boot -> Reader -> EpubReader` within 500 ms. It
  clears caches; it does not decide the boot destination.
- **To force a Home boot, set `readerActivityLoadCount` to 1 in
  `fs_/.crosspoint/state.json`.** Booting into the last book is deliberate —
  `main.cpp`'s comment is "The device IS the current book" — and Home exists only
  as an escape hatch for the two cases that need it: holding BACK during boot, or
  a non-zero `readerActivityLoadCount` (the crash-recovery counter). Setting that
  counter is the only lever a headless script has, since it cannot hold a button
  during boot. Clearing `openEpubPath` or `lastSleepFromReader` does NOT work —
  both are checked, then the branch opens the book anyway unless one of those two
  escape conditions holds.
- `HOME` is **not** handled by the reader. It reaches Home from Home (a no-op)
  and does nothing from `EpubReader`, so it is not the state-independent opener
  an earlier version of this file suggested. Force the boot state with the
  counter above and script from Home; do not try to normalise at runtime.
- On Home, **Back** opens the most recently read book and **Confirm/ENTER**
  activates the selected row. Home lists the recent books followed by the menu
  items (File Browser, Recents, OPDS, File Transfer, Settings), Settings last,
  with no wrap-around — so `DOWN` x15 then `ENTER` reaches Settings regardless
  of how many books are listed.
- Inside the reader **Confirm does nothing** — there is no reader menu. Sending
  `ENTER` while still on Home opens a book, which logs a page render; that is
  the only thing ENTER does on either screen.
- In Settings, `ENTER` on the first row cycles the category tab, so repeated
  `ENTER` + screenshot walks every tab.

```bash
CROSSPOINT_SIM_INPUT_SCRIPT='2000:DOWN;2200:DOWN;…;4800:DOWN;5400:ENTER;6800:ENTER;11500:QUIT' \
CROSSPOINT_SIM_SCREENSHOTS='6300:./qa/tab1.bmp;7700:./qa/tab2.bmp' \
  .pio/build/simulator/program
```

Screenshots are BMP; convert with `sips -s format png in.bmp --out out.png` to
view them.

**Fonts come from two roots.** `SdCardFontRegistry` scans both `fs_/.fonts/`
(hidden) and `fs_/fonts/` (visible) and dedupes by family name, so an installed
set is routinely split across the two. Check both before concluding a family is
missing, and take both when copying a set out. The log line
`SD font system ready (N families discovered)` is the quickest confirmation that
a card layout is well-formed.

## When making changes

- Adding a new HAL method? Mirror the firmware signature exactly and stub it (usually no-op) in the matching `Hal*.cpp/.h`. Do not invent new public methods that don't exist in the firmware HAL.
- Adding a new Arduino/ESP-IDF symbol? Add the minimum stub to the corresponding header in [src/](src/) (e.g. [src/WiFi.h](src/WiFi.h), [src/Arduino.h](src/Arduino.h)). Match the upstream signature, return a sensible default.
- Touching storage or caching code? After the change, `rm -rf ./fs_/.crosspoint/` in the firmware project before re-running, otherwise stale caches built by the old code will mask the fix.
- Touching display, threading, or shutdown? Re-read the "Why the simulator's design has the shape it does" section above first. Several of those decisions undo subtle bugs that will resurface if reverted.
- Adding, removing, or renaming a firmware translation unit? The iOS source set is generated, not hand-written — regenerate it, or the CMake configure step fails with the exact command:
  ```bash
  cd <firmware> && pio run -e simulator -t compiledb
  python3 <simulator>/tools/gen_cmake_sources.py --firmware-dir . --compile-db compile_commands.json
  ```
- Changing anything shared? Build the desktop env first. Green desktop + red iOS means the iOS harness is wrong; both red means the HAL drifted.
