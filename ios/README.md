# CrossPoint X3 on iPhone — iOS harness

A native iOS target for the CrossPoint simulator, scoped to the X3 profile
(`SIMULATOR_DEVICE_X3`), portrait, CMake + SDL3.

## The model

**The iPhone impersonates X3 peripherals; it is not a new CrossPoint board.**

| Layer | Sees | Change |
|---|---|---|
| Harness (`ios/`) | touches on an on-screen button pad | new |
| Device (`HalGPIO`) | the X3's seven GPIO buttons, as SDL scancodes | none |
| Firmware | nothing iOS-specific | none |

The harness translates the first into the second by pushing synthetic
`SDL_EVENT_KEY_DOWN` / `_UP` onto SDL's event queue. There is no
`#if TARGET_OS_IPHONE` in `HalGPIO` or the firmware.

`hasTouch()` stays **false** for X3 — verified in both capability tables:

- simulator `src/BoardConfig.h:74` — `inline bool hasTouch() { return isX4Pro(); }`
- firmware SDK `freeink-sdk/.../BoardConfig.h:987` — table-driven, and the
  `XTEINK_X3` profile passes `NO_TOUCH`

Flipping it would make the firmware take X4-Pro-only paths, i.e. test a device
that does not exist. Hit-testing happens in the harness, above SDL; no coordinate
ever reaches the firmware.

## Status

The real firmware runs. `CROSSPOINT_BUILD_FIRMWARE=ON` compiles the whole source
set — 135 firmware TUs plus 20 simulator TUs — for `arm64-apple-ios`, links it
into the app, and `main()` comes from `src/simulator_main.cpp` exactly as on
desktop.

Verified on an iPhone Air simulator (`iOS 26.5`, native 1260×2736 px):

- **The library and the reader both render**, with dithered covers and the
  firmware's own 4-level greyscale.
- **All seven buttons drive the firmware.** `UP` `DOWN` `LEFT` `RIGHT` `CONFIRM`
  `BACK` `POWER` each log a clean down/up pair; CONFIRM opens a book, a
  horizontal press turns the page, BACK returns to the library.
- **1-bit fidelity is exact.** 0 of 418,176 origin-aligned 2×2 blocks are
  non-uniform, so integer scale with nearest-neighbour sampling is holding. With
  the diagnostic pattern enabled the panel contains exactly two colours.
- **Geometry.** Panel 1056×1584 px at 2×, centred, on a white field that matches
  a blank page so no panel edge is visible.

Not yet run on a physical device — no iPhone Air is paired to this Mac.

## Build and run

```bash
cmake -B build/ios-app -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCROSSPOINT_FIRMWARE_DIR=$HOME/crosspoint/crosspoint-reader \
  -DCROSSPOINT_BUILD_FIRMWARE=ON
cmake --build build/ios-app --config Debug --target CrossPointX3
```

For a device, drop `CMAKE_OSX_SYSROOT` and add `-DCROSSPOINT_IOS_TEAM_ID=<id>`.

The first configure clones and builds SDL3 (`release-3.4.12`), which takes a few
minutes. Reuse an existing checkout with
`-DFETCHCONTENT_SOURCE_DIR_SDL3=<path>`.

Install, seed books, and launch:

```bash
xcrun simctl install <udid> build/ios-app/ios/Debug-iphonesimulator/CrossPointX3.app
cp -R $HOME/crosspoint/crosspoint-reader/fs_ "$(xcrun simctl get_app_container <udid> com.natebunnyfield.crosspoint.x3 data)/Documents/"
xcrun simctl launch <udid> com.natebunnyfield.crosspoint.x3
```

`HalStorage` prefixes every path with `./fs_`, which relies on the process CWD.
On iOS that is the read-only bundle, so the harness `chdir()`s to the app's
Documents directory before `setup()` touches storage. The Info.plist enables file
sharing and in-place document opening, so books can also be sideloaded via Files.

The shipped panel shows only what the firmware draws, which means a presentation
bug has nothing to show itself against. Add
`-DCROSSPOINT_HARNESS_TEST_PATTERN=ON` for a diagnostic pattern — 1 px gratings,
a Bayer 4×4 ramp, and per-corner glyphs that identify rotation. It is a build
flag, not on-screen UI.

## Controls

An on-screen pad with one control per physical X3 button — no more, no fewer.
There are no gestures: every control is down-on-touch and up-on-lift, so it
carries a real hold, which is what page-turn autorepeat and long-press power-off
need and what a tap or swipe cannot express. Dragging off a control cancels it.

Two bottom-aligned rows in a five-wide grid; blank slots stay empty.

```
Left      —      Power      —      Right
Back    Select     —       Up      Down
```

| Control | Scancode | Glyph |
|---|---|---|
| Back | `ESCAPE` | chevron against a bar |
| Select | `RETURN` | checkmark |
| Up / Down / Left / Right | `UP` `DOWN` `LEFT` `RIGHT` | chevrons |
| Power | `P` | IEC power mark |

Scancodes mirror `HalGPIO.cpp`'s `buttonScancode[]` exactly. There is no HOME —
`hasHomeKey()` is X4-Pro-only. There is no control for the simulator's own SLEEP
(`S`) either: that is a harness command, not a button the hardware has.

Back draws a chevron against a bar rather than a bare chevron so it cannot be
mistaken for Left.

**Placement is specified, not derived.** Nothing in this source tree encodes
where the buttons physically sit on the X3 chassis; the SDK describes them only
electrically (six on a resistor ladder across two ADC pins, POWER on its own
digital pin — `BoardConfig` `InputPins`, `InputStyle::XteinkAdcLadder`).

Sizing follows Apple's Human Interface Guidelines: targets are 69.6 × 46 pt
against the 44 × 44 pt minimum, separated by 8 pt, with the lower row on the
bottom of the safe area rather than the physical edge — below that line is the
home indicator, and a control there would fight the system's own swipe. Layout is
computed in points and converted once, so targets keep their real physical size
at any device scale. The panel edge the rows sit under is derived from the same
integer-scale rule the renderer uses, so the two cannot drift apart.

## How the harness attaches

Two seams, both in simulator code — the firmware and `HalGPIO` are untouched.

**Input: an event watch, not a poll loop.** `HalGPIO::update()` owns the SDL
event pump for the whole simulator and must keep owning it; two pollers would
split events between them. `SDL_AddEventWatch` observes events as they are queued
without consuming them, so the harness sees finger events that `HalGPIO` ignores
and neither steals from the other.

**Output: `SimulatorOverlay`.** The pad is painted by a free hook the
presentation path calls (`src/SimulatorOverlay.h`), deliberately not a
`HalDisplay` method — the HAL's public surface must mirror the firmware's, and an
on-screen pad has no analog on real hardware. The callback runs with logical
presentation disabled and receives the real output size, so it can paint the
letterboxed margins that the panel's logical coordinate space cannot reach. It
also exposes `requestPresent()`, because an e-ink firmware presents rarely and a
pressed state would otherwise not appear until the next page render.

`simulator_main.cpp` holds the only iOS-specific lines in the simulator: the
`chdir()`, the harness install, and a normal `return 0` in place of `_exit(0)`
(iOS reports a self-terminating process as a crash).

## Open issue: level reads do not see injected keys

Measured, not assumed:

```
SDL_PushEvent rc=1
AFTER PUSH+PUMP: GetKeyboardState[P]=0
event dequeued from queue=1
AFTER POLL:      GetKeyboardState[P]=0
```

`SDL_PushEvent` delivers an event to the queue but does not update SDL's internal
keyboard state array — that array is only written on the real-input path.
Consequences for `HalGPIO`:

- **Edge reads work.** `update()` sets `pressedThisFrame` / `releasedThisFrame`
  straight from the event (`HalGPIO.cpp:503-516`), so `wasPressed()` /
  `wasReleased()` behave. Everything demonstrated above runs on this path.
- **Level reads do not.** `isPressed()` (`:552`), `anyButtonHeld()` (`:588`) and
  `powerHoldDuration()` (`:601`) consult
  `SDL_GetKeyboardState(NULL) || syntheticButtonDown[]`. Injected keys set
  neither, so `powerHoldDuration()` returns 0 at its early exit and
  **long-press power-off never fires**. Autorepeat/held page-turn breaks the same
  way.

The pad now expresses a genuine hold, so the input side is no longer the
limitation — the firmware simply cannot read it. Any fix touches the simulator's
`HalGPIO`; the firmware still does not change and no `#ifdef` is needed:

1. **Promote a live injection API** — a platform-neutral
   `HalGPIO::injectButtonDown/Up(uint8_t)` setting the same three arrays the
   existing synthetic path sets (`processSyntheticEvents`, `HalGPIO.cpp:369-409`,
   which is file-local and driven only by the `CROSSPOINT_SIM_INPUT_SCRIPT` env
   var — a startup batch, not a live API), with that script refactored to call it
   so there is one code path.
2. **Make the keyboard path level-consistent** — have `update()` also set
   `syntheticButtonDown[]` on KEYDOWN/KEYUP. Keeps the translation point at SDL,
   but needs focus-loss handling so a real held key cannot stick.

Option 1 is the recommendation. **This is an open decision.**

## Resolved along the way

- **Both `run_simulator.py` build-time patches are dead.** Its docstring
  describes two patches CMake would need to replicate; the implementing code no
  longer exists in the 63-line script. `BookMetadataCache` `size_t` → `uint32_t`
  is already a real source edit upstream
  (`lib/Epub/Epub/BookMetadataCache.h:23`), so there is no cache-corruption risk
  on arm64. The `GfxRenderer::setOrientation` notify is superseded by polling:
  `setOrientation` is a bare setter (`GfxRenderer.h:150`) and
  `presentIfNeeded` re-reads `getOrientation()` every present
  (`HalDisplay.cpp:395-396`). The stale docstring is worth deleting.
- **SDL2 → SDL3 is done, on both toolchains.** Four files touched SDL
  (`HalDisplay.cpp/.h`, `HalGPIO.cpp`, `simulator_main.cpp`). The desktop
  PlatformIO env moved to SDL3 at the same time (`!pkg-config --cflags --libs
  sdl3`), so there is one source set and no per-platform SDL shim. The only
  non-mechanical change was `SDL_GetKeyboardState` returning `const bool*`
  instead of `const Uint8*`.
- **Presentation policy is keyed on intent, not platform.**
  `CROSSPOINT_SIM_PIXEL_EXACT` selects `INTEGER_SCALE` + `SCALEMODE_NEAREST`;
  without it the desktop keeps letterbox + linear filtering, which is right at
  1:1 because Bayer dither averaging to grey is what e-ink actually looks like.
  A desktop build can ask for exact pixels too.

## Still deferred

- **Tilt.** `HalTiltSensor::begin()` sets `_available = true` for
  `SIMULATOR_DEVICE_X3` while every predicate hard-returns false and there is no
  injection hook. **Leave it that way** — the real X3 does carry the sensor
  (`ImuType::Qmi8658` in the X3 board profile), and forcing it false would hide a
  capability the hardware exposes. Known, accepted dead zone.
- **Physical device.** Signing identities exist; no iPhone Air is paired.

## Source-set translation

`cmake/CrossPointSources.cmake` is **generated**, never hand-edited — 135
firmware TUs, 20 simulator TUs, 24 include dirs, 14 defines, derived from the
PlatformIO env that already works:

```bash
cd $HOME/crosspoint/crosspoint-reader
pio run -e simulator -t compiledb
python3 <simulator>/tools/gen_cmake_sources.py --firmware-dir . --compile-db compile_commands.json
```

The root `CMakeLists.txt` validates every listed path at configure time and fails
loudly with the regeneration command if any has moved. On a fork the firmware
moves under you continuously, so a hand-maintained list would rot silently.

Notable: the `simulator` env compiles **zero third-party TUs**. ArduinoJson is
header-only, and QRCode/WebSockets are shimmed by the simulator itself, so SDL is
the only real external dependency.

## Keeping the desktop build green

The desktop build is the canary: green desktop + red iOS means the harness is
wrong; both red means the HAL drifted. Build desktop first whenever iOS fails.

```bash
cd $HOME/crosspoint/crosspoint-reader && pio run -e simulator
```

For that to carry signal it must compile *this* working copy, so the firmware's
`[env:simulator]` `lib_deps` uses
`simulator=symlink:///Users/natebunnyfield/src/crosspoint-simulator` rather than
the upstream git URL. Verified live by appending `#error` to `HalGPIO.cpp` and
confirming the build failed with it.
