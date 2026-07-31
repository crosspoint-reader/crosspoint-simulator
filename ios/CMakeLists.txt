# iOS app target: the peripheral-impersonator harness plus the real firmware.
#
# main() comes from the firmware side (src/simulator_main.cpp, inside
# crosspoint_core); this target supplies only the gesture harness that
# simulator_main calls into. Rendering belongs to HalDisplay, exactly as on
# desktop -- the harness draws nothing.

set(CROSSPOINT_IOS_BUNDLE_ID "com.natebunnyfield.crosspoint.x3" CACHE STRING
  "iOS bundle identifier")
set(CROSSPOINT_IOS_TEAM_ID "" CACHE STRING
  "Apple Developer Team ID. Required for device builds; leave empty for the Simulator.")

# Version and build number, driven by ios/testflight.sh.
#
# CFBundleVersion must be unique for a given marketing version, so every upload
# needs a new one. TestFlight's daily upload cap (error 90382) is also scoped per
# marketing version, so the answer to hitting it is bumping the marketing
# version, not waiting for the next day.
set(CROSSPOINT_IOS_MARKETING_VERSION "0.1.0" CACHE STRING
  "CFBundleShortVersionString")
set(CROSSPOINT_IOS_BUILD_NUMBER "1" CACHE STRING
  "CFBundleVersion; must be unique per marketing version")

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
  ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
  @ONLY)

# The asset catalog carries the app icon. TestFlight validation rejects an
# upload without one, so it is part of the target rather than an afterthought.
set(CROSSPOINT_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/Assets.xcassets)
set_source_files_properties(${CROSSPOINT_ASSETS} PROPERTIES
  MACOSX_PACKAGE_LOCATION Resources)

add_executable(CrossPointX3 MACOSX_BUNDLE
  ${CMAKE_CURRENT_SOURCE_DIR}/CrossPointIOSShim.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/CrossPointFsPrep.cpp
  ${CROSSPOINT_ASSETS})

if(NOT CROSSPOINT_BUILD_FIRMWARE)
  message(FATAL_ERROR
    "The iOS app needs the firmware source set. Configure with "
    "-DCROSSPOINT_BUILD_FIRMWARE=ON -DCROSSPOINT_FIRMWARE_DIR=<path>.")
endif()

target_link_libraries(CrossPointX3 PRIVATE crosspoint_core SDL3::SDL3)

target_compile_definitions(CrossPointX3 PRIVATE
  # The harness impersonates an X3. Kept consistent with the desktop env so the
  # two builds cannot silently disagree about which board they are simulating.
  SIMULATOR
  SIMULATOR_DEVICE_X3)

set_target_properties(CrossPointX3 PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${CROSSPOINT_IOS_BUNDLE_ID}
  XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "17.0"
  # 1 = iPhone only. The panel and control rows are laid out for a phone in
  # portrait; iPad would just letterbox.
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1"
  XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
  XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
  # Required for `xcodebuild archive` to produce a DISTRIBUTABLE archive.
  #
  # CMake generates Xcode targets with SKIP_INSTALL=YES, so archive builds the
  # app but never installs it into the archive: Products/ comes out empty and
  # the archive Info.plist has no ApplicationProperties. Xcode then finds no
  # valid distribution methods and -exportArchive fails with the misleading
  # `exportOptionsPlist error for key "method" expected one {}` — an EMPTY set
  # of methods, not a bad method string.
  XCODE_ATTRIBUTE_SKIP_INSTALL "NO"
  XCODE_ATTRIBUTE_INSTALL_PATH "$(LOCAL_APPS_DIR)"
  # A release build for distribution should not carry the simulator's debug
  # assertions; the firmware is noisy enough on a 64-bit host as it is.
  XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH[variant=Release] "NO")

if(CROSSPOINT_IOS_TEAM_ID)
  set_target_properties(CrossPointX3 PROPERTIES
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ${CROSSPOINT_IOS_TEAM_ID}
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic")
else()
  # Simulator builds need no signature; forcing this off keeps a machine with
  # multiple identities from picking one at random.
  set_target_properties(CrossPointX3 PROPERTIES
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")
endif()
