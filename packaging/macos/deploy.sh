#!/usr/bin/env bash
# deploy.sh -- build, sign, and upload a CrossPoint TestFlight build.
#
# Run this ON the Mac, from a Terminal.app session (see deploy.applescript for
# why). Fire it from the consuming firmware repo:
#
#   ./lib-or-libdeps-path/packaging/macos/deploy.sh
#
# Configuration comes from the environment so no secrets live in the repo:
#
#   PIO_ENV           PlatformIO env to build          (default: simulator_x3)
#   DEVICE            packaging device profile         (default: derived)
#   MARKETING_VERSION CFBundleShortVersionString       (default: 0.1.0)
#   BUILD_NUMBER      CFBundleVersion, must be unique  (default: auto-bump)
#   BUNDLE_ID         must match App Store Connect     (required)
#   SIGN_APP          "Apple Distribution: ..."        (required)
#   SIGN_INSTALLER    "3rd Party Mac Developer Installer: ..."  (required)
#   PROVISIONING_PROFILE  path to a Mac App Store .provisionprofile for
#                     BUNDLE_ID; required for a store upload
#   ASC_KEY_ID        App Store Connect API key id     (required to upload)
#   ASC_ISSUER        App Store Connect issuer id      (required to upload)
#   ASC_KEY_PATH      path to the .p8 private key      (required to upload)
#   DRY_RUN=1         print every command, run nothing
#   SKIP_UPLOAD=1     build and sign, stop before altool
#
# The gate ordering and several specific checks are ported from the crds-ios
# deploy pipeline, which learned them the expensive way:
#
#   D.2  build-number drift -> Apple silently rejects a duplicate build number.
#        CrossPoint X3 build 1 is already consumed by the ITMS-90683 rejection,
#        so a rebuild MUST go up. This script auto-bumps from the last
#        macos-build-N tag rather than trusting a hand-typed number.
#   D.7  deploy lock -- two concurrent deploys corrupt each other's archive.
#   D.12 codesign needs the login keychain, which only a GUI Terminal session
#        has. Running this from a sandboxed agent shell fails with
#        errSecInternalComponent.
#   L4   TestFlight enforces a per-marketing-version daily upload cap
#        (error 90382). Bump MARKETING_VERSION to reset it.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGER="$HERE/package_macos_app.py"
ENTITLEMENTS="${ENTITLEMENTS:-$HERE/CrossPoint.entitlements}"

PIO_ENV="${PIO_ENV:-simulator_x3}"
MARKETING_VERSION="${MARKETING_VERSION:-0.1.0}"
DRY_RUN="${DRY_RUN:-0}"
SKIP_UPLOAD="${SKIP_UPLOAD:-0}"
OUTPUT_DIR="${OUTPUT_DIR:-dist}"
LOCK_DIR="${TMPDIR:-/tmp}/crosspoint-deploy.lock"

case "$PIO_ENV" in
  *x4_pro|*x4-pro) DEFAULT_DEVICE="x4-pro" ;;
  *x3)             DEFAULT_DEVICE="x3" ;;
  *)               DEFAULT_DEVICE="x4" ;;
esac
DEVICE="${DEVICE:-$DEFAULT_DEVICE}"

say()  { printf '\n==== %s ====\n' "$*"; }
run()  {
  printf '+ %s\n' "$*"
  if [ "$DRY_RUN" != "1" ]; then "$@"; fi
}
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

require() {
  local missing=()
  for name in "$@"; do
    [ -n "${!name:-}" ] || missing+=("$name")
  done
  [ ${#missing[@]} -eq 0 ] || die "missing required environment: ${missing[*]}"
}

# --- preflight -------------------------------------------------------------

say "preflight"

[ "$(uname -s)" = "Darwin" ] || die "this script only runs on macOS"
[ -f "$PACKAGER" ] || die "packager not found at $PACKAGER"
[ -f "$ENTITLEMENTS" ] || die "entitlements not found at $ENTITLEMENTS"
command -v pio >/dev/null || die "pio not on PATH"
command -v xcrun >/dev/null || die "xcrun not on PATH (install Xcode command line tools)"

require BUNDLE_ID SIGN_APP SIGN_INSTALLER
if [ "$SKIP_UPLOAD" != "1" ]; then
  require ASC_KEY_ID ASC_ISSUER ASC_KEY_PATH
  [ -f "$ASC_KEY_PATH" ] || die "ASC_KEY_PATH does not exist: $ASC_KEY_PATH"
fi

# D.12: codesign silently needs the GUI login keychain. Fail loudly and early
# with the fix, instead of 3 minutes deep in the archive.
if ! security show-keychain-info login.keychain-db >/dev/null 2>&1; then
  cat >&2 <<'MSG'
error: the login keychain is locked or unreachable.

codesign will fail with errSecInternalComponent. Fixes, in order of preference:
  1. Run this from Terminal.app on an unlocked Mac:
       osascript packaging/macos/deploy.applescript
  2. Unlock it by hand:
       security unlock-keychain login.keychain-db
MSG
  exit 1
fi

# D.7: one deploy at a time.
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  if [ -f "$LOCK_DIR/pid" ] && ! kill -0 "$(cat "$LOCK_DIR/pid")" 2>/dev/null; then
    echo "clearing stale deploy lock from dead pid $(cat "$LOCK_DIR/pid")"
    rm -rf "$LOCK_DIR"
    mkdir "$LOCK_DIR"
  else
    die "another deploy holds $LOCK_DIR (pid $(cat "$LOCK_DIR/pid" 2>/dev/null || echo unknown))"
  fi
fi
echo $$ > "$LOCK_DIR/pid"
trap 'rm -rf "$LOCK_DIR"' EXIT

# D.2: never reuse a build number. Derive from the last macos-build-N tag.
if [ -z "${BUILD_NUMBER:-}" ]; then
  LAST=$(git tag --list 'macos-build-*' 2>/dev/null \
         | sed 's/^macos-build-//' | sort -n | tail -1)
  # Build 1 is already consumed by the rejected upload, so the floor is 2.
  BUILD_NUMBER=$(( ${LAST:-1} + 1 ))
  echo "auto-bumped BUILD_NUMBER to $BUILD_NUMBER (last tag: ${LAST:-none})"
fi
echo "version $MARKETING_VERSION build $BUILD_NUMBER, device $DEVICE, env $PIO_ENV"

# --- build -----------------------------------------------------------------

say "build"
run pio run -e "$PIO_ENV"
BINARY=".pio/build/$PIO_ENV/program"
[ "$DRY_RUN" = "1" ] || [ -f "$BINARY" ] || die "expected binary at $BINARY"

say "assemble bundle"
APP="$OUTPUT_DIR/$(python3 - "$DEVICE" <<'PY'
import sys
names = {"x3": "CrossPointX3", "x4": "CrossPointX4", "x4-pro": "CrossPointX4Pro"}
print(names[sys.argv[1]])
PY
).app"
run python3 "$PACKAGER" build \
  --binary "$BINARY" \
  --device "$DEVICE" \
  --version "$MARKETING_VERSION" \
  --build "$BUILD_NUMBER" \
  --bundle-id "$BUNDLE_ID" \
  --output-dir "$OUTPUT_DIR"

# The whole point of this pipeline: never upload a bundle that will bounce on
# ITMS-90683 again.
say "verify purpose strings"
run python3 "$PACKAGER" verify "$APP"

# --- embed dylibs ----------------------------------------------------------
#
# The App Store rejects a binary that references /opt/homebrew or /usr/local.
# Copy every non-system dylib into Contents/Frameworks and repoint the
# executable at @rpath.

say "embed dylibs"
if [ "$DRY_RUN" != "1" ]; then
  MACOS_DIR="$APP/Contents/MacOS"
  EXE="$MACOS_DIR/$(basename "$APP" .app)"
  FRAMEWORKS="$APP/Contents/Frameworks"
  mkdir -p "$FRAMEWORKS"
  otool -L "$EXE" | awk 'NR>1 {print $1}' | while read -r lib; do
    case "$lib" in
      /usr/lib/*|/System/*|@*) continue ;;
    esac
    base="$(basename "$lib")"
    echo "+ embedding $lib"
    cp -f "$lib" "$FRAMEWORKS/$base"
    chmod u+w "$FRAMEWORKS/$base"
    install_name_tool -id "@rpath/$base" "$FRAMEWORKS/$base"
    install_name_tool -change "$lib" "@rpath/$base" "$EXE"
  done
  install_name_tool -add_rpath "@executable_path/../Frameworks" "$EXE" 2>/dev/null || true
  echo "remaining external references:"
  otool -L "$EXE" | awk 'NR>1 {print "  " $1}'
fi

# --- provisioning profile --------------------------------------------------
#
# Must happen before signing -- the profile is sealed into the signature. Xcode
# does this during -exportArchive; a hand-assembled bundle has to do it
# explicitly. App Store Connect rejects a store build with no embedded profile
# even though codesign succeeds locally.

say "provisioning profile"
if [ -n "${PROVISIONING_PROFILE:-}" ]; then
  [ -f "$PROVISIONING_PROFILE" ] || die "PROVISIONING_PROFILE not found: $PROVISIONING_PROFILE"

  # A profile whose App ID does not match BUNDLE_ID signs fine locally and is
  # only rejected at upload, after a full build. Catch it in a second here.
  # Parsing is best-effort: warn if the profile cannot be read, but fail hard
  # on a definite mismatch.
  if [ "$DRY_RUN" != "1" ]; then
    PROFILE_PLIST="$(mktemp -t crosspoint-profile)"
    if security cms -D -i "$PROVISIONING_PROFILE" > "$PROFILE_PLIST" 2>/dev/null; then
      PROFILE_APP_ID=""
      for key in ':Entitlements:com.apple.application-identifier' \
                 ':Entitlements:application-identifier'; do
        PROFILE_APP_ID=$(/usr/libexec/PlistBuddy -c "Print $key" "$PROFILE_PLIST" 2>/dev/null) && break
      done
      if [ -n "$PROFILE_APP_ID" ]; then
        # Stored as TEAMID.bundle.id; a wildcard profile ends in ".*".
        PROFILE_BUNDLE_ID="${PROFILE_APP_ID#*.}"
        case "$BUNDLE_ID" in
          ${PROFILE_BUNDLE_ID}) echo "profile matches $BUNDLE_ID" ;;
          *) die "profile is for '$PROFILE_BUNDLE_ID' but BUNDLE_ID is '$BUNDLE_ID'. Create a Mac App Store profile for $BUNDLE_ID, or fix BUNDLE_ID." ;;
        esac
      else
        echo "warning: could not read an application-identifier from the profile; skipping the match check."
      fi
    else
      echo "warning: could not decode $PROVISIONING_PROFILE; skipping the match check."
    fi
    rm -f "$PROFILE_PLIST"
  fi

  run cp "$PROVISIONING_PROFILE" "$APP/Contents/embedded.provisionprofile"
else
  echo "warning: PROVISIONING_PROFILE not set. Mac App Store uploads normally"
  echo "require an embedded profile; expect a rejection if this is a store build."
fi

# --- sign ------------------------------------------------------------------
#
# Inside-out: every nested dylib first, then the bundle. Signing the outer
# bundle first invalidates it as soon as a nested item is signed.

say "sign"
if [ "$DRY_RUN" != "1" ] && [ -d "$APP/Contents/Frameworks" ]; then
  find "$APP/Contents/Frameworks" -type f -name '*.dylib' -print0 |
    while IFS= read -r -d '' lib; do
      run codesign --force --timestamp --options runtime \
        --sign "$SIGN_APP" "$lib"
    done
fi
run codesign --force --timestamp --options runtime \
  --entitlements "$ENTITLEMENTS" \
  --sign "$SIGN_APP" "$APP"
run codesign --verify --deep --strict --verbose=2 "$APP"

# --- package ---------------------------------------------------------------
#
# App Store uploads for macOS take a signed installer package, not the .app.

say "package"
PKG="$OUTPUT_DIR/$(basename "$APP" .app)-$MARKETING_VERSION-$BUILD_NUMBER.pkg"
run productbuild --component "$APP" /Applications \
  --sign "$SIGN_INSTALLER" "$PKG"

if [ "$SKIP_UPLOAD" = "1" ]; then
  say "done (SKIP_UPLOAD=1)"
  echo "signed package: $PKG"
  exit 0
fi

# --- upload ----------------------------------------------------------------

say "upload"
set +e
ALTOOL_OUT=$(xcrun altool --upload-app \
  -f "$PKG" \
  -t macos \
  --apiKey "$ASC_KEY_ID" \
  --apiIssuer "$ASC_ISSUER" \
  --private-key "@file:$ASC_KEY_PATH" 2>&1)
ALTOOL_RC=$?
set -e
echo "$ALTOOL_OUT"

if [ "$ALTOOL_RC" -ne 0 ]; then
  # L4: per-marketing-version daily cap. Bumping the version resets it.
  if echo "$ALTOOL_OUT" | grep -qE "90382|Upload limit reached"; then
    NEXT=$(echo "$MARKETING_VERSION" | awk -F. -v OFS=. '{$3=$3+1; print}')
    die "TestFlight daily upload cap hit for $MARKETING_VERSION. Re-run with MARKETING_VERSION=$NEXT"
  fi
  # Ported diagnosis: altool hides an expired ASC agreement behind rc 19.
  if echo "$ALTOOL_OUT" | grep -qE "Cannot determine the Apple ID|\(19\)"; then
    die "rc 19 is usually an EXPIRED App Store Connect agreement, not a key or code bug. Check Agreements, Tax, and Banking."
  fi
  if echo "$ALTOOL_OUT" | grep -q "ITMS-90683"; then
    die "ITMS-90683 again: a purpose string is missing. Run: python3 $PACKAGER verify $APP"
  fi
  die "altool failed with rc $ALTOOL_RC"
fi

say "uploaded"
run git tag "macos-build-$BUILD_NUMBER"
echo "Tagged macos-build-$BUILD_NUMBER. Push it: git push origin macos-build-$BUILD_NUMBER"
echo
echo "The build now processes in App Store Connect (usually 5-15 min). It"
echo "appears in TestFlight once processing finishes. Internal testers get it"
echo "with no review; external testers need Beta App Review first."
