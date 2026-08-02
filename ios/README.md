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

The harness translates the first into the second by calling
`gpio.injectButtonDown/Up()` directly. Pushing synthetic `SDL_EVENT_KEY_*` was the
original design and was abandoned: `SDL_PushEvent` does not update
`SDL_GetKeyboardState`, so level reads (`isPressed`, `powerHoldDuration`) stayed
false and long-press power-off never fired. See the injection section below.
There is no `#if TARGET_OS_IPHONE` in `HalGPIO` or the firmware.

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
  -DCROSSPOINT_FIRMWARE_DIR=$HOME/src/crosspoint-reader \
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
cp -R $HOME/src/crosspoint-reader/fs_/books "$(xcrun simctl get_app_container <udid> com.natebunnyfield.crosspoint.x3 data)/Documents/"
xcrun simctl launch <udid> com.natebunnyfield.crosspoint.x3
```

**The app's Documents directory is the emulated SD card.** The harness
`chdir()`s there (the iOS default CWD is the read-only bundle), points
`CROSSPOINT_SIM_SD` at it, and creates `books/` on first launch. Because the
Info.plist enables file sharing and in-place document opening, the card is
browsable in the Files app as **On My iPhone → CrossPoint X3**, and dropping an
EPUB into its `books` folder — from Files, iCloud Drive, or AirDrop — is how
books get onto the phone. Firmware state lives in `.crosspoint`, dot-prefixed,
which both `HalStorage` iteration and Files keep hidden. Installs that predate
this layout (which kept the card at `Documents/fs_`) are migrated on launch:
`fs_/books` and `fs_/.crosspoint` are renamed up a level, so libraries and
reading positions survive the update.

**Fonts sideload the same way, through a visible `fonts` folder.** The firmware
scans two SD roots and merges them — `/.fonts` (hidden, preferred) and `/fonts`
(visible; `SdCardFontRegistry.h:33-34`) — but Files refuses to create or show
dot-prefixed names, so on the phone only the visible root is reachable. The
harness creates `fonts/` eagerly next to `books/`; drop a font family folder of
`.cpfont` files into it (see the firmware's `docs/sd-card-fonts.md`) and the
firmware picks it up at the next boot. Families the firmware itself downloaded
into `.fonts` earlier are moved up into `fonts/` on launch (per-family rename,
never overwriting), which also steers the firmware's future downloads to the
visible root — `defaultWriteRoot()` only prefers the hidden root while it
exists. This lives in `CrossPointFsPrep.cpp`, split from the shim so the whole
filesystem-prep path can be compiled and exercised on a desktop host.

The shipped panel shows only what the firmware draws, which means a presentation
bug has nothing to show itself against. Add
`-DCROSSPOINT_HARNESS_TEST_PATTERN=ON` for a diagnostic pattern — 1 px gratings,
a Bayer 4×4 ramp, and per-corner glyphs that identify rotation. It is a build
flag, not on-screen UI.

## Deploying without touching the Mac

Signing needs the login keychain, which only a GUI Terminal session has — an
SSH shell fails at codesign with `errSecInternalComponent`. The bridge, same as
crds-ios: let AppleScript hand the command to Terminal.app, which runs it under
the logged-in user with the keychain unlocked. **The usual trigger is a Claude
session on the Mac** (crds-ios `DEPLOY_BUGS.md` D.12: "agents CAN deploy this
way") — paste [DISPATCH.md](DISPATCH.md) into one and it ships a build
end-to-end. A phone SSH client works identically:

From any phone SSH client (Terminus, Blink):

```bash
ssh <your-mac> 'osascript ~/src/crosspoint-simulator/ios/deploy.applescript'
```

That opens a Terminal tab on the Mac running
[deploy-from-repo.sh](deploy-from-repo.sh), which pulls the current branch
(`--ff-only`) and runs [testflight.sh](testflight.sh) — so a phone-fired deploy
ships what was just merged, and the result lands back on the phone as the
script's ntfy notification. KEY=VALUE arguments pass through `env` into the
Terminal subshell:

```bash
ssh <your-mac> 'osascript ~/src/crosspoint-simulator/ios/deploy.applescript "CROSSPOINT_MARKETING_VERSION=0.1.1"'
```

One-tap version, as an iOS Shortcut (crds-ios `SHORTCUTS_RECIPES.md` pattern):

| step | action | parameters |
|---|---|---|
| 1 | Run Script Over SSH | `osascript ~/src/crosspoint-simulator/ios/deploy.applescript` |
| 2 | Show Notification | "Deploy started — watch ntfy" |

Pin it to the home screen as **"X3 Deploy"**. One-time Mac setup: allow the
SSH-launched `osascript` to control Terminal (Privacy & Security → Automation —
the prompt appears on the Mac's screen the first time, so approve it while
you're at the machine), and the Mac must be logged in and unlocked when you
fire — a locked screen keeps the keychain shut.

## Controls

An on-screen pad with one control per physical X3 button — no more, no fewer.
PURE PASSTHROUGH: every control is down-on-touch and up-on-lift and nothing
else, so it carries a real hold — page-turn autorepeat and hold-to-sleep both
work exactly as on hardware. Dragging off a control cancels it. The
finger→button decisions live in `ios/PadCore.{h,cpp}`, a pure, SDL-free,
clock-free unit (`tests/pad_core_test.cpp`); the API cannot express time, so
time-based gesture invention (an earlier POWER tap-stretch held the injected
button 600 ms past the finger and read as a stuck control) cannot return
without changing that header.

Two rows of 60 pt squares anchored directly under the panel's bottom edge
(`SimulatorOverlay::panelBottomPx`), clamped clear of the home indicator:

```
[Up]          [Power]          [Down]      <- side pair + power
[Back|Select]          [Left|Right]        <- front buttons, two fused rockers
```

UP/DOWN are the X3's SIDE buttons (fixed page-turn pair); BACK/SELECT and
LEFT/RIGHT are the FRONT buttons, and each front pair paints as one capsule —
rounded outer corners only, a hairline divider, no pinched notch between two
rounded squares.

| Control | Button index |
|---|---|
| Back | `HalGPIO::BTN_BACK` |
| Select | `HalGPIO::BTN_CONFIRM` |
| Up / Down / Left / Right | `BTN_UP` `BTN_DOWN` `BTN_LEFT` `BTN_RIGHT` |
| Power | `HalGPIO::BTN_POWER` |

The controls are deliberately **unlabelled** — no glyph, no text. An earlier
revision of this table listed one per control; `paintPad` draws none.

Each control names a `HalGPIO::BTN_*` index directly and drives it through
`gpio.injectButtonDown/Up()`. Scancodes are not involved: they were the transport
while the pad injected by pushing SDL key events, and that indirection is what
broke level reads (see below). There is no HOME — `hasHomeKey()` is X4-Pro-only.
There is no control for the simulator's own SLEEP (`S`) either: that is a harness
command, not a button the hardware has.


**Placement is specified, not derived.** Nothing in this source tree encodes
where the buttons physically sit on the X3 chassis; the SDK describes them only
electrically (six on a resistor ladder across two ADC pins, POWER on its own
digital pin — `BoardConfig` `InputPins`, `InputStyle::XteinkAdcLadder`).

Sizing: 60 pt squares (owner-picked, vs the 44 pt HIG minimum), 16 pt between
rows, fused pairs touching. Layout is computed in points and converted once, so
targets keep their real physical size at any device scale. The panel is
top-aligned in the space above a fixed reserved bottom band
(`SimulatorOverlay::setBottomInset`), presents at an integer scale, and
publishes its bottom edge; the pad anchors to that edge with a fallback to
bottom-anchoring before the first present.

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

## Closed: level reads do not see injected keys

**Resolved.** Option 1 below was taken. The pad no longer pushes SDL key events;
it calls `gpio.injectButtonDown/Up(HalGPIO::BTN_*)`.

The bug, measured not assumed:

```
SDL_PushEvent rc=1
AFTER PUSH+PUMP: GetKeyboardState[P]=0
event dequeued from queue=1
AFTER POLL:      GetKeyboardState[P]=0
```

`SDL_PushEvent` delivers an event to the queue but does not update SDL's internal
keyboard state array — that array is only written on the real-input path.
Consequences for `HalGPIO`, when the pad injected by pushing key events:

- **Edge reads worked.** `update()` sets `pressedThisFrame` /
  `releasedThisFrame` straight from the dequeued event, so `wasPressed()` /
  `wasReleased()` behaved.
- **Level reads did not.** `isPressed()`, `getHeldTime()` and
  `getPowerButtonHeldTime()` consult
  `SDL_GetKeyboardState(NULL) || syntheticButtonDown[]`. Pushed events set
  neither, so every held-button gesture silently never fired. Two the user hit:
  - **Hold POWER to sleep.** `main.cpp:573-574` needs
    `gpio.isPressed(BTN_POWER) && gpio.getPowerButtonHeldTime() > 400`; the
    second call returned 0 at its early exit, so the device could only ever
    sleep on the inactivity timeout.
  - **Hold a side button to cycle font family.**
    `EpubReaderActivity.cpp:648-665` needs
    `ReaderUtils::detectHeldSideDirection()` (which is `isPressed()`) and
    `getHeldTime() >= 700`; both were dead, so the branch was unreachable. The
    font-SIZE tap in the same block survived, because it reads a release edge.

The fix, in `HalGPIO`; the firmware does not change and no `#ifdef` is needed:

1. **A live injection API** — platform-neutral
   `HalGPIO::injectButtonDown/Up(uint8_t)`, writing the press edge, the held
   level and the `SDL_GetTicks()` press timestamp together.
   `processSyntheticEvents()` (the `CROSSPOINT_SIM_INPUT_SCRIPT` batch) and the
   `S` sleep shortcut were refactored onto it, so there is one code path and the
   script env var is now a second caller of the same API rather than a parallel
   implementation.
2. *(not taken)* **Make the keyboard path level-consistent** — have `update()`
   also set `syntheticButtonDown[]` on KEYDOWN/KEYUP. Keeps the translation point
   at SDL, but needs focus-loss handling so a real held key cannot stick.

Verified on the desktop binary, which runs the identical `injectButtonDown/Up`
code the pad now calls, driven through `CROSSPOINT_SIM_INPUT_SCRIPT`:

- `4000:P:2000` → `[4430] [MAIN] Entering deep sleep`, i.e. 430 ms after the
  injected press, matching `getPowerButtonDuration()` = 400 ms. A later
  `9000:P:300` woke it: the relaunched process logged
  `Verifying power button press duration`, which is reached only when
  `getWakeupReason() == PowerButton`.
- `6000:DOWN:1400` in the reader → `[6743] Loaded /.fonts/Edgar/Edgar_12.cpfont`
  and `sdFontFamilyName` `Coelacanth` → `Edgar`, i.e. the family cycled 743 ms
  into the hold (`SKIP_HOLD_MS` = 700).
- `6000:DOWN:150` (a tap) stepped `fontSize` 0 → 1 and left the family alone, so
  the edge path is not regressed and hold/tap still disambiguate.

### Closed: the wake path no longer execs on iOS

This section previously described `rebootAsPowerWake()`'s `execvp()` as an open
problem on iOS. It was closed by `10a8c5a` ("fix(ios): wake from deep sleep
without exec"): `SimulatorLifecycle.h` defines `CROSSPOINT_SIM_REBOOT_IN_PROCESS`
for `__APPLE__ && TARGET_OS_IPHONE`, and `SimulatorLifecycle.cpp` takes a
`std::longjmp` back to the armed jump buffer before the `execvp()` line can be
reached. The exec is still compiled into the iOS binary but is unreachable there.

Leaving the stale text in place cost a later investigation real time — it is the
first suspect anyone reads. The actual iOS wake bug was downstream of the jump:
the firmware's inactivity clock was a `loop()`-local static, so it survived the
longjmp holding its pre-sleep value and the first post-wake `loop()` immediately
auto-slept again. Fixed in the firmware by hoisting it to file scope and
resetting it in `setup()`, next to the same reset for `deepSleepInProgress`.

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
cd $HOME/src/crosspoint-reader
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
cd $HOME/src/crosspoint-reader && pio run -e simulator
```

For that to carry signal it must compile *this* working copy, so the firmware's
`[env:simulator]` `lib_deps` uses
`simulator=symlink:///Users/natebunnyfield/src/crosspoint-simulator` rather than
the upstream git URL. Verified live by appending `#error` to `HalGPIO.cpp` and
confirming the build failed with it.
