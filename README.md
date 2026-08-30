# CrossPoint Simulator

A desktop simulator for [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader)-based firmware. Compiles the firmware natively and renders the e-ink display in an SDL2 window. No device required. Can be used with forks of Crosspoint but any new methods added to the firmware will need to be stubbed. If your fork diverges from the CrossPoint HAL, see [FORKING.md](FORKING.md).

> [!NOTE]
> **Platform support:** macOS and Linux/WSL use different native compiler and library flags. Start from `sample-platformio-macos.ini` on macOS, or `sample-platformio-linux-wsl.ini` on Linux/WSL. Native Windows is not supported; use WSL and follow the Linux instructions.

> [!WARNING]
> This has been tested on x86_64 macOS (Intel), ARM64 macOS (Apple Silicon,
> M4), and Ubuntu under WSL on Windows. Other platforms may need additional
> libraries or platform-specific stubs.

## Prerequisites

SDL2 and `curl` must be installed on the host machine. Linux/WSL users also need OpenSSL development headers for MD5 support.

```bash
# macOS
brew install sdl2

# Linux — Debian/Ubuntu (including WSL)
sudo apt install libsdl2-dev libssl-dev

# Linux — Fedora/RHEL
sudo dnf install SDL2-devel openssl-devel

# Linux — Arch
sudo pacman -S sdl2 openssl
```

## Integration

Add the simulator to your firmware's platformio.ini as a `lib_dep` and configure the `[env:simulator]` environment. Use the sample file for your host OS:

- `sample-platformio-macos.ini`
- `sample-platformio-linux-wsl.ini`

No scripts need to be copied into the firmware repo for the simulator to build. The simulator library automatically patches consumer-side compatibility issues from its own build script when PlatformIO fetches it as a dependency, including the common `GfxRenderer::setOrientation()` hook needed for SDL window resizing.

Keep the sample `build_src_filter` exclusions unless your firmware has already
moved those files behind simulator guards. In the current CrossPoint layout,
the firmware-owned `CrossPointWebServer` and `WebDAVHandler` compile against
the simulator's lower-level `WebServer`, `WebSocketsServer`, and
`NetworkClient` shims. This exercises the real settings, files, status, and
WebDAV routes instead of a reduced simulator-only substitute.

The simulator defaults to the original X4 panel shape and SSD1677 controller.
Device-specific environments can extend the base simulator environment with
these flags:

- `-DSIMULATOR_DEVICE_X3` switches the framebuffer to 792x528 landscape,
  selects the X3 board profile, and exposes the simulator tilt sensor.
- `-DSIMULATOR_DEVICE_X4_PRO` keeps the X4 family's 800x480 framebuffer and
  selects the X4 Pro board profile. It exposes touch and swipe input, the
  capacitive Home key, the RTC, display inversion, and frontlight state.
- `-DSIMULATOR_DEVICE_STICKY` selects the Seeed Sticky's 800x480 SSD1677
  profile. It exposes touch and swipe input, the RTC, and the tilt sensor
  without exposing the X4 Pro-only Home key or frontlight.
- `-DSIMULATOR_DEVICE_PAPERMONO` selects the M5Stack PaperMono's 800x480
  SSD1677 profile. It exposes FT6336-compatible touch and swipe input, the RTC,
  and single-channel frontlight state without a Home key or color-temperature
  control.
- `-DSIMULATOR_DISPLAY_UC8179` selects the newer UC8179 controller used by
  some X4 and X4 Pro production batches.
- `-DSIMULATOR_DISPLAY_UC8279` selects UC8279d on X3, or the 800x480 UC8279
  controller on X4-family profiles.

The sample PlatformIO files include ready-to-use environments for the original
profiles plus `simulator_sticky`, `simulator_x3_uc8279`, `simulator_x4_uc8179`,
`simulator_x4_uc8279`, `simulator_x4_pro_uc8179`, and
`simulator_x4_pro_uc8279`, plus `simulator_papermono`. The UC8279 X4 Pro path
mirrors current FreeInk SDK support but remains pending validation on physical
UC8279 X4 Pro hardware.

Controller profiles expose the same framebuffer geometry and device
capabilities as their original production run. The simulator records the
selected `BoardConfig::DisplayController` and identifies it in the window title;
it does not attempt to model controller timing, LUT waveforms, ghosting, or
power sequencing.

If a fork has a custom renderer and the auto-patch cannot recognize it, its simulator build should notify the display when orientation changes:

```cpp
#ifdef SIMULATOR
display.setSimulatorOrientation(static_cast<int>(o));
#endif
```

Put that in the renderer's orientation setter after updating the renderer's own orientation state.
By default, the simulator keeps its own `JPEGDEC`, `PNGdec`, and QRCode compatibility shims so existing firmware projects can update this library without changing their simulator environment. To test against the native decoder libraries instead, follow the opt-in comments in the sample PlatformIO files: define `CROSSPOINT_SIM_USE_NATIVE_DECODERS`, set `lib_compat_mode = off`, change simulator `lib_ignore` to `hal, WebSockets`, and add the native `PNGdec`/`JPEGDEC` dependencies. `WebSockets` is ignored only in native simulator builds because this repo supplies the host-backed `WebSocketsServer` implementation.

If you only want a self-contained simulator dependency, stop there.

If you also want the `Run Simulator` task to appear in the consuming repo's PlatformIO IDE task list (under the "Custom" folder), let the consuming project own the IDE task registration. Add `custom_run_simulator_target_owner = project` to `[env:simulator]`, then add one project-level hook:

For a normal fetched dependency:

```ini
custom_run_simulator_target_owner = project

extra_scripts =
  pre:scripts/gen_i18n.py
  pre:scripts/git_branch.py
  pre:scripts/build_html.py
  post:.pio/libdeps/$PIOENV/simulator/run_simulator_project.py
```

For a local symlinked dependency:

```ini
custom_run_simulator_target_owner = project

extra_scripts =
  pre:scripts/gen_i18n.py
  pre:scripts/git_branch.py
  pre:scripts/build_html.py
  post:../crosspoint-simulator/run_simulator_project.py
```

Use the symlink form only when the `Crosspoint` repo and this `crosspoint-simulator` repo are checked out side by side and your `lib_deps` entry is:

```ini
simulator=symlink://../crosspoint-simulator
```

The `custom_run_simulator_target_owner = project` line tells the library-side hook not to register the same launcher a second time. Without that, closing one simulator window can immediately relaunch another because both the library hook and the project hook try to own `run_simulator`.

Do not point `post:` at `run_simulator.py` directly. That file is already auto-loaded via `library.json` and is the backward-compatible library hook.

The `post:` line above only exposes the task in the consuming project UI. The actual launcher logic still lives in this simulator repo.


## Setup

Place EPUB books at `./fs_/books/` in the Crosspoint repo's root. This maps to the `/books/` path on the physical SD card.

## Build and run

Run this command from the Crosspoint project after you have added the `[env:simulator]` config to Crosspoint's `platformio.ini` file. Alternatively, if you added the project hook above, you can click "Build" from PlatformIO's IDE task list and then "Run Simulator" (nested under the "Custom" folder).

```bash
pio run -e simulator -t run_simulator
```

## Controls

| Key    | Action                             |
| ------ | ---------------------------------- |
| ↑ / ↓  | Page back / forward (side buttons) |
| ← / →  | Left / right front buttons         |
| Return | Confirm / Select                   |
| Escape | Back                               |
| P      | Power                              |
| S      | Simulate sleep                     |
| H      | X4 Pro capacitive Home key         |
| Mouse  | Touch-device tap and swipe         |

When the simulator is on the sleep screen, pressing any mapped simulator key wakes it. Under the hood the simulator relaunches itself and reports a synthetic power-button wake, because the native build has no real ESP deep-sleep resume path.

## Automated QA and screenshots

Two optional environment variables make repeatable navigation and screenshot
tests possible without desktop-control permissions:

- `CROSSPOINT_SIM_INPUT_SCRIPT` schedules input as
  `<milliseconds>:<action>`, separated by semicolons. Button actions use
  `<key>[:<hold-milliseconds>]`; keys are `BACK`, `ENTER`, `LEFT`, `RIGHT`,
  `UP`, `DOWN`, `POWER`, `SLEEP`, `HOME`, and `QUIT`. A normal key press is
  held for 80 ms unless a duration is provided.
- Touch-device actions use `TAP:<x>,<y>[,<hold-milliseconds>]` or
  `SWIPE:<x1>,<y1>,<x2>,<y2>[,<duration-milliseconds>]`. Coordinates are in
  displayed logical pixels, so they match UI layouts and screenshots after the
  firmware changes orientation. Normalized coordinates from 0.0 to 1.0 are
  also accepted for existing scripts.
- `CROSSPOINT_SIM_SCREENSHOTS` saves BMP screenshots as
  `<milliseconds>:<path>`, separated by semicolons. Create the destination
  directory before running the simulator.
- The optional `simulator_heap` sample profile enables a bounded TLSF heap arena
  for runtime firmware allocations. `ESP.getFreeHeap()`, `ESP.getMinFreeHeap()`, and
  `ESP.getMaxAllocHeap()` then report live arena state, including fragmentation.
  `CROSSPOINT_SIM_HEAP_BYTES` sets the post-setup budget in bytes; the default is
  a sample 153371-byte budget. To approximate a specific scenario, copy the free
  heap reported by the physical device at the same point in the workflow.
- `CROSSPOINT_SIM_HEAP_TRACE=1` records live allocation call stacks and prints
  the largest sites on OOM. `CROSSPOINT_SIM_HEAP_VIZ=<directory>` writes SVG
  snapshots of arena occupancy and free-block fragmentation. Both diagnostics
  allocate from the host heap so they do not consume the budget being measured.
- On macOS, C allocations are counted only when their immediate caller belongs
  to the firmware/simulator executable; allocations made directly by AppKit,
  SDL, and other shared frameworks remain on the host heap.
- Regular simulator builds, which do not define `CROSSPOINT_SIM_HEAP_ARENA`, retain the older
  `CROSSPOINT_SIM_FREE_HEAP` and `CROSSPOINT_SIM_MAX_ALLOC_HEAP` reported-value
  overrides. Those overrides do not make allocations fail and are ignored once
  the arena is active.
- A sleep/wake test starts a fresh simulator process, matching the existing
  deep-sleep model. Set `CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE` and
  `CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE` for that second process. The
  pre-sleep schedules are cleared during relaunch so they cannot repeat
  forever.

Times are measured from process startup. For example:

```bash
mkdir -p ./qa-artifacts
CROSSPOINT_SIM_INPUT_SCRIPT='900:DOWN;1250:DOWN;1600:DOWN;1900:ENTER;3000:QUIT' \
CROSSPOINT_SIM_SCREENSHOTS='2400:./qa-artifacts/settings.bmp' \
  .pio/build/simulator/program
```

An X4 Pro touch and Home-key smoke test can use:

```bash
CROSSPOINT_SIM_INPUT_SCRIPT='2000:TAP:240,530;3000:HOME:100;3900:QUIT' \
CROSSPOINT_SIM_SCREENSHOTS='2500:./qa-artifacts/x4-pro-settings.bmp;3500:./qa-artifacts/x4-pro-home.bmp' \
  .pio/build/simulator_x4_pro/program
```

For Sticky, the same touch path is available without the Home key:

```bash
CROSSPOINT_SIM_INPUT_SCRIPT='2000:TAP:240,530;3600:QUIT' \
CROSSPOINT_SIM_SCREENSHOTS='1500:./qa-artifacts/sticky-home.bmp;3000:./qa-artifacts/sticky-settings.bmp' \
  .pio/build/simulator_sticky/program
```

A deterministic sleep/wake smoke test can use:

```bash
CROSSPOINT_SIM_INPUT_SCRIPT='900:SLEEP;3500:ENTER' \
CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE='2200:QUIT' \
CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE='1600:./qa-artifacts/wake.bmp' \
  .pio/build/simulator/program
```

The screenshot contains the SDL renderer output at the host's actual drawable
resolution, including Retina/HiDPI scaling. BMP is used because it is supported
directly by SDL2 and adds no image-encoding dependency to the simulator.

## Memory diagnostics and their limits

The arena work is adapted from reader PR
[#2455](https://github.com/crosspoint-reader/crosspoint-reader/pull/2455). It
routes `malloc`, `calloc`, `realloc`, `free`, and C++ `new`/`delete` through the
same Espressif TLSF implementation used to model a bounded, fragmentable heap.
It activates after firmware `setup()` so SDL initialization and simulator
scaffolding do not consume the device budget. A failed `new (std::nothrow)`
returns null; a failed throwing `new` aborts in the sample profile, matching the
firmware's `-fno-exceptions` failure behavior.

This is useful for automatic OOM-path and fragmentation regression testing, but
it is not a cycle- or byte-exact ESP32 profiler. Allocations made before arena
activation are excluded, 64-bit host object sizes differ from 32-bit ESP32
sizes, host decoder/shim implementations can allocate differently, and the
simulator does not reproduce task stacks, IRAM/DRAM capabilities, e-ink
waveforms, DMA constraints, or radio/SDK heap use. Calibrated physical-device
logs remain the release gate for memory-sensitive firmware.

On Linux/WSL, use the separate `simulator_asan` sample environment for AddressSanitizer
and UndefinedBehaviorSanitizer checks. That profile disables the bounded arena:
ASan needs each allocation to remain a distinct host allocation so its redzones
can detect overflows and use-after-free. Run both profiles there; they answer
different questions. The current macOS `sdl2-compat` library stops an ASan build
in its load-time error dialog, so the macOS sample does not advertise that target.

## Notes

**Host-backed network flows**: OPDS/catalog downloads and KOReader sync use the
host's `curl` binary through simulator implementations of `HTTPClient` and
`esp_http_client`. This keeps the firmware code path intact while allowing the
desktop build to reach real HTTP/HTTPS services.

**Mocked downloads**: Set `CROSSPOINT_SIM_HTTP_MOCK_ROOT` to a folder of local
fixtures to make host-backed HTTP requests return local files by basename before
falling back to the real network. This is useful for SD-font testing because the
firmware can request its normal release URLs while the simulator serves a local
`fonts.json` and `.cpfont` files:

```bash
cd /path/to/firmware
python3 -m pip install -r lib/EpdFont/scripts/requirements.txt
python3 lib/EpdFont/scripts/build-sd-fonts.py \
  --only NotoSansExtended \
  --manifest \
  --base-url "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/local/"
CROSSPOINT_SIM_HTTP_MOCK_ROOT="$PWD/lib/EpdFont/scripts/output" \
  pio run -e simulator -t run
```

The mock still uses the firmware's normal manifest parsing, file download,
write-to-SD, `.cpfont` validation, registry refresh, and font-selection flow.

**File transfer**: The simulator provides host-backed `WebServer`,
`WebSocketsServer`, and `NetworkClient` shims so firmware-owned file-transfer
routes can run on the host. Firmware web servers that bind port 80 are exposed
on `http://127.0.0.1:8080/`; WebSocket servers that bind port 81 are exposed on
`ws://127.0.0.1:8081/`. Set `CROSSPOINT_SIM_HTTP_PORT` to another unprivileged
port if that pair is occupied; the WebSocket endpoint uses the following port.
For example, `CROSSPOINT_SIM_HTTP_PORT=18080` exposes HTTP on 18080 and
WebSocket on 18081. This supports the browser file manager, WebSocket upload
progress, streamed downloads, and common WebDAV-style requests such as
`OPTIONS`, `PROPFIND`, `PUT`, `DELETE`, `MKCOL`, `MOVE`, and `COPY`. WebDAV
`LOCK` and `UNLOCK` remain compatibility-only unless the firmware implements
locking semantics.

The `run_simulator` target also accepts the port through PlatformIO, which is
convenient when the conflict is permanent on a development machine:

```ini
[env:simulator]
custom_simulator_http_port = 18080
```

Direct binary launches use the environment variable form.

**Firmware updates**: OTA and SD-card firmware flashing are non-destructive in
the simulator. The simulator stubs those update paths so the UI can be opened
without flashing firmware or changing boot partitions.

**Image previews**: The default simulator shims decode JPEG and PNG files on the
host and render a rough grayscale preview through the firmware's normal image
callbacks. This is meant to make image pages and PNG sleep overlays visible
while testing desktop flows. Native decoder libraries can be enabled with the
sample config's opt-in flags when decoder compatibility matters more than the
self-contained default. Neither mode simulates device-specific e-ink image
quality, refresh behaviour, or memory pressure.

**Cache**: On first open of an ebook, an "Indexing..." popup will appear while the section cache is built. If you see rendering issues after a code change that affects layout, delete `./fs_/.crosspoint/` to clear stale caches.

> [!WARNING]
> **Upstream compatibility:** The simulator mirrors interfaces used by Crosspoint. If Crosspoint adds or changes methods in a shared library and the simulator build reaches that code path, the simulator can fail to compile or link until a matching implementation or stub is added here. In many cases this is just a small no-op shim. Open a PR if the change tracks upstream CrossPoint, fills a gap in the emulated Arduino/ESP-IDF layer, or fixes the simulator itself. If the change only matches your own fork's HAL, maintain it in a fork of this repo instead. See [FORKING.md](FORKING.md).
