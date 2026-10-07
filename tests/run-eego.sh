#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/sim-eego.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT
for variant in frontlit lightless; do
  variant_flag=""
  if [ "$variant" = lightless ]; then
    variant_flag="-DSIMULATOR_EEGO_NO_FRONTLIGHT"
  fi
  c++ -std=c++20 -Wall -Wextra -Wno-unused-parameter \
    -DSIMULATOR_DEVICE_EEGO_A4 $variant_flag \
    -Itests/native-stubs -Isrc $(sdl2-config --cflags) \
    tests/eego-input.cpp src/HalGPIO.cpp src/HalClock.cpp src/HalTiltSensor.cpp \
    src/HalFrontlight.cpp $(sdl2-config --libs) -o "$build_dir/$variant"
  "$build_dir/$variant"
done
