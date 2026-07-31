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

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
  ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
  @ONLY)

add_executable(CrossPointX3 MACOSX_BUNDLE
  ${CMAKE_CURRENT_SOURCE_DIR}/CrossPointIOSShim.cpp)

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
  # 1 = iPhone only. The panel + control zones are laid out for a phone in
  # portrait; iPad would just letterbox.
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1"
  XCODE_ATTRIBUTE_ENABLE_BITCODE "NO")

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
