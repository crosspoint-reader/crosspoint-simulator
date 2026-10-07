# EEGO A4

This profile follows CrossPoint's `eego_a4` environment added in
[firmware PR 3887](https://github.com/crosspoint-reader/crosspoint-reader/pull/3887)
and FreeInk SDK revision `9729236ce7b730b81b8fcceec4aa77051b6dfae3`.

The source of truth is the SDK's
[board profile](https://github.com/Free-Ink/freeink-sdk/blob/9729236ce7b730b81b8fcceec4aa77051b6dfae3/libs/hardware/BoardConfig/include/BoardConfig.h#L1405)
and [hardware notes](https://github.com/Free-Ink/freeink-sdk/blob/9729236ce7b730b81b8fcceec4aa77051b6dfae3/docs/eego-a4-support.md).
The SDK documents incomplete physical hardware validation. This host profile
does not validate those provisional pin assignments or controller waveforms.

## Profile Contract

- Native framebuffer: 768x552, UC8279C, 52,992 bytes at one bit per pixel.
  Portrait UI dimensions are 552x768; landscape dimensions are 768x552.
- Viewable insets: top/right/bottom/left = 24/24/8/24, transformed by the renderer.
- GSLX680 touch coordinates use the existing orientation-aware host tap/swipe path.
- Physical Up/Down/Power only: keyboard Up/Down/P. Escape, Return and Left/Right
  are disabled, rather than adding navigation buttons the device lacks.
- H emits the GSLX680 screen-key press, short-release and 700 ms hold events.
  Page Up/Down do not emit Metalio cover events.
- PCF8563-compatible clock availability, backed by host time; no tilt sensor or haptics.
- Optional LM3630A frontlight state: brightness and warm/cool balance. The host
  models state only, not I2C, illumination or electrical power sequencing.

`simulator_eego_a4` models a frontlit unit. `simulator_eego_a4_no_frontlight`
models a missing frontlight chip: `present()` and color-temperature availability
are false, and the light cannot turn on. Both retain `FREEINK_CAP_FRONTLIGHT=1`,
as the same hardware firmware binary probes for the optional controller.
The board reports an I2C frontlight, not a PWM frontlight.

The SDK's nominal UI-scale metadata is 1.2. Current CrossPoint uses one fixed
UI-font tier in `src/components/UIScale.h`; this profile does not force a
different scale or change the firmware UI policy.

## SDK Home-Key Caveat

At the referenced SDK revision, `EEGO_A4.touch.hasHomeKey` is false even though
`InputManager::pollGslx680()` produces Home-key events for its special sentinel.
CrossPoint's `HalGPIO::hasHomeKey()` forwards that false capability, and
`MappedInputManager::update()` gates Home gesture handling on it.

The simulator deliberately reproduces both sides: H generates the driver's
events, but `gpio.hasHomeKey()` remains false. It does not silently fix the
firmware/SDK contract or promise that H performs Back/Home in the application.
A future SDK correction should be mirrored here and covered by the input test.

## Verification

From the simulator repository:

```sh
python3 tests/check-profile-selection.py
sh tests/run-eego.sh
sh tests/run-metalio.sh
```

The profile test checks all eight boards and rejects mixed devices,
incompatible controllers and a lightless flag without EEGO. Native SDL tests
exercise both light variants, the framebuffer size and margins, button
availability, screen-key events, clock/tilt capabilities, frontlight state,
edge-coordinate taps and swipes in all four orientations. Metalio is rechecked
because it shares the input and frontlight paths.

Use either sample PlatformIO configuration to add the two environments to a
firmware checkout. Simulator evidence is not physical e-paper, touch firmware,
RTC, frontlight-probe or ESP32 memory validation.

### Application Build Limit

The current `develop-lpla` application build was attempted at `792e4abbb`.
This PR adds the missing `BatteryMonitor::loadDesignCapacity()` host shim
needed by recent upstream firmware; it returns false because no hardware
gauge work is pending on the host.

The full application did not build. Unrelated compatibility blockers remain:
`TrustedTime` calls undeclared `settimeofday`, the protected-book headers
redefine `ContentDecryptor`, and the integrated GIF branch requires
`StaticSemaphore_t`, which the host FreeRTOS shim does not provide yet.
A local, uncommitted clock fixture that refuses host clock changes exposed
the latter two errors. No application screenshot or end-to-end reading test
is claimed. The native SDL input tests above run without that fixture.
