# Simulator Development Context

## What This Is

A desktop simulator for [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) firmware. Compiles the firmware as a native binary (PlatformIO `platform = native`) and renders the e-ink display in an SDL2 window. Now supports macOS, Linux, and WSL — Windows native is not supported.

The repo ships as a PlatformIO library; downstream firmware adds it as a `lib_dep` named `simulator` and configures an `[env:simulator]` environment that builds with `-DSIMULATOR`.

## Current State

The simulator builds and runs on macOS and Linux/WSL. Portrait orientation is correct, gray shading renders cleanly at HiDPI, file browsing lists EPUBs from `./fs_/books/`, and reading a book shows the "Indexing..." popup on first open before rendering pages. Window close exits cleanly. Icons render in the UI (drawImage / drawImageTransparent are now implemented, not stubs). JPEG and PNG decoder shims render rough host-side previews for EPUB images and PNG sleep overlays by default; native `PNGdec`/`JPEGDEC` can be enabled explicitly with `CROSSPOINT_SIM_USE_NATIVE_DECODERS`, `lib_compat_mode = off`, and simulator `lib_ignore = hal, WebSockets`. BoardConfig exposes X4 by default, X3 via `SIMULATOR_DEVICE_X3`, and X4 Pro via `SIMULATOR_DEVICE_X4_PRO`. The X4 Pro profile keeps the 800x480 panel and adds touch/swipe input, the capacitive Home key, RTC, inversion, and frontlight state. Host-backed web shims cover `WebServer`, `WebSocketsServer`, and `NetworkClient`, with firmware port 80 exposed on `http://127.0.0.1:8080/` and port 81 WebSockets exposed on `ws://127.0.0.1:8081/`. `CROSSPOINT_SIM_HTTP_PORT` moves both host ports as a pair.

## Setup

**Prerequisites**

- macOS: `brew install sdl2`
- Debian/Ubuntu/WSL: `sudo apt install libsdl2-dev libssl-dev`
- Fedora/RHEL: `sudo dnf install SDL2-devel openssl-devel`
- Arch: `sudo pacman -S sdl2 openssl`

Linux/WSL needs OpenSSL because [MD5Builder_linux.h](src/MD5Builder_linux.h) wraps `openssl/md5.h` instead of the macOS `CommonCrypto` path used in [MD5Builder.h](src/MD5Builder.h).

**Integration into firmware**

1. Copy [sample-platformio-macos.ini](sample-platformio-macos.ini) or [sample-platformio-linux-wsl.ini](sample-platformio-linux-wsl.ini) contents into the firmware's `platformio.ini` as a new `[env:simulator]` block.
2. For local dev, replace the git ref with a symlink: `simulator=symlink://../crosspoint-simulator`.
3. Optional: if you want PlatformIO's IDE task list to show `Run Simulator`, add `custom_run_simulator_target_owner = project` and the `post:` hook shown in [README.md](README.md). Do not copy [run_simulator.py](run_simulator.py) into the firmware repo; it is auto-loaded from this library through [library.json](library.json).
4. Optional native decoder mode: add `-DCROSSPOINT_SIM_USE_NATIVE_DECODERS`, set `lib_compat_mode = off`, use `lib_ignore = hal, WebSockets`, and add the native `PNGdec`/`JPEGDEC` dependencies shown in the sample comments.
5. Place EPUBs at `./fs_/books/` (relative to the binary's working directory). This maps to SD card path `/books/`.

**Build and run**

```bash
pio run -e simulator -t run_simulator
```

## Architecture (Key Files)

| Purpose                     | Path                                                                |
| --------------------------- | ------------------------------------------------------------------- |
| Simulator entry point       | [src/simulator_main.cpp](src/simulator_main.cpp)                    |
| SDL display impl            | [src/HalDisplay.cpp](src/HalDisplay.cpp)                            |
| SDL keyboard / quit input   | [src/HalGPIO.cpp](src/HalGPIO.cpp)                                  |
| POSIX-fd filesystem mock    | [src/HalStorage.cpp](src/HalStorage.cpp)                            |
| FreeRTOS → std::thread mock | [src/freertos/](src/freertos/)                                      |
| Arduino / ESP-IDF stubs     | [src/Arduino.h](src/Arduino.h), [src/ESP.cpp](src/ESP.cpp), etc.    |
| MD5: macOS path             | [src/MD5Builder.h](src/MD5Builder.h) (CommonCrypto)                 |
| MD5: Linux path             | [src/MD5Builder_linux.h](src/MD5Builder_linux.h) (OpenSSL)          |
| Sample firmware ini (macOS) | [sample-platformio-macos.ini](sample-platformio-macos.ini)          |
| Sample firmware ini (Linux) | [sample-platformio-linux-wsl.ini](sample-platformio-linux-wsl.ini)  |
| Filesystem root (runtime)   | `./fs_/` relative to the binary's working dir                       |

## How It Works

**Display thread model.** SDL on macOS requires all SDL calls happen on the main thread, but firmware drives rendering from a FreeRTOS render task (now a `std::thread`). The split: [HalDisplay::refreshDisplay](src/HalDisplay.cpp) (background thread) converts the 1bpp framebuffer to ARGB pixels and sets an atomic `pendingPresent` flag. [HalDisplay::presentIfNeeded](src/HalDisplay.cpp) (called from `simulator_main` on the main thread) uploads to the texture, applies orientation rotation, and calls `SDL_RenderPresent`.

**Orientation.** The renderer's `rotateCoordinates` writes content into the physical landscape buffer rotated 90° CCW for `Portrait` (and 90° CW for `PortraitInverted`). The simulator undoes this with `SDL_RenderCopyEx` rotation:

| Orientation        | SDL angle |
| ------------------ | --------- |
| `Portrait`         | `+90.0`   |
| `PortraitInverted` | `−90.0`   |
| `Landscape*`       | `0`       |

`SDL_RenderCopyEx` rotates around the dst rect's centre, so the dst rect is landscape-oriented (`{−80, 80, 400, 240}`) for portrait modes; after rotation it fills the portrait window.

**Rendering quality.** `SDL_WINDOW_ALLOW_HIGHDPI` plus `SDL_RenderSetLogicalSize` keeps logic in window coords while letting macOS use full Retina pixels. `SDL_HINT_RENDER_SCALE_QUALITY=1` (must be set before texture creation) enables bilinear filtering so Bayer-dithered grays don't show as harsh black/white stripes.

**Filesystem.** [HalStorage](src/HalStorage.cpp) uses POSIX file descriptors (`::open` / `::read` / `::write` / `lseek` / `fsync`) — not `std::fstream`. fstream's separate get/put pointers, eofbit-blocks-seek behaviour, and write-only mode restrictions caused several silent-corruption bugs early on; POSIX fds avoid all of them. `HalStorage::open()` `stat()`s the path and routes to `openAsDir` (DIR\*) or file-open. Directory iteration uses `readdir`/`rewinddir`, skipping any entry starting with `.`. All paths are prefixed with `./fs_` so the simulator's filesystem is sandboxed in a single directory under the binary's working dir.

**Input.** [HalGPIO::update](src/HalGPIO.cpp) owns the SDL event pump (so polling isn't split between callers). It maps SDL scancodes → button indices (`BTN_BACK=0` … `BTN_POWER=6`) and maintains per-frame pressed/released arrays. On X4 Pro, SDL mouse input is transformed from the oriented logical window back into normalized physical touch coordinates, and `H` emulates the capacitive Home key. `SDL_QUIT` sets the shared `quitRequested` atomic that `HalDisplay::shouldQuit()` reads.

**Threading.** [src/freertos/](src/freertos/) maps `xTaskCreate` to `std::thread`, `ulTaskNotifyTake`/`xTaskNotify` to a condvar + counter, and `SemaphoreHandle_t` to `std::recursive_mutex`. `thread_local SimTaskHandle*` lets each task thread find its own handle for notifies.

**Time.** [Arduino.h](src/Arduino.h) `millis()` and `micros()` use `std::chrono::steady_clock`, not `system_clock`, so wall-clock changes don't affect timing. (Was `system_clock` originally; switched for predictability across host systems.)

## Recent Changes (since 2026-03-17)

### Linux / WSL support (PR #1, merged 2026-04-23)

- New [src/MD5Builder_linux.h](src/MD5Builder_linux.h): OpenSSL-backed `MD5Builder` for Linux. macOS keeps using [src/MD5Builder.h](src/MD5Builder.h) (CommonCrypto). Downstream firmware swaps which one it includes per host.
- README expanded with install instructions for Debian/Ubuntu, Fedora/RHEL, Arch.
- [src/Arduino.h](src/Arduino.h) → switched `millis`/`micros` from `system_clock` to `steady_clock` (5babace).
- [src/WString.h](src/WString.h) explicitly includes `<cstring>` (Linux compilers don't pull it in transitively the way macOS clang does).
- The single `sample-platformio.ini` was split into two host-specific files. macOS keeps `-arch arm64` and `/opt/homebrew/{include,lib}` paths; Linux/WSL adds `-lssl -lcrypto` and `-Wno-deprecated-declarations` (OpenSSL 3.x deprecates `MD5_*`).

### X3 device support scaffolding (commit 674c571, 2026-04-23)

- [HalGPIO](src/HalGPIO.h) now has `enum class DeviceType : uint8_t { X4, X3 }` plus `deviceIsX3()` / `deviceIsX4()` helpers. `_deviceType` defaults to `X4`, and `SIMULATOR_DEVICE_X3` selects the X3 device path and 792x528 framebuffer. This matches a downstream firmware change that branches on device type — without it, simulator builds break.

### Match upstream HAL surface (2026-04-06 onward)

- [HalDisplay](src/HalDisplay.cpp) gained `getDisplayWidth/Height/WidthBytes/getBufferSize` runtime accessors and an `extern HalDisplay display;` global definition.
- [HalGPIO](src/HalGPIO.cpp) added `startDeepSleep()` and `verifyPowerButtonWakeup()` no-ops, plus the `extern HalGPIO gpio;` global.
- [WiFi.h](src/WiFi.h) added `SSID(int)`, `RSSI(int)`, `encryptionType(int)`, `setSleep`, `getHostname`, `softAPgetStationNum`, `scanComplete`, etc. — anything new the firmware calls needs a stub here.

### Image rendering implemented (commit c19b64c, 2026-04-07)

- `drawImage` and `drawImageTransparent` were no-op stubs; now they copy 1bpp packed image data into the framebuffer (drawImage = overwrite, drawImageTransparent = AND-mask). This makes UI icons visible.

### Host-side image decoder previews (2026-05-08)

- [src/JPEGDEC.h](src/JPEGDEC.h) and [src/PNGdec.h](src/PNGdec.h) decode via vendored [src/stb_image.h](src/stb_image.h) by default, then feed grayscale/RGBA rows through the same callback shape used by the embedded libraries. With `CROSSPOINT_SIM_USE_NATIVE_DECODERS`, those headers pass through to the native PlatformIO `JPEGDEC`/`PNGdec` dependencies instead. Both paths are desktop preview paths; neither models e-ink waveforms, device memory pressure, or exact image quality.

### Host-backed web server shims (2026-05-10)

- [src/WebServer.cpp](src/WebServer.cpp), [src/WebSocketsServer.cpp](src/WebSocketsServer.cpp), and [src/NetworkClient.cpp](src/NetworkClient.cpp) provide native socket-backed shims for firmware web routes. Firmware servers that bind port 80 are exposed on `http://127.0.0.1:8080/`; WebSocket servers that bind port 81 are exposed on `ws://127.0.0.1:8081/`.
- The sample PlatformIO files compile the current firmware-owned `network/CrossPointWebServer.cpp` and `network/WebDAVHandler.cpp` with `CROSSPOINT_SIMULATOR_PROJECT_WEBSERVER`, which disables the simulator's legacy reduced substitute. Only embedded updater/flasher paths remain excluded.

### Mac App Store purpose strings (2026-07-31)

- App Store Connect rejected the CrossPoint X3 upload (version 0.1.0, build 1) with ITMS-90683 for a missing `NSCameraUsageDescription`, and warned about `NSBluetoothAlwaysUsageDescription`. The simulator only calls `SDL_Init(SDL_INIT_VIDEO)` — the flagged APIs come from Apple's static scan of the linked SDL2 library, which references camera and game-controller (Bluetooth) APIs. Apple requires the purpose string regardless of whether the app calls them.
- The repo had no bundle packaging at all, so there was no `Info.plist` to fix. New [packaging/macos/Info.plist.in](packaging/macos/Info.plist.in) holds the strings and is the single source of truth; [packaging/macos/package_macos_app.py](packaging/macos/package_macos_app.py) has `build` (wrap a binary in a `.app`), `patch` (inject missing keys into a bundle built elsewhere, preserving binary-plist format), and `verify` (non-zero exit before upload).
- `run_simulator.py` registers a `package_macos_app` target under its own `builtins` sentinel. All path resolution happens inside the target action, never at script-load time, so a packaging problem cannot break ordinary simulator builds. The library checkout is found via `__file__` first, then a `$PROJECT_LIBDEPS_DIR/$PIOENV` scan, because SCons does not guarantee `__file__` in SConscript globals.
- Not covered: code signing, notarization, and embedding the SDL2 dylib. Bundle id and build number are caller-supplied — a wrong bundle id or a reused build number fails the upload for reasons unrelated to purpose strings.

### TestFlight deploy path (2026-07-31)

- [packaging/macos/deploy.sh](packaging/macos/deploy.sh) chains build → bundle → verify purpose strings → embed dylibs → sign → `productbuild` → `altool --upload-app` → tag. macOS only.
- `codesign` needs the login keychain, which only a GUI Terminal session has; firing the deploy from a sandboxed agent shell or bare SSH fails with `errSecInternalComponent`. [packaging/macos/deploy.applescript](packaging/macos/deploy.applescript) hands the command to Terminal.app, which is what lets an agent on the Mac deploy unattended. Pattern and several gates (build-number drift, deploy lock, the 90382 daily cap, the rc-19 expired-agreement diagnosis) are ported from the crds-ios pipeline.
- Build numbers auto-bump from the last `macos-build-N` tag. Apple silently rejects a duplicate build number, and build 1 is already consumed by the ITMS-90683 rejection, so the floor is 2.
- **Bundled-app storage root.** A `.app` launched from Finder has cwd `/`, so the default `./fs_` resolved to an unwritable `/fs_` and the library came up empty. `configuredStorageRoot()` now detects `*.app/Contents/MacOS/` via `_NSGetExecutablePath` and returns `$HOME/Library/Application Support/<AppName>/fs_`. Under the App Sandbox, HOME is already the container, so the path stays inside it. `CROSSPOINT_SIM_SD` still wins, and non-bundled dev builds are untouched.
- **Known sandbox gap:** Mac App Store builds must be sandboxed, and the sandbox blocks spawning binaries outside the bundle. `SimHttpFetch.h` uses `popen("curl ...")`, so OPDS/catalog downloads, KOReader sync, and SD-font fetches cannot work on TestFlight until they move to an in-process HTTP client.

### HalStorage menu-items fix (commit 40c578e, 2026-04-19)

- Major HalStorage refactor — directory iteration and child-file handling were tightened so menu lists populate correctly.

### Cleaner exit (current [simulator_main.cpp](src/simulator_main.cpp))

- Loop now ends with `_exit(0)` instead of `return 0` after `SDL_Quit()`. `_exit` skips C++ global destructors, which avoids a SIGABRT/SIGSEGV race: `activityManager` and other globals are constructed before the render thread starts, and the render task runs a `[[noreturn]]` infinite loop. If normal `exit()` runs destructors while the render thread is mid-render, they race → "quit unexpectedly" dialog. SDL is torn down before `_exit` so this is safe.

## Historical Bug Fixes

These shaped the current code; details kept short since the fixes are already in place. Useful when a similar symptom resurfaces.

**Black screen** — `clearScreen` was writing to the SDL pixel array instead of the framebuffer; now `memset(getFrameBuffer(), color, BUFFER_SIZE)`.

**Sideways / upside-down portrait** — Two bugs: (1) Portrait and PortraitInverted had their SDL rotation angles swapped (renderer stores Portrait CCW → SDL must rotate +90° CW to undo); (2) `SDL_RenderCopyEx` rotates around dst centre, so the rect must be landscape-shaped and centre-offset, not portrait-shaped.

**Dithered UI showed harsh stripes** — Add `SDL_WINDOW_ALLOW_HIGHDPI`, `SDL_RenderSetLogicalSize`, and `SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1")` *before* `SDL_CreateTexture`.

**"Program quit unexpectedly" on window close** — Replaced `exit(0)` from the SDL handler with `quitRequested.store(true)`; main loop checks `display.shouldQuit()`. (Later strengthened with `_exit(0)` after `SDL_Quit` — see Recent Changes.)

**File browser empty** — `HalStorage` directory iteration (`open` / `isDirectory` / `rewindDirectory` / `openNextFile` / `getName`) was no-op stubs. Now backed by `opendir` / `readdir` / `rewinddir`, with `stat` to distinguish dir from file.

**Stuck on boot screen** — `xTaskCreate` was a no-op so `renderTaskLoop` never ran. Now backed by `std::thread` + condvar in [src/freertos/task.h](src/freertos/task.h).

**Ebook reader showed nothing on first press (and "double press required" symptom)** — Originally three separate `std::fstream` bugs: (1) `eofbit` set by reading near EOF silently blocked all later seeks (needed `stream.clear()`); (2) `tellg()` returns -1 on write-only fstreams (needed `tellp()` fallback); (3) write-only fstreams can't seek at all (needed `in | out`). All three were eliminated by rewriting [HalFile::Impl](src/HalStorage.cpp) on POSIX file descriptors instead of `std::fstream` — POSIX fds have no eof state, no separate get/put pointers, and no mode-dependent seek restrictions.

**Spine cache files failed to open** — The HalFile flag-translation code was converting SdFat flag values to POSIX, but [src/common/FsApiConstants.h](src/common/FsApiConstants.h) just `#include <fcntl.h>` and `typedef int oflag_t`, so callers already pass native POSIX values. The translation stripped CREAT/TRUNC bits. Fix: `HalFile::Impl::open()` now passes flags straight through to `::open()`.

**LOG output invisible** — `LOG_*` was going to `std::cout` via `HWCDC::write` while `[SIM]` errors went to `std::cerr`. Fixed [HardwareSerial.h](src/HardwareSerial.h) so `HWCDC::write` and `HWCDC::printf` both go to `std::cerr` (and `printf` actually formats now — was a no-op stub).

**Spine entries had empty hrefs after caches loaded** — `BookMetadataCache::lutOffset` was `size_t` (8 bytes on macOS 64-bit) but `headerASize` was computed as `sizeof(uint32_t)` (4 bytes). The 4-byte mismatch shifted all spine seeks. Fixed in firmware by changing `lutOffset` to `uint32_t` (on ESP32 they're identical, so no device impact).

After any of the storage / cache fixes: `rm -rf ./fs_/.crosspoint/` to drop stale caches built with broken code.

## Known Remaining Work

- SDL window size now follows orientation changes at present time; keep resize and `SDL_RenderSetLogicalSize` on the main-thread `presentIfNeeded()` path. The library build hook patches the common `GfxRenderer::setOrientation()` implementation so consuming repos notify `HalDisplay` without a manual source edit.
- Thread safety relies on `std::recursive_mutex` in `RenderLock`; no broader audit.
- `HalPowerManager::startDeepSleep` should not trigger on `WakeupReason::Other` — verify if it ever does.
- Each new HAL method added in upstream firmware will fail to link until a matching stub is added here. Most are one-line no-ops.

## Button Mapping

| Key    | Action                             |
| ------ | ---------------------------------- |
| ↑ / ↓  | Page back / forward (side buttons) |
| ← / →  | Left / right front buttons         |
| Return | Confirm / Select                   |
| Escape | Back                               |
| P      | Power                              |
