# CROSSPOINT X3 → iPhone Air PORT — SESSION CONTEXT

**Status:** IMPLEMENTED. The firmware runs on iOS. This file is kept as the historical planning
record — for the current state of the port, read [ios/README.md](ios/README.md), which supersedes
§5 and §7 below.
**Purpose:** cold-start briefing for the session that implemented this port. §1–§4 are still
accurate; the decisions they describe all held. See §7 for what the open questions resolved to.

**This is not a crds project.** It lives on this branch only because the branch was provisioned
here. The port itself belongs in a fresh repo (see §6).

---

## §0 — BOOTSTRAP PROMPT (paste this)

```
FIRST: read CROSSPOINT_X3_IOS_PORT_CONTEXT.md in full. It contains verified source facts with
file cites — do not re-derive them.

Task: port the CrossPoint simulator to run as a native iOS app on an iPhone Air, scoped to my
custom X3 firmware fork.

The architecture is already decided (§3): the iPhone is a peripheral impersonator, not a new
CrossPoint board. hasTouch() stays false for X3. iPhone gestures and on-screen buttons are
translated into synthetic SDL keyboard scancodes and pushed onto SDL's event queue, so every
line of input handling below SDL is the same code the desktop simulator already exercises.

Descoped, do not implement: tilt/shake (CoreMotion), and the firmware's capacitive touchscreen
path. See §1 for why, and §4.3 for the dead-code trap tilt leaves behind.

First milestone only (§5): CMake source-set translation plus a stub ios/ shim that builds,
launches, and presents a static framebuffer with the button overlay wired to synthetic
scancodes. Prove the toolchain and the injection model before touching any HAL.

Keep the desktop build green throughout — it is the canary that tells "my HAL drifted" apart
from "my iOS shim is wrong" (§4.2).
```

---

## §1 — SCOPE (locked)

| Item | Decision |
|---|---|
| Device profile | X3 only — `-DSIMULATOR_DEVICE_X3` |
| Target | iPhone Air (a.k.a. iPhone 17 Air), portrait |
| Firmware | user's own X3 fork, not upstream `crosspoint-reader` |
| Input | harness-recognized gestures + on-screen buttons → synthetic scancodes |
| Touchscreen path | **descoped** — `hasTouch()` stays false (§3) |
| Tilt / shake-to-turn | **descoped** — no CoreMotion (§4.3 for the trap this leaves) |
| Approach | native iOS app, CMake + SDL3. Not WASM, not remote-streaming. |

**Honest note on justification.** Tilt was the strongest single argument for building this at all
on X3 — it is the one capability the desktop simulator structurally cannot provide (§4.3). With
it descoped, the remaining case is reading in hand, at true physical scale, away from the desk.
That is real for a pocket e-reader, but it is softer, and a Retina Mac configured for a 1:1
backing store already shows the panel at ~117% life-size with perfect 1-bit pixels. The port
earns its keep on context of use, not pixel density. Re-scoping is the user's call.

---

## §2 — VERIFIED SOURCE FACTS (do not re-derive)

Read from `crosspoint-reader/crosspoint-simulator@main`.

**It is not an emulator.** `platform = native`, `-std=gnu++2a`. No ESP-IDF, no Xtensa/RISC-V
emulation, no JIT anywhere — so no App Store dynamic-code problem. It is a portable POSIX/C++
app that happens to compile ESP32-C3 firmware.

| Concern | Fact |
|---|---|
| Threading | FreeRTOS shimmed onto `std::thread`; `SemaphoreHandle_t` → `std::recursive_mutex`; task notifications → condvars + counters; per-thread handle via TLS |
| SDL | main thread only — already a hard rule because of macOS, which is also the iOS rule. Render task writes a 1-bit framebuffer + sets an atomic flag; main thread's `presentIfNeeded` uploads and presents |
| Rotation | firmware rotates content 90° CCW into the landscape buffer; simulator undoes it via `SDL_RenderCopyEx` |
| Storage | **POSIX fds, not `fstream`** (deliberate). All paths sandboxed under `./fs_/`; `/books/` → `./fs_/books/` |
| Timing | `steady_clock`, so wall-clock changes don't disrupt firmware timing |
| Networking | `NetworkClient.cpp` is ~120 lines of raw BSD sockets (`getaddrinfo`/`socket`/`connect`/`send`), behind a private `Impl` — compiles on iOS unchanged. **No curl in it.** |
| curl | declared a required package, used somewhere outside `NetworkClient` (OTA/WebDAV, likely). Not traced. May drop out entirely — see §4.4 |
| MD5 | platform-selected; OpenSSL on Linux only, so macOS already uses a non-OpenSSL path (CommonCrypto) that iOS inherits |
| Webserver | shims on ports 8080/8081 |
| Exit | `simulator_main.cpp` calls `_exit(0)` to dodge a global-dtor/render-thread race |
| Python | **build-time only.** `run_simulator.py` patches two files and registers a target; the binary needs no Python at runtime |

### X3 capabilities — `BoardConfig.h`

```
inline bool hasTouch()        { return isX4Pro(); }   // ← FALSE for X3
inline bool hasHomeKey()      { ... X4_PRO only }     // ← X3 has no home key
inline bool hasPwmFrontlight(){ ... X4_PRO only }
```

Device selection is compile-time. `SIMULATOR_DEVICE_X3` / `SIMULATOR_DEVICE_X4_PRO`; default X4.

### Input — `HalGPIO.cpp`

`update()` owns all SDL event polling (prevents double-consumption). `beginFrame()` clears the
per-frame pressed/released edge latches. Consumes `SDL_KEYDOWN`/`SDL_KEYUP`, mouse (left button
only), `SDL_QUIT`. Mouse→touch tap/swipe recognition (28 px slop, 60 px min swipe, 700 ms max)
exists but is gated behind `hasTouch()` — **dead on X3.**

Scancode map, all devices:

| Button | Key |
|---|---|
| `BTN_BACK` (0) | `ESCAPE` |
| `BTN_CONFIRM` (1) | `RETURN` |
| `BTN_LEFT` (2) | `LEFT` |
| `BTN_RIGHT` (3) | `RIGHT` |
| `BTN_UP` (4) | `UP` |
| `BTN_DOWN` (5) | `DOWN` |
| `BTN_POWER` (6) | `P` |
| *(simulator sleep)* | `S` |
| *(home — X4 Pro only)* | `H` |

### Geometry

X3 panel: 792×528 landscape framebuffer, presented portrait as **528×792**. ~257 ppi (reported
250–259 for the 3.7″ panel), so the real screen is **2.05″ × 3.08″**.

iPhone Air: 6.5″, 2736×1260 px, 460 ppi, 3× (912×420 pt landscape).

**Portrait, 2× nearest-neighbor → 1056×1584 px = 2.30″ × 3.44″ ≈ 112% of life-size.** Leaves
204 px of side margin and 1152 px (2.5″) below for the control zones.

Integer scale + nearest-neighbor is non-negotiable on a 1-bit panel. Any fractional or linear
filter grays the dither and you will misjudge every rendering change you make.

### Build config — `sample-platformio-macos.ini`

- `platform = native`, `lib_ldf_mode = deep+`, `-std=gnu++2a`
- `build_src_filter` **excludes**: `network/FirmwareFlasher.cpp`, `network/OtaBootSwitch.cpp`,
  `network/OtaUpdater.cpp`, `platform/skip_efuse_blk_check.c`
- Defines: `SIMULATOR`, `CROSSPOINT_SIMULATOR_PROJECT_WEBSERVER`,
  `CROSSPOINT_VERSION="dev-simulator"`, `ENABLE_SERIAL_LOG`, `LOG_LEVEL=2`,
  `EINK_DISPLAY_SINGLE_BUFFER_MODE=1`, `MINIZ_NO_ZLIB_COMPATIBLE_NAMES`, `XML_GE=0`,
  `PNG_MAX_BUFFERED_PIXELS=16416`, + `SIMULATOR_DEVICE_X3`
- `lib_deps`: simulator, FreeInkUI, Icons, ArduinoJson 7.4.2, QRCode ^0.0.1, WebSockets 2.7.3
- `lib_ignore`: `hal`, `PNGdec`, `JPEGDEC`

---

## §3 — THE MODEL

**The iPhone is a peripheral impersonator, not a new CrossPoint board.**

Two surfaces, one translation point:

- **Harness layer** (UIKit/SDL) — recognizes swipes and on-screen button taps. Lives *outside*
  the simulated device; has no analog on real hardware.
- **Device layer** (`HalGPIO`) — sees only the X3's seven GPIO buttons, via scancodes.

The harness translates the first into the second by pushing synthetic `SDL_KEYDOWN`/`SDL_KEYUP`
onto SDL's event queue. **That injection point is the whole trick:** `HalGPIO::update()` consumes
synthetic keys identically to real ones, so every line of input handling below SDL stays the code
already exercised on desktop. No `#if TARGET_OS_IPHONE` in the firmware or the HAL.

### The discriminator: do NOT flip `hasTouch()`

The tempting shortcut is `hasTouch() → true` so the touchscreen "just works." Don't. The custom
X3 firmware would start taking X4-Pro-only code paths and you would be testing a device that
does not exist. Fidelity to the target board is the only reason a simulator is worth running.

**iPhone touches become button events, never touch events.** Use SDL touch to drive the overlay
only; never let a coordinate reach the `hasTouch()` branch.

### Mapping

`H` is dropped entirely — `hasHomeKey()` is X4-Pro-only, X3 has no home key.

- swipe left / right → `LEFT` / `RIGHT`
- swipe up / down → `UP` / `DOWN`
- two-finger tap → `BACK`
- `CONFIRM` and `POWER` as persistent on-screen buttons — `POWER` especially, since
  long-press-to-power-off needs a real held duration a swipe cannot express

### Two visually distinct control zones

Device buttons (what the X3 physically has) in one row; harness controls (sleep, reset,
screenshot) in another. `S` is you commanding the simulator, not a button the X3 has. Same
fidelity logic as `hasTouch()`: you should never be unsure whether you just pressed something
the hardware actually has.

### Layers

| Layer | Change |
|---|---|
| Firmware (user's X3 fork) | untouched |
| Simulator HAL | extended, never diverged |
| New `ios/` shim beneath SDL | gesture + button recognition → `SDL_PushEvent` |

---

## §4 — GOTCHAS

### 4.1 Edge latches and hold duration

`HalGPIO::beginFrame()` clears the per-frame pressed/released latches. Push `KEYDOWN` and `KEYUP`
inside one frame and the firmware sees press and release simultaneously — held-button semantics
break (page-turn autorepeat, long-press power-off). Model gestures as a discrete press held
**≥ 2 loop iterations**; model on-screen buttons as genuine down-on-touch / up-on-lift.

### 4.2 The two Python patches must become real source edits

`run_simulator.py` applies two patches at build time. CMake will not:

1. `BookMetadataCache` — `size_t` → `uint32_t`. **Still required:** the patch exists because the
   firmware assumes 32-bit and arm64 iOS is 64-bit exactly like the desktop host. Missing it
   fails as *silent cache corruption*, not a build error.
2. `GfxRenderer::setOrientation` — notify `HalDisplay` on orientation change.

**Topology.** The simulator's stated failure mode is the HAL stub rule: every HAL method the
firmware adds needs a matching stub or the build breaks. On a fork, *you* break it continuously.
So: CMake over one source set, two toolchains (desktop + iOS); firmware as sibling checkout or
submodule; add an `ios/` dir + `CMakeLists.txt` in an upstream-mergeable shape. **Never fork the
simulator.** Keep desktop green as the canary — green desktop + red iOS means the shim is wrong;
both red means the HAL drifted.

### 4.3 The tilt descope leaves a live trap — write it down

`HalTiltSensor::begin()` sets `_available = true` **specifically for `SIMULATOR_DEVICE_X3`**,
while `wasTiltedForward()`, `wasTiltedBack()`, and `hadActivity()` all hard-return `false`,
`update()` is a no-op, and there is **no injection hook**.

So the firmware believes the sensor is present and never receives an event. If the custom X3 build
has a shake-to-turn setting, it renders as available and silently does nothing.

**Leave `_available = true`** — the real X3 does have the sensor, and forcing it false would hide
a capability the hardware exposes. This is a known, accepted dead zone, not a bug to chase in
three months. Cheap to un-descope later: CoreMotion (or `SDL_SENSOR_ACCEL`) into three
predicates, nothing structural.

### 4.4 Everything else

- **Filesystem.** `./fs_/` relies on CWD, meaningless on iOS. `chdir()` to `NSDocumentDirectory`
  at launch is likely sufficient given relative POSIX paths. Enable file sharing / document
  browser to sideload EPUB/TXT via Files — convenient, since the X3 has no USB port (pogo pins).
- **iOS steals your edges.** Swipe-up-from-bottom is the app switcher; swipe-down-from-top-right
  is Control Center. Avoid edge-originating swipes; if the bottom edge is needed, defer with
  `preferredScreenEdgesDeferringSystemGestures`.
- **`_exit(0)` must go.** On iOS that reads as a crash. Needs SDL's iOS `main` shim (SDL renames
  `main` and calls it from `UIApplicationMain`).
- **Dependencies.** OTA is already excluded from the build and `NetworkClient` is raw BSD
  sockets, so curl may drop out entirely — confirm at link time rather than assuming. Plan for
  zero third-party deps beyond SDL. mbedtls is vendored in `src/mbedtls`.
- **Webserver** runs on-device and is reachable over LAN, but only while foregrounded — iOS
  suspends background sockets.
- **Signing.** Free personal team works, expires every 7 days, 3-app limit. $99/yr → one year +
  TestFlight. AltStore/SideStore automates the weekly re-sign.

---

## §5 — FIRST MILESTONE

CMake source-set translation + a stub `ios/` shim that builds, launches, and presents a **static**
framebuffer with the button overlay wired to synthetic scancodes.

Proves the toolchain and the injection model before any HAL work, and is the piece most likely to
surface surprises. Do not start HAL work until this runs on device.

Then, in order: real framebuffer presentation → `chdir()` + sideloaded book → gesture recognizer
→ long-press/autorepeat verification.

---

## §6 — SESSION SETUP

**Repos.** The implementing session needs the X3 firmware fork **and** a `crosspoint-simulator`
fork, both under the user's own account — cross-owner adds are refused, so a session rooted at
`natebunnyfield/crds-ios` cannot reach `crosspoint-reader/*`. Root the new session at the
firmware fork.

**Recommended driver: Claude Opus 5 at `xhigh` effort.** 1M context, $5/$25 per MTok. The 1M
window is load-bearing here — firmware `src/` + simulator `src/` (60+ headers) + five Arduino
libs does not fit in 200K, which rules Haiku 4.5 out as a driver regardless of cost. `xhigh` is
the recommended setting for coding and agentic work and is the Claude Code default.

Worth delegating: **Claude Sonnet 5** ($3/$15 list; **$2/$10 introductory through 2026-08-31**,
also 1M context) for the CMake translation grind and HAL stub filling — high-volume, mechanical,
near-Opus quality on coding. **Haiku 4.5** ($1/$5, 200K) for single-file stub mirroring.

Keep Opus on the architecture, the input-injection layer, and debugging the silent-failure class
in §4.2 — those are where judgment pays for itself. Fable 5 ($10/$50) is not warranted; this is
mechanical translation plus careful design, not frontier reasoning.

Two behavioral notes that matter for a long grind on Opus 5: it verifies its own work unprompted
(so do **not** add "double-check your work" instructions or a separate verification pass — that
causes over-verification), and it reaches for subagents readily, so cap delegation explicitly if
cost matters.

---

## §7 — OPEN QUESTIONS (all resolved)

1. **Does the custom X3 fork add HAL methods** beyond upstream? **No stub work was needed.** The
   fork's only HAL change vs the merge-base is `HalClock`, and its one new public method
   (`getDateTime`) was already mirrored at `src/HalClock.h:19`. Diffing upstream `develop`'s HAL
   against the simulator also showed zero public gaps, so a rebase costs nothing either. The
   remaining differences are private device-side helpers (`writeDateTimeToRTC`, `readGyro`,
   `readReg`, `writeReg`) the simulator never needs.
2. **Does the fork change `BoardConfig`** — e.g. already flipped `hasTouch()`? **No.** Both
   capability tables report false for X3: the simulator's `isX4Pro()` form at
   `src/BoardConfig.h:74`, and the firmware SDK's independent table-driven form at
   `BoardConfig.h:987`, where the `XTEINK_X3` profile passes `NO_TOUCH`. §3's discriminator stands.
3. **Where does curl actually get used?** **It dropped out entirely, as hoped.** The `simulator`
   env compiles zero third-party TUs — ArduinoJson is header-only, QRCode and WebSockets are
   shimmed by this library — so SDL is the only external dependency. No `curl.xcframework`.
4. **SDL3 or SDL2?** **SDL3, on both toolchains.** The migration was small: four files touch SDL
   (`HalDisplay.cpp/.h`, `HalGPIO.cpp`, `simulator_main.cpp`), ~20 mechanical renames, and one
   real change — `SDL_GetKeyboardState` returns `const bool*` in SDL3. The desktop PlatformIO env
   moved with it, so there is one source set and no per-platform SDL shim.

### Resolved beyond the original list

- **§4.2's two Python patches were already dead.** `run_simulator.py`'s docstring describes them
  but the implementing code no longer exists in the 63-line script. `BookMetadataCache`
  `size_t` → `uint32_t` is a real source edit upstream (`BookMetadataCache.h:23`), so the silent
  cache-corruption risk the plan feared does not exist. The `setOrientation` notify is superseded
  by polling in `presentIfNeeded`.
- **A trap the plan did not anticipate.** `SDL_PushEvent` does not update SDL's internal keyboard
  state, so §3's injection model drives edge reads but not level reads — long-press power-off and
  held-button autorepeat cannot work until `HalGPIO` grows a live injection API. This is the one
  substantive open decision left; see [ios/README.md](ios/README.md).
- **Scope changed during implementation.** Gestures were built, verified, then removed: input is
  now an on-screen pad with one control per physical button, which is what expresses a genuine
  hold. §3's gesture mapping is historical.
