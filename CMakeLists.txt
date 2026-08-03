# CrossPoint simulator -- CMake front end.
#
# This repo remains a PlatformIO library first: the desktop simulator is still
# built by the consuming firmware via `pio run -e simulator`, and that build is
# the canary. CMake exists so the same source set can be driven by a second
# toolchain (iOS) without forking the simulator.
#
# One source set, two toolchains. The source set is generated, never hand-edited
# -- see tools/gen_cmake_sources.py.
#
# Configure for the iOS Simulator (arm64 host):
#
#   cmake -B build/ios-sim -G Xcode \
#     -DCMAKE_SYSTEM_NAME=iOS \
#     -DCMAKE_OSX_SYSROOT=iphonesimulator \
#     -DCMAKE_OSX_ARCHITECTURES=arm64 \
#     -DCROSSPOINT_FIRMWARE_DIR=$HOME/src/crosspoint-reader
#   cmake --build build/ios-sim --config Debug -- -sdk iphonesimulator
#
# Configure for a device:
#
#   cmake -B build/ios-dev -G Xcode \
#     -DCMAKE_SYSTEM_NAME=iOS \
#     -DCMAKE_OSX_ARCHITECTURES=arm64 \
#     -DCROSSPOINT_FIRMWARE_DIR=$HOME/src/crosspoint-reader \
#     -DCROSSPOINT_IOS_TEAM_ID=<TEAMID>

cmake_minimum_required(VERSION 3.24)

# Deployment target. MUST precede project(): CMake bakes the target into the
# toolchain during project(), and setting it afterwards leaves try_compile and
# the real build disagreeing (a fresh configure then fails outright).
# It also has to be here rather than on the app target so it reaches as much
# of the build as possible; see the note below on what it does not reach. ios/CMakeLists.txt sets IPHONEOS_DEPLOYMENT_TARGET as
# an XCODE_ATTRIBUTE_ on the app target only, which does NOT propagate to the
# SDL3 subproject -- SDL and crosspoint_core were building at the SDK's own
# minimum (26.5) while the app advertised 17.0.
#
# That is not cosmetic: at minos 26.5 the compiler folds `if (@available(iOS
# 18.0, *))` to a constant true and suppresses -Wunguarded-availability, so a
# guard that should protect a 17.0 device compiles out. SDL already contains one
# such call (GCEventInteraction, iOS 18+, SDL_uikitvideo.m). At 26.0 that is
# simply available, which is the point of targeting 26+.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING
    "Minimum iOS version; must match ios/CMakeLists.txt's IPHONEOS_DEPLOYMENT_TARGET")
endif()
# Applies the target to every target the Xcode generator emits. SDL3 still sets
# its own, but both now land on 26.0 so nothing disagrees.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

project(crosspoint_simulator
  VERSION 1.0.0
  DESCRIPTION "Desktop/iOS simulator for CrossPoint e-reader firmware"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(CROSSPOINT_FIRMWARE_DIR "" CACHE PATH
  "Path to the CrossPoint firmware checkout (the repo that owns src/ and lib/)")

option(CROSSPOINT_BUILD_FIRMWARE
  "Compile the firmware + simulator HAL source set. OFF during milestone 1, \
which proves the toolchain and the input-injection model with the iOS shim only."
  OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CrossPointSources.cmake)

# ---------------------------------------------------------------------------
# Source-set validation.
#
# This is the drift detector. On a fork the firmware moves under you, so a
# generated list that no longer matches the tree must fail loudly at configure
# time rather than as a confusing compile error 140 TUs later.
# ---------------------------------------------------------------------------
function(crosspoint_validate_sources)
  set(missing "")

  foreach(src IN LISTS CROSSPOINT_SIM_SOURCES)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
      list(APPEND missing "simulator: ${src}")
    endif()
  endforeach()

  if(CROSSPOINT_FIRMWARE_DIR)
    foreach(src IN LISTS CROSSPOINT_FW_SOURCES)
      if(NOT EXISTS "${CROSSPOINT_FIRMWARE_DIR}/${src}")
        list(APPEND missing "firmware: ${src}")
      endif()
    endforeach()
  endif()

  list(LENGTH missing n_missing)
  if(n_missing GREATER 0)
    string(REPLACE ";" "\n  " pretty "${missing}")
    message(FATAL_ERROR
      "${n_missing} source file(s) in cmake/CrossPointSources.cmake no longer "
      "exist:\n  ${pretty}\n\n"
      "The generated source set has drifted from the tree. Regenerate it:\n"
      "  cd ${CROSSPOINT_FIRMWARE_DIR}\n"
      "  pio run -e simulator -t compiledb\n"
      "  python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_cmake_sources.py "
      "--firmware-dir . --compile-db compile_commands.json")
  endif()
endfunction()

crosspoint_validate_sources()

list(LENGTH CROSSPOINT_SIM_SOURCES _n_sim)
list(LENGTH CROSSPOINT_FW_SOURCES _n_fw)
message(STATUS "CrossPoint source set: ${_n_sim} simulator TUs, ${_n_fw} firmware TUs")
if(NOT CROSSPOINT_BUILD_FIRMWARE)
  message(STATUS
    "CrossPoint: CROSSPOINT_BUILD_FIRMWARE=OFF -- source set is validated but "
    "not compiled (milestone 1: iOS shim only)")
endif()

# ---------------------------------------------------------------------------
# The shared source set as a library target.
#
# Deliberately NOT built during milestone 1. Enabling it is milestone 2, and it
# is also the point at which the shared SDL call sites must move from SDL2 to
# SDL3 -- see ios/README.md.
# ---------------------------------------------------------------------------
# SDL3 rather than SDL2: SDL3.xcframework is a first-class target of SDL's own
# CMake build and the iOS backend is the better-maintained one. The desktop
# PlatformIO env was moved to SDL3 at the same time, so both toolchains compile
# one source set with no per-platform SDL shim.
include(FetchContent)
set(CROSSPOINT_SDL3_TAG "release-3.4.12" CACHE STRING
  "SDL3 git tag to build against")
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
# Disable SDL3 subsystems the app never calls. The reader only calls
# SDL_Init(SDL_INIT_VIDEO); audio, camera, Bluetooth game controllers, haptic,
# motion sensors, and the Vulkan render/GPU backends are all dead weight on iOS.
# Removing camera and Bluetooth here lets us remove their privacy strings from
# Info.plist.in (ITMS-90683 requires a purpose string for every referenced API,
# even one we never invoke). Vulkan on iOS is unsupported and adds nothing.
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
# HIDAPI on iOS uses CoreBluetooth for Bluetooth device enumeration; with
# SDL_JOYSTICK=OFF there is nothing left that would call into it.
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_GPU_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG ${CROSSPOINT_SDL3_TAG}
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(SDL3)

if(CROSSPOINT_BUILD_FIRMWARE)
  if(NOT CROSSPOINT_FIRMWARE_DIR)
    message(FATAL_ERROR
      "CROSSPOINT_BUILD_FIRMWARE=ON requires -DCROSSPOINT_FIRMWARE_DIR=<path>")
  endif()

  # On iOS the network/OTA/SD-flash subsystem cannot function (no WiFi
  # hardware, no SD slot). Strip those TUs before building crosspoint_core so
  # the iOS binary carries none of that code. The desktop build is unaffected.
  if(IOS)
    list(LENGTH CROSSPOINT_IOS_EXCLUDED_SIM_SOURCES _n_exc_sim)
    list(LENGTH CROSSPOINT_IOS_EXCLUDED_FW_SOURCES _n_exc_fw)
    list(REMOVE_ITEM CROSSPOINT_SIM_SOURCES ${CROSSPOINT_IOS_EXCLUDED_SIM_SOURCES})
    list(REMOVE_ITEM CROSSPOINT_FW_SOURCES ${CROSSPOINT_IOS_EXCLUDED_FW_SOURCES})
    message(STATUS "CrossPoint iOS: stripped ${_n_exc_sim} simulator + ${_n_exc_fw} firmware network/OTA TUs")
  endif()

  list(TRANSFORM CROSSPOINT_SIM_SOURCES
    PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/" OUTPUT_VARIABLE _sim_abs)
  list(TRANSFORM CROSSPOINT_FW_SOURCES
    PREPEND "${CROSSPOINT_FIRMWARE_DIR}/" OUTPUT_VARIABLE _fw_abs)
  list(TRANSFORM CROSSPOINT_FW_INCLUDE_DIRS
    PREPEND "${CROSSPOINT_FIRMWARE_DIR}/" OUTPUT_VARIABLE _fw_inc_abs)

  add_library(crosspoint_core STATIC ${_sim_abs} ${_fw_abs})
  target_include_directories(crosspoint_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/ios
    ${_fw_inc_abs})
  target_compile_definitions(crosspoint_core PUBLIC ${CROSSPOINT_DEFINES})
  # The panel is scaled up on a phone, so it must land on exact device pixels.
  target_compile_definitions(crosspoint_core PUBLIC CROSSPOINT_SIM_PIXEL_EXACT=1)
  # The firmware calls SDL only through the simulator HAL, but those TUs are in
  # this target, so it needs the headers and the library.
  target_link_libraries(crosspoint_core PUBLIC SDL3::SDL3)
  # Firmware sources were written for a 32-bit MCU and are noisy on a 64-bit
  # host; the desktop env silences the same class of warning.
  target_compile_options(crosspoint_core PRIVATE -Wno-c++11-narrowing)
endif()

if(IOS)
  add_subdirectory(ios)
endif()
