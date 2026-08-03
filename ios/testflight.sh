#!/usr/bin/env bash
#
# Archive, export and upload CrossPoint X3 to TestFlight.
#
#   ios/testflight.sh            archive + export + upload
#   ios/testflight.sh --no-upload    stop after producing the IPA
#
# Prerequisites, in the order they bite:
#
# 1. An App Store Connect *app record* for the bundle ID below. The ASC API
#    refuses to create one ("The resource 'apps' does not allow 'CREATE'"), so it
#    is a one-time manual step at https://appstoreconnect.apple.com → Apps → +.
#    Without it, altool fails with rc 19 "Cannot determine the Apple ID from
#    Bundle ID", which is the same code it returns for an expired paid-developer
#    agreement — check both before believing either.
# 2. An Apple Distribution certificate in the login keychain.
# 3. The App Store Connect API key at ASC_KEY_PATH.
#
# The bundle ID is already registered in the developer portal (id G42B2FV8A8).

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIRMWARE_DIR="${CROSSPOINT_FIRMWARE_DIR:-$HOME/src/crosspoint-reader}"

BUNDLE_ID="com.natebunnyfield.crosspoint.x3"
TEAM_ID="887M8FR447"
ASC_KEY_ID="92428LY4AJ"
ASC_ISSUER="69a6de73-c01e-47e3-e053-5b8c7c11a4d1"
ASC_KEY_PATH="$HOME/.appstoreconnect/private_keys/AuthKey_${ASC_KEY_ID}.p8"

BUILD_DIR="$REPO/build/ios-dev"
ARCHIVE="$REPO/build/CrossPointX3.xcarchive"
EXPORT_DIR="$REPO/build/export"
IPA="$EXPORT_DIR/CrossPointX3.ipa"

UPLOAD=1
[[ "${1:-}" == "--no-upload" ]] && UPLOAD=0

AUTH=(-allowProvisioningUpdates
      -authenticationKeyPath "$ASC_KEY_PATH"
      -authenticationKeyID "$ASC_KEY_ID"
      -authenticationKeyIssuerID "$ASC_ISSUER")

say() { printf '\n=== %s ===\n' "$1"; }

# Phone notification via ntfy.sh, same topic and pattern as crds-ios
# scripts/notify.sh. Delivery is best-effort and never fails the deploy.
NTFY_TOPIC="${CROSSPOINT_NTFY_TOPIC:-crds-ios-natebunnyfield-9k3m2p7v}"
notify() { # notify <priority> <tag> <title> <body>
  curl -s -m 10 \
    -H "Title: $3" -H "Priority: $1" -H "Tags: $2" -d "$4" \
    "https://ntfy.sh/$NTFY_TOPIC" >/dev/null 2>&1 || true
}

[[ -f "$ASC_KEY_PATH" ]] || { echo "ERROR: no ASC key at $ASC_KEY_PATH"; exit 1; }

# pio lives outside PATH in non-login shells (a direct wrapper invocation, an
# agent shell). The osascript -> Terminal route gets the login profile, which
# is why the same script finds it there. Resolve the standard install
# locations before declaring the canary red for the wrong reason.
if ! command -v pio >/dev/null; then
  for candidate in "$HOME/.platformio/penv/bin" "$HOME/.local/bin" \
                   /opt/homebrew/bin /usr/local/bin; do
    if [[ -x "$candidate/pio" ]]; then
      export PATH="$candidate:$PATH"
      echo "pio resolved at $candidate/pio"
      break
    fi
  done
fi
command -v pio >/dev/null || {
  echo "ERROR: pio not found on PATH or in known install locations."
  echo "  Install PlatformIO, or run via osascript ios/deploy.applescript"
  echo "  (Terminal's login shell carries your full PATH)."
  exit 1
}

say "Desktop canary"
# The desktop build is the canary: green desktop + red iOS means the harness is
# wrong, both red means the HAL drifted. Catch it here rather than 140 TUs into
# an archive.
( cd "$FIRMWARE_DIR" && pio run -e simulator >/dev/null ) \
  && echo "desktop OK" \
  || { echo "ERROR: desktop build is red — fix that before shipping."; exit 1; }

say "Version"
# Build number = highest existing build-N tag + 1, so re-uploads never collide.
# CFBundleVersion must be unique for a marketing version; Apple rejects a repeat
# outright.
LAST_BUILD=$(git -C "$REPO" tag --list 'build-*' \
             | sed 's/^build-//' | sort -n | tail -1)
BUILD_NUMBER=$(( ${LAST_BUILD:-0} + 1 ))

# Marketing version is bumped only on demand. TestFlight's daily upload cap
# (error 90382) is per marketing version, so that is the lever when it trips —
# waiting a day is the wrong fix.
MARKETING_VERSION="${CROSSPOINT_MARKETING_VERSION:-0.1.0}"
echo "version $MARKETING_VERSION, build $BUILD_NUMBER"

say "Configure"
# Optional bundled fonts: point CROSSPOINT_SEED_FONTS_DIR at a
# build-sd-fonts.py output directory to ship those families inside the app
# (they seed the phone's fonts/ folder at launch). CI builds its own; local
# deploys opt in explicitly. The ${VAR[@]+...} guard keeps set -u happy on
# macOS's bash 3.2 when the array is empty.
SEED_FONTS_ARGS=()
if [[ -n "${CROSSPOINT_SEED_FONTS_DIR:-}" ]]; then
  SEED_FONTS_ARGS=(-DCROSSPOINT_IOS_SEED_FONTS_DIR="$CROSSPOINT_SEED_FONTS_DIR")
elif [[ "${CROSSPOINT_ALLOW_NO_FONTS:-0}" != "1" ]]; then
  # Build 13 shipped with an empty Resources/SeedFonts and nobody noticed until
  # the archive was opened by hand: the app still launches, still renders, and
  # silently falls back to the built-in Noto faces. For a reading app whose
  # whole point is its two curated faces, that is a broken build that looks
  # fine. CMake only errors when the directory is SET and empty, so an UNSET
  # variable was the one path with no guard on it at all. This is that guard.
  echo "ERROR: CROSSPOINT_SEED_FONTS_DIR is not set — the app would ship with"
  echo "  no .cpfont families and fall back to built-in Noto."
  echo "  Point it at a build-sd-fonts.py output tree (<Family>/*.cpfont, plus"
  echo "  <Family>/2x/*.cpfont for the 2x render scale the iOS target uses)."
  echo "  Deliberately shipping without them: CROSSPOINT_ALLOW_NO_FONTS=1"
  exit 1
fi
cmake -B "$BUILD_DIR" -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCROSSPOINT_FIRMWARE_DIR="$FIRMWARE_DIR" \
  -DCROSSPOINT_BUILD_FIRMWARE=ON \
  -DCROSSPOINT_IOS_TEAM_ID="$TEAM_ID" \
  -DCROSSPOINT_IOS_MARKETING_VERSION="$MARKETING_VERSION" \
  -DCROSSPOINT_IOS_BUILD_NUMBER="$BUILD_NUMBER" \
  ${SEED_FONTS_ARGS[@]+"${SEED_FONTS_ARGS[@]}"} >/dev/null

say "Archive"
rm -rf "$ARCHIVE"
xcodebuild archive \
  -project "$BUILD_DIR/crosspoint_simulator.xcodeproj" \
  -scheme CrossPointX3 \
  -configuration Release \
  -destination 'generic/platform=iOS' \
  -archivePath "$ARCHIVE" \
  "${AUTH[@]}" | tail -5

say "Export IPA"
rm -rf "$EXPORT_DIR"
xcodebuild -exportArchive \
  -archivePath "$ARCHIVE" \
  -exportOptionsPlist "$REPO/ios/ExportOptions.plist" \
  -exportPath "$EXPORT_DIR" \
  "${AUTH[@]}" | tail -5

# Xcode names the IPA after CFBundleName, not the target, so it lands as
# "CrossPoint X3.ipa" -- with a space. Glob rather than assume the target name.
IPA=$(find "$EXPORT_DIR" -maxdepth 1 -name '*.ipa' | head -1)
[[ -n "$IPA" && -f "$IPA" ]] || {
  echo "ERROR: no .ipa in $EXPORT_DIR"; ls -la "$EXPORT_DIR" || true; exit 1; }
echo "IPA: $IPA ($(du -h "$IPA" | cut -f1))"

say "Verify purpose strings"
# Build 1 was rejected with ITMS-90683: the linked SDL3 library references
# camera and Bluetooth APIs, so App Store Connect demands purpose strings even
# though the app never calls them. Check the IPA that will actually be
# uploaded, not the source template, so a plist-processing regression cannot
# slip through. Cheap (<1s) next to a wasted upload and a burned build number.
PURPOSE_KEYS=(NSCameraUsageDescription
              NSBluetoothAlwaysUsageDescription
              NSBluetoothPeripheralUsageDescription)
IPA_PLIST_DIR=$(mktemp -d)
trap 'rm -rf "$IPA_PLIST_DIR"' EXIT
unzip -q "$IPA" 'Payload/*.app/Info.plist' -d "$IPA_PLIST_DIR"
IPA_PLIST=$(find "$IPA_PLIST_DIR" -name Info.plist | head -1)
[[ -n "$IPA_PLIST" ]] || { echo "ERROR: no Info.plist inside $IPA"; exit 1; }
# A purpose string is required IFF the shipped binary actually references the
# API. Demanding all three unconditionally was right while SDL3 was built with
# every subsystem on. Once SDL_CAMERA/AUDIO/JOYSTICK/HAPTIC/SENSOR were turned
# off (CMakeLists.txt), CoreBluetooth, AVFoundation, CoreHaptics and
# AudioToolbox stopped linking at all and the strings were correctly dropped
# from Info.plist.in -- at which point an unconditional check blocks a build
# ITMS-90683 would never have rejected. Verified on build 21: zero undefined
# refs to CBCentral/AVCapture/CBPeripheral.
#
# Derive the requirement from the binary instead. This still catches the
# original failure: re-enable an SDL subsystem, the framework returns, and the
# string is demanded again.
unzip -q -o "$IPA" 'Payload/*.app/CrossPointX3' -d "$IPA_PLIST_DIR" 2>/dev/null || true
IPA_BIN=$(find "$IPA_PLIST_DIR/Payload" -maxdepth 2 -type f -name 'CrossPointX3' | head -1)
NEEDS_CAMERA=0
NEEDS_BT=0
if [[ -n "$IPA_BIN" ]]; then
  LINKED=$(otool -L "$IPA_BIN" 2>/dev/null || true)
  CLASSES=$(otool -v -s __TEXT __objc_classname "$IPA_BIN" 2>/dev/null || true)
  grep -q 'AVFoundation' <<<"$LINKED" && NEEDS_CAMERA=1
  grep -qiE 'AVCapture|SDLCamera|CaptureVideoData' <<<"$CLASSES" && NEEDS_CAMERA=1
  grep -q 'CoreBluetooth' <<<"$LINKED" && NEEDS_BT=1
  grep -qiE 'CBCentral|CBPeripheral' <<<"$CLASSES" && NEEDS_BT=1
else
  # Could not inspect the binary -- demand everything rather than silently
  # skipping the check.
  echo "  WARNING: no binary found inside the IPA; requiring all purpose strings"
  NEEDS_CAMERA=1
  NEEDS_BT=1
fi

MISSING=0
for key in "${PURPOSE_KEYS[@]}"; do
  case "$key" in
    NSCameraUsageDescription) REQUIRED=$NEEDS_CAMERA ;;
    NSBluetooth*) REQUIRED=$NEEDS_BT ;;
    *) REQUIRED=1 ;;
  esac
  VALUE=$(plutil -extract "$key" raw -o - "$IPA_PLIST" 2>/dev/null) || VALUE=""
  if [[ -n "$VALUE" ]]; then
    echo "  $key ok"
  elif [[ $REQUIRED -eq 1 ]]; then
    echo "  $key MISSING (binary references the API)"
    MISSING=1
  else
    echo "  $key not needed (API not referenced by the binary)"
  fi
done
if [[ $MISSING -ne 0 ]]; then
  echo "ERROR: the IPA is missing purpose strings for APIs it actually"
  echo "references, and App Store Connect will reject it with ITMS-90683."
  echo "Fix ios/Info.plist.in and rebuild."
  exit 1
fi

if [[ $UPLOAD -eq 0 ]]; then
  say "Stopping before upload (--no-upload)"
  exit 0
fi

say "Upload to TestFlight"
set +e
OUT=$(xcrun altool --upload-app -f "$IPA" -t ios \
        --apiKey "$ASC_KEY_ID" --apiIssuer "$ASC_ISSUER" 2>&1)
RC=$?
set -e
echo "$OUT"

if [[ $RC -ne 0 ]]; then
  echo
  echo "ERROR: upload failed (rc=$RC)."
  if [[ $RC -eq 19 ]] || echo "$OUT" | grep -q "Apple ID"; then
    echo "  rc 19 has two common causes and altool does not distinguish them:"
    echo "    a) no App Store Connect app record for $BUNDLE_ID — create it at"
    echo "       https://appstoreconnect.apple.com → Apps → + → New App"
    echo "    b) a paid-developer agreement needs re-accepting (the API reports"
    echo "       403 FORBIDDEN.REQUIRED_AGREEMENTS_MISSING_OR_EXPIRED)"
  fi
  if echo "$OUT" | grep -q "90382"; then
    echo "  Error 90382 is the daily upload cap, and it is scoped PER MARKETING"
    echo "  VERSION — do not wait a day. Re-run with a bumped version:"
    echo "      CROSSPOINT_MARKETING_VERSION=$(echo "$MARKETING_VERSION" |
            awk -F. '{printf "%d.%d.%d", $1, $2, $3+1}') $0"
  fi
  notify 4 warning "CrossPoint X3 upload FAILED (rc=$RC)" \
    "$(echo "$OUT" | grep -m2 -i 'error\|ITMS' || echo 'see terminal for details')"
  exit $RC
fi

# Tag the build so the next run's build number picks up from here, and push the
# tag so remote observers (agents, other machines) can see the upload happened.
git -C "$REPO" tag "build-$BUILD_NUMBER" 2>/dev/null \
  && echo "tagged build-$BUILD_NUMBER"
git -C "$REPO" push origin "build-$BUILD_NUMBER" 2>/dev/null \
  && echo "pushed build-$BUILD_NUMBER" \
  || echo "tag push failed (non-fatal) — push it later: git push origin build-$BUILD_NUMBER"

say "Uploaded"
echo "Apple takes roughly 5-10 minutes to process before the build appears in"
echo "the TestFlight app."
notify 4 rocket "CrossPoint X3 $MARKETING_VERSION ($BUILD_NUMBER) uploaded" \
  "TestFlight processing takes ~5-10 min, then it appears in the TestFlight app."
