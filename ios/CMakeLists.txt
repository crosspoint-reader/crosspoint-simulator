# iOS app targets: the peripheral-impersonator harness plus the real firmware.
#
# main() comes from the firmware side (src/simulator_main.cpp, inside the core
# library); these targets supply only the gesture harness that simulator_main
# calls into. Rendering belongs to HalDisplay, exactly as on desktop -- the
# harness draws nothing.
#
# Two apps are built from one source set:
#   CrossPointX3   the reader, links crosspoint_core
#   CrossPointZen  the-device-is-the-book, links crosspoint_core_zen
# They differ only in which core they link and their bundle identity. The zen
# behaviour lives in firmware TUs compiled into the core, so it CANNOT be
# switched on per app target -- see CROSSPOINT_BUILD_ZEN in the root
# CMakeLists.txt.

set(CROSSPOINT_IOS_BUNDLE_ID "com.natebunnyfield.crosspoint.x3" CACHE STRING
  "iOS bundle identifier")
set(CROSSPOINT_IOS_ZEN_BUNDLE_ID "com.natebunnyfield.crosspoint.zen" CACHE STRING
  "iOS bundle identifier for the zen app")
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

# The asset catalog carries the app icons. TestFlight validation rejects an
# upload without one, so it is part of the target rather than an afterthought.
# Both apps include the same catalog and select their own icon set by name.
set(CROSSPOINT_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/Assets.xcassets)
set_source_files_properties(${CROSSPOINT_ASSETS} PROPERTIES
  MACOSX_PACKAGE_LOCATION Resources)

# Optional bundled font families. Point this at a build-sd-fonts.py output
# directory (one subdirectory per family, .cpfont files inside) and each family
# ships inside the app under Resources/SeedFonts/<Family>/. At launch,
# CrossPointFsPrep copies them into the emulated SD card's visible "fonts/"
# root, so the phone gets font updates with the app instead of by hand through
# the Files app. Globbed at configure time — CI configures fresh every run, so
# a stale glob only affects incremental local builds.
set(CROSSPOINT_IOS_SEED_FONTS_DIR "" CACHE PATH
  "Directory of .cpfont font families to bundle and seed at launch")

# Default books, always bundled (small public-domain testers checked into
# ios/seedbooks/). CrossPointFsPrep copies each into books/ ONLY when absent —
# books are user data, so unlike fonts they are never overwritten or pruned.
file(GLOB _seed_books "${CMAKE_CURRENT_SOURCE_DIR}/seedbooks/*.epub")
if(_seed_books)
  set_source_files_properties(${_seed_books} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources/SeedBooks")
endif()

if(NOT CROSSPOINT_BUILD_FIRMWARE)
  message(FATAL_ERROR
    "The iOS app needs the firmware source set. Configure with "
    "-DCROSSPOINT_BUILD_FIRMWARE=ON -DCROSSPOINT_FIRMWARE_DIR=<path>.")
endif()

# Build one app bundle. Everything that differs between the reader and zen is a
# parameter; everything else is shared so the two cannot drift apart.
function(crosspoint_add_ios_app _target _core _bundle_id _display_name _icon)
  set(CROSSPOINT_IOS_DISPLAY_NAME "${_display_name}")
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    ${CMAKE_CURRENT_BINARY_DIR}/${_target}-Info.plist
    @ONLY)

  add_executable(${_target} MACOSX_BUNDLE
    ${CMAKE_CURRENT_SOURCE_DIR}/CrossPointIOSShim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PadCore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CrossPointFsPrep.cpp
    ${CROSSPOINT_ASSETS})

  if(CROSSPOINT_IOS_SEED_FONTS_DIR)
    file(GLOB _seed_families RELATIVE "${CROSSPOINT_IOS_SEED_FONTS_DIR}"
      "${CROSSPOINT_IOS_SEED_FONTS_DIR}/*")
    set(_seed_count 0)
    foreach(_family IN LISTS _seed_families)
      set(_family_dir "${CROSSPOINT_IOS_SEED_FONTS_DIR}/${_family}")
      if(IS_DIRECTORY "${_family_dir}")
        file(GLOB _family_fonts "${_family_dir}/*.cpfont")
        if(_family_fonts)
          set_source_files_properties(${_family_fonts} PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/SeedFonts/${_family}")
          target_sources(${_target} PRIVATE ${_family_fonts})
          list(LENGTH _family_fonts _n)
          math(EXPR _seed_count "${_seed_count} + ${_n}")
          message(STATUS "${_target} seed fonts: ${_family} (${_n} files)")
        endif()
      endif()
    endforeach()
    if(_seed_count EQUAL 0)
      message(FATAL_ERROR
        "CROSSPOINT_IOS_SEED_FONTS_DIR=${CROSSPOINT_IOS_SEED_FONTS_DIR} "
        "contains no <Family>/*.cpfont files — wrong path, or the font build "
        "did not run.")
    endif()
  endif()

  if(_seed_books)
    target_sources(${_target} PRIVATE ${_seed_books})
    list(LENGTH _seed_books _n)
    message(STATUS "${_target} seed books: ${_n}")
  endif()

  target_link_libraries(${_target} PRIVATE ${_core} SDL3::SDL3)

  target_compile_definitions(${_target} PRIVATE
    # The harness impersonates an X3. Kept consistent with the desktop env so the
    # two builds cannot silently disagree about which board they are simulating.
    SIMULATOR
    SIMULATOR_DEVICE_X3)

  set_target_properties(${_target} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/${_target}-Info.plist
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${_bundle_id}
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "17.0"
    # 1 = iPhone only. The panel and control rows are laid out for a phone in
    # portrait; iPad would just letterbox.
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1"
    XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "${_icon}"
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
    set_target_properties(${_target} PROPERTIES
      XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ${CROSSPOINT_IOS_TEAM_ID}
      XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic")
  else()
    # Simulator builds need no signature; forcing this off keeps a machine with
    # multiple identities from picking one at random.
    set_target_properties(${_target} PROPERTIES
      XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
      XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")
  endif()
endfunction()

crosspoint_add_ios_app(CrossPointX3 crosspoint_core
  ${CROSSPOINT_IOS_BUNDLE_ID} "CrossPoint X3" "AppIcon")

if(CROSSPOINT_BUILD_ZEN)
  crosspoint_add_ios_app(CrossPointZen crosspoint_core_zen
    ${CROSSPOINT_IOS_ZEN_BUNDLE_ID} "CrossPoint Zen" "AppIconZen")
endif()
