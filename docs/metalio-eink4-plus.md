# Metalio E-Ink 4 Plus simulator profile

Build the firmware with `pio run -e simulator_metalio_eink4_plus` using one of the sample PlatformIO configurations. This profile provides the desktop simulator for CrossPoint's `metalio_eink4_plus` environment from the `feat-metalio-eink4plus` branch. It models an ESP32-S31 on `board = esp32-s31-coreboard-1` with pioarduino 61.04.00-RC1 and `-DFREEINK_DEVICE_METALIO_EINK4_PLUS=1`.

## Hardware specifications

The specifications match `freeink-sdk/docs/metalio-eink4-plus-support.md` and `CloudZao/Metalio-E-INK4-Plus`:

- The display is a 4.7-inch 1216x684 ED047TC2 / E0470A14-AF raw parallel panel (684x1216 in portrait, `uiScale = 1.5`) driven by EPDiy and a TPS65185 PMIC.
- Viewable insets are 2px top, 2px right, 12px bottom, and 2px left (`{2, 2, 12, 2}`).
- The touch controller is an FT6336U on I2C address 0x38, with an interrupt on GPIO 5 and a reset line on IO expander pin P1.2. It reports taps, swipes, and bezel buttons (Home, Prev, and Next).
- The frontlight uses dual-channel LEDC pulse-width modulation with cool white on GPIO 6 and warm white on GPIO 7.
- Physical buttons include BOOT on GPIO 61 for confirm, and volume up, volume down, and power keys on TCA9555 expander pins P1.0, P1.1, and P1.7.

## Source contract

| SDK property | Simulator behavior |
| --- | --- |
| `MetalioEInk4Plus`, `metalio_eink4_plus` | Sets matching board identity and firmware device flag |
| EPDiy, 1216 by 684 (684 by 1216 portrait) | Allocates 103,968-byte monochrome framebuffer (`1216 / 8 * 684`); ignores Xteink controller overrides |
| FT6336U touch | Translates mouse and scripted touch through the active orientation transform |
| BOOT, power, volume, and cover keys | Maps Return, P, Up/Down, Page Up/Page Down, and H for the cover Home key |
| No Back/Left/Right button contacts | Ignores Escape and Left/Right keyboard and scripted actions |
| Warm and cool frontlight | Provides frontlight controls with brightness and warmth sliders (`FREEINK_CAP_WARMLIGHT = 1`) |
| PCF8563 RTC, SC7A20H accelerometer, haptics | Backs clock with host system time, enables tilt sensor, and stubs haptics as no-ops |
| ESP32-S31 with 16 MB flash and PSRAM | Sets configuration flags; host binary does not emulate physical PSRAM |

## Scope and verification

This profile implements the HAL classes and board configuration for the desktop simulator. It does not emulate raw I2C transactions, expander sequencing, audio, battery ADC curves, USB mass storage, cellular modem, power rails, or display waveform timings.

### Verification checklist

1. Touch taps and swipes respond in all four screen rotations.
2. Return, Up, and Down navigate menus; Page Up and Page Down trigger cover page turns; Escape, Left, and Right produce no action.
3. H tap and hold trigger Home actions.
4. Frontlight controls show brightness and warmth sliders.
5. Settings > About displays `metalio_eink4_plus` and `EPDiy`.

To run the verification suite:

```sh
python3 tests/check-profile-selection.py
sh tests/run-metalio.sh
```
