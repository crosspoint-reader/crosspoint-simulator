#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/sim-picco.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT
for variant in normal ndebug; do
  variant_flag=""
  if [ "$variant" = ndebug ]; then
    variant_flag="-DNDEBUG"
  fi
  c++ -std=c++20 -Wall -Wextra -Wno-unused-parameter \
    -DSIMULATOR_DEVICE_PICCO $variant_flag \
    -Itests/native-stubs -Isrc $(sdl2-config --cflags) \
    tests/picco-input.cpp src/HalGPIO.cpp src/HalClock.cpp src/HalTiltSensor.cpp \
    src/HalFrontlight.cpp src/HalPowerManager.cpp src/SimulatorLifecycle.cpp \
    src/SimLog.cpp tests/native-stubs/Screenshot.cpp \
    $(sdl2-config --libs) -o "$build_dir/$variant"
  "$build_dir/$variant"
  "$build_dir/$variant" held
  c++ -std=c++20 -Wall -Wextra -pthread $variant_flag -Isrc \
    tests/static-mutex.cpp -o "$build_dir/mutex-$variant"
  "$build_dir/mutex-$variant"
done
