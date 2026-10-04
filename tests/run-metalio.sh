#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/sim-metalio.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT
c++ -std=c++20 -Wall -Wextra -Wno-unused-parameter -DSIMULATOR_DEVICE_METALIO_EINK4 \
  -Itests/native-stubs -Isrc $(sdl2-config --cflags) \
  tests/metalio-input.cpp src/HalGPIO.cpp src/HalClock.cpp src/HalTiltSensor.cpp \
  src/HalFrontlight.cpp $(sdl2-config --libs) -o "$build_dir/input"
"$build_dir/input"
