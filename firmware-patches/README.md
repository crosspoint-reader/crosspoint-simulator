# firmware-patches/

Patches applied on top of the pinned upstream CrossPoint firmware commit
(`CROSSPOINT_FIRMWARE_PIN` in [../cmake/CrossPointSources.cmake](../cmake/CrossPointSources.cmake))
when CI builds the X3 iOS app. Upstream is not ours to push to; anything the
app needs beyond the pin is vendored here as `git format-patch` output and
applied in numeric order with `git apply` before the build.

Each patch must apply clean on the bare pin (`git apply --check`). When a
patch adds, removes, or renames a firmware translation unit, the generated
source list in `cmake/CrossPointSources.cmake` must agree with the *patched*
tree — regenerate it via `tools/gen_cmake_sources.py` if the two drift.
Generated firmware files (`lib/I18n/I18nKeys.h`, `I18nStrings.h`,
`I18nStrings.cpp`, `*.generated.h`) are never patched: CI regenerates them
from the patched sources (`scripts/gen_i18n.py`, `scripts/build_html.py`).

| Patch | What it does |
| --- | --- |
| `01-calendar-views.patch` | Adds the "Calendar Four/Five/Six" sleep-screen styles (4/5/6-week calendar grids, today highlighted) plus the `HalClock::getDateTime` accessor they need. Creates `src/activities/boot_sleep/CalendarSleepScreen.*` and `HolidayCalculator.*`, which the vendored CMake source list already expects. |
