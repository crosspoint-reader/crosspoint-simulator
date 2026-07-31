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
FIRMWARE_DIR="${CROSSPOINT_FIRMWARE_DIR:-$HOME/crosspoint/crosspoint-reader}"

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

[[ -f "$ASC_KEY_PATH" ]] || { echo "ERROR: no ASC key at $ASC_KEY_PATH"; exit 1; }

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
cmake -B "$BUILD_DIR" -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCROSSPOINT_FIRMWARE_DIR="$FIRMWARE_DIR" \
  -DCROSSPOINT_BUILD_FIRMWARE=ON \
  -DCROSSPOINT_IOS_TEAM_ID="$TEAM_ID" \
  -DCROSSPOINT_IOS_MARKETING_VERSION="$MARKETING_VERSION" \
  -DCROSSPOINT_IOS_BUILD_NUMBER="$BUILD_NUMBER" >/dev/null

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

[[ -f "$IPA" ]] || { echo "ERROR: no IPA at $IPA"; ls -la "$EXPORT_DIR" || true; exit 1; }
echo "IPA: $IPA ($(du -h "$IPA" | cut -f1))"

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
  exit $RC
fi

# Tag the build so the next run's build number picks up from here.
git -C "$REPO" tag "build-$BUILD_NUMBER" 2>/dev/null \
  && echo "tagged build-$BUILD_NUMBER"

say "Uploaded"
echo "Apple takes roughly 5-10 minutes to process before the build appears in"
echo "the TestFlight app."
