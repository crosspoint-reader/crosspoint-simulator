#!/usr/bin/env python3
"""
Build, patch, and verify the macOS app bundle for a CrossPoint simulator build.

The native PlatformIO build produces a bare Mach-O executable
(`.pio/build/<env>/program`). The Mac App Store needs that executable inside an
app bundle whose `Info.plist` carries privacy purpose strings, otherwise App
Store Connect rejects the upload with ITMS-90683:

    Missing purpose string in Info.plist -- ... should contain a
    NSCameraUsageDescription key ...

The simulator itself only ever calls `SDL_Init(SDL_INIT_VIDEO)`; it does not
open a camera or a Bluetooth device. The rejection comes from Apple's static
scan of the linked SDL2 library, which references those APIs for camera and
game-controller support. As Apple's notice says, "While your app might not use
these APIs, a purpose string is still required." The strings therefore live in
Info.plist.in next to this script, which is the single source of truth for all
three subcommands.

Subcommands
-----------
  build    Assemble `<name>.app` around a built simulator binary.
  patch    Add missing purpose strings to a bundle built by another pipeline
           (Xcode, a packaging script, a hand-assembled bundle).
  verify   Exit non-zero if a bundle is missing a purpose string. Run this
           before every upload.

Examples
--------
    python3 packaging/macos/package_macos_app.py build \\
        --binary .pio/build/simulator_x3/program \\
        --device x3 --version 0.1.0 --build 2 \\
        --bundle-id com.example.CrossPointX3 --output-dir dist

    python3 packaging/macos/package_macos_app.py patch dist/CrossPointX3.app
    python3 packaging/macos/package_macos_app.py verify dist/CrossPointX3.app

This script does not code-sign, notarize, or embed dylibs. Keep those steps in
whatever signing pipeline already produces the uploaded build; run `verify`
just before upload so a bundle can never regress.
"""

import argparse
import os
import plistlib
import shutil
import stat
import string
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TEMPLATE_PATH = os.path.join(SCRIPT_DIR, "Info.plist.in")

# Keys App Store Connect demands for a build that links SDL2. NSCamera... is the
# blocking ITMS-90683 error; NSBluetoothAlways... was reported as a warning on
# the same upload and blocks later ones. NSBluetoothPeripheral... is the legacy
# spelling that older systems read instead of NSBluetoothAlways....
REQUIRED_PRIVACY_KEYS = (
    "NSCameraUsageDescription",
    "NSBluetoothAlwaysUsageDescription",
    "NSBluetoothPeripheralUsageDescription",
)

DEVICE_PROFILES = {
    "x3": {"product_name": "CrossPoint X3", "executable_name": "CrossPointX3"},
    "x4": {"product_name": "CrossPoint X4", "executable_name": "CrossPointX4"},
    "x4-pro": {"product_name": "CrossPoint X4 Pro", "executable_name": "CrossPointX4Pro"},
}

DEFAULT_BUNDLE_ID_PREFIX = "com.crosspoint"
DEFAULT_MINIMUM_SYSTEM_VERSION = "11.0"
DEFAULT_COPYRIGHT = "Distributed under the MIT License."
DEFAULT_SHORT_VERSION = "0.1.0"
DEFAULT_BUILD_VERSION = "1"


class PackagingError(Exception):
    """A user-facing failure; reported without a traceback."""


# --- Info.plist template ---


def _substitute(value, mapping):
    """Expand ${...} placeholders through every string in a plist value."""
    if isinstance(value, str):
        return string.Template(value).substitute(mapping)
    if isinstance(value, dict):
        return {k: _substitute(v, mapping) for k, v in value.items()}
    if isinstance(value, list):
        return [_substitute(v, mapping) for v in value]
    return value


def load_template(mapping):
    """Read Info.plist.in and expand its placeholders.

    Placeholders that resolve to an empty string drop their key entirely, so an
    omitted icon does not leave CFBundleIconFile pointing at a missing file.
    """
    try:
        with open(TEMPLATE_PATH, "rb") as handle:
            template = plistlib.load(handle)
    except FileNotFoundError:
        raise PackagingError("Info.plist template not found at %s" % TEMPLATE_PATH)
    except Exception as error:  # malformed XML, unreadable file
        raise PackagingError("could not read %s: %s" % (TEMPLATE_PATH, error))

    try:
        expanded = _substitute(template, mapping)
    except KeyError as error:
        raise PackagingError(
            "Info.plist.in uses placeholder %s, which this script does not define" % error
        )
    return {k: v for k, v in expanded.items() if v != ""}


def privacy_strings(product_name):
    """The purpose strings from the template, worded for one product name."""
    template = load_template(_placeholder_mapping(product_name=product_name))
    missing = [key for key in REQUIRED_PRIVACY_KEYS if not template.get(key)]
    if missing:
        raise PackagingError(
            "Info.plist.in is missing required purpose strings: %s" % ", ".join(missing)
        )
    return {key: template[key] for key in REQUIRED_PRIVACY_KEYS}


def _placeholder_mapping(
    product_name,
    executable_name="",
    bundle_identifier="",
    short_version="",
    build_version="",
    minimum_system_version="",
    icon_file="",
    copyright_text="",
):
    return {
        "PRODUCT_NAME": product_name,
        "EXECUTABLE_NAME": executable_name,
        "BUNDLE_IDENTIFIER": bundle_identifier,
        "BUNDLE_SHORT_VERSION_STRING": short_version,
        "BUNDLE_VERSION": build_version,
        "MINIMUM_SYSTEM_VERSION": minimum_system_version,
        "ICON_FILE": icon_file,
        "COPYRIGHT": copyright_text,
    }


# --- bundle plist I/O ---


def resolve_plist_path(path):
    """Accept either a `.app` bundle or a direct path to an Info.plist."""
    if os.path.isdir(path):
        candidate = os.path.join(path, "Contents", "Info.plist")
        if not os.path.isfile(candidate):
            raise PackagingError("no Contents/Info.plist inside %s" % path)
        return candidate
    if os.path.isfile(path):
        return path
    raise PackagingError("no such bundle or plist: %s" % path)


def read_plist(path):
    """Load a plist, remembering whether it was XML or binary on disk.

    Xcode writes binary plists into built bundles; rewriting one as XML would
    still work, but keeping the original format keeps diffs and signatures
    boring.
    """
    with open(path, "rb") as handle:
        data = handle.read()
    fmt = plistlib.FMT_BINARY if data[:8] == b"bplist00" else plistlib.FMT_XML
    try:
        return plistlib.loads(data), fmt
    except Exception as error:
        raise PackagingError("could not parse %s: %s" % (path, error))


def write_plist(path, contents, fmt=plistlib.FMT_XML):
    with open(path, "wb") as handle:
        plistlib.dump(contents, handle, fmt=fmt, sort_keys=False)


def product_name_from_plist(contents):
    for key in ("CFBundleDisplayName", "CFBundleName", "CFBundleExecutable"):
        value = contents.get(key)
        if isinstance(value, str) and value:
            return value
    return "This app"


# --- build ---


def _ensure_replaceable(bundle_path):
    """Refuse to delete anything that is not recognizably an app bundle."""
    if not os.path.exists(bundle_path):
        return
    looks_like_bundle = bundle_path.endswith(".app") and os.path.isdir(
        os.path.join(bundle_path, "Contents")
    )
    if not looks_like_bundle:
        raise PackagingError(
            "%s already exists and is not an app bundle; refusing to overwrite it"
            % bundle_path
        )
    shutil.rmtree(bundle_path)


def cmd_build(args):
    profile = DEVICE_PROFILES[args.device]
    product_name = args.product_name or profile["product_name"]
    executable_name = args.executable_name or profile["executable_name"]
    bundle_id = args.bundle_id or "%s.%s" % (DEFAULT_BUNDLE_ID_PREFIX, executable_name)

    if not os.path.isfile(args.binary):
        raise PackagingError(
            "simulator binary not found: %s (build it first, e.g. `pio run -e simulator_%s`)"
            % (args.binary, args.device.replace("-", "_"))
        )

    if args.output:
        bundle_path = args.output
        if not bundle_path.endswith(".app"):
            raise PackagingError("--output must end in .app (got %s)" % bundle_path)
    else:
        bundle_path = os.path.join(args.output_dir, "%s.app" % executable_name)

    icon_file = os.path.basename(args.icon) if args.icon else ""
    if args.icon and not os.path.isfile(args.icon):
        raise PackagingError("icon not found: %s" % args.icon)

    info = load_template(
        _placeholder_mapping(
            product_name=product_name,
            executable_name=executable_name,
            bundle_identifier=bundle_id,
            short_version=args.version,
            build_version=args.build,
            minimum_system_version=args.minimum_system_version,
            icon_file=icon_file,
            copyright_text=args.copyright,
        )
    )

    _ensure_replaceable(bundle_path)
    contents_dir = os.path.join(bundle_path, "Contents")
    macos_dir = os.path.join(contents_dir, "MacOS")
    resources_dir = os.path.join(contents_dir, "Resources")
    os.makedirs(macos_dir)
    os.makedirs(resources_dir)

    installed_binary = os.path.join(macos_dir, executable_name)
    shutil.copy2(args.binary, installed_binary)
    mode = os.stat(installed_binary).st_mode
    os.chmod(installed_binary, mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    if args.icon:
        shutil.copy2(args.icon, os.path.join(resources_dir, icon_file))

    write_plist(os.path.join(contents_dir, "Info.plist"), info)
    with open(os.path.join(contents_dir, "PkgInfo"), "w") as handle:
        handle.write("APPL????")

    print("Built %s" % bundle_path)
    print("  bundle identifier   %s" % bundle_id)
    print("  version / build     %s (%s)" % (args.version, args.build))
    print("  purpose strings     %s" % ", ".join(REQUIRED_PRIVACY_KEYS))
    print(
        "\nBefore uploading: the bundle identifier must match the app record in "
        "App Store Connect, and CFBundleVersion (%s) must be higher than any "
        "build already uploaded for version %s." % (args.build, args.version)
    )
    print(
        "This script does not code-sign, notarize, or embed SDL2; run those "
        "steps next, then re-run `verify` on the signed bundle."
    )
    return 0


# --- patch ---


def cmd_patch(args):
    plist_path = resolve_plist_path(args.bundle)
    contents, fmt = read_plist(plist_path)
    product_name = args.product_name or product_name_from_plist(contents)
    strings = privacy_strings(product_name)

    added = []
    replaced = []
    for key, value in strings.items():
        existing = contents.get(key)
        if isinstance(existing, str) and existing.strip():
            if args.overwrite and existing != value:
                contents[key] = value
                replaced.append(key)
            continue
        contents[key] = value
        added.append(key)

    if not added and not replaced:
        print("%s already has every required purpose string; nothing to do." % plist_path)
        return 0

    if args.dry_run:
        for key in added:
            print("would add %s" % key)
        for key in replaced:
            print("would replace %s" % key)
        return 0

    write_plist(plist_path, contents, fmt=fmt)
    for key in added:
        print("added %s" % key)
    for key in replaced:
        print("replaced %s" % key)
    print("Updated %s" % plist_path)
    print(
        "Re-sign the bundle after this edit; changing Info.plist invalidates an "
        "existing signature."
    )
    return 0


# --- verify ---


def _invocation_path():
    """The shortest sensible way to name this script in a suggested command."""
    absolute = os.path.abspath(__file__)
    try:
        relative = os.path.relpath(absolute)
    except ValueError:  # different drive/root
        return absolute
    return relative if len(relative) < len(absolute) else absolute


def cmd_verify(args):
    plist_path = resolve_plist_path(args.bundle)
    contents, _ = read_plist(plist_path)

    missing = []
    for key in REQUIRED_PRIVACY_KEYS:
        value = contents.get(key)
        if not isinstance(value, str) or not value.strip():
            missing.append(key)

    if missing:
        print("%s is missing required purpose strings:" % plist_path, file=sys.stderr)
        for key in missing:
            print("  %s" % key, file=sys.stderr)
        print(
            "\nApp Store Connect will reject this build with ITMS-90683. Fix it with:"
            "\n  python3 %s patch %s" % (_invocation_path(), args.bundle),
            file=sys.stderr,
        )
        return 1

    print("%s has all %d required purpose strings." % (plist_path, len(REQUIRED_PRIVACY_KEYS)))
    return 0


# --- CLI ---


def build_parser():
    parser = argparse.ArgumentParser(
        prog="package_macos_app.py",
        description="Build, patch, and verify the macOS app bundle for a CrossPoint build.",
    )
    subparsers = parser.add_subparsers(dest="command")

    build = subparsers.add_parser("build", help="assemble a .app around a built binary")
    build.add_argument("--binary", required=True, help="path to the built simulator binary")
    build.add_argument(
        "--device",
        default="x4",
        choices=sorted(DEVICE_PROFILES),
        help="device profile the binary was built for (default: x4)",
    )
    build.add_argument("--output", help="full path of the .app to write")
    build.add_argument(
        "--output-dir",
        default="dist",
        help="directory to write <Executable>.app into when --output is omitted (default: dist)",
    )
    build.add_argument(
        "--bundle-id",
        help="CFBundleIdentifier; must match the App Store Connect record "
        "(default: %s.<Executable>)" % DEFAULT_BUNDLE_ID_PREFIX,
    )
    build.add_argument(
        "--version",
        default=DEFAULT_SHORT_VERSION,
        help="CFBundleShortVersionString (default: %s)" % DEFAULT_SHORT_VERSION,
    )
    build.add_argument(
        "--build",
        default=DEFAULT_BUILD_VERSION,
        help="CFBundleVersion; must increase with every upload (default: %s)"
        % DEFAULT_BUILD_VERSION,
    )
    build.add_argument("--product-name", help="override the display name for this device")
    build.add_argument("--executable-name", help="override the executable name inside the bundle")
    build.add_argument("--icon", help="path to an .icns file to install as the bundle icon")
    build.add_argument(
        "--minimum-system-version",
        default=DEFAULT_MINIMUM_SYSTEM_VERSION,
        help="LSMinimumSystemVersion (default: %s)" % DEFAULT_MINIMUM_SYSTEM_VERSION,
    )
    build.add_argument(
        "--copyright",
        default=DEFAULT_COPYRIGHT,
        help="NSHumanReadableCopyright (default: %s)" % DEFAULT_COPYRIGHT,
    )
    build.set_defaults(func=cmd_build)

    patch = subparsers.add_parser(
        "patch", help="add missing purpose strings to an existing bundle"
    )
    patch.add_argument("bundle", help="path to a .app bundle or an Info.plist")
    patch.add_argument(
        "--overwrite",
        action="store_true",
        help="also replace purpose strings that are already present",
    )
    patch.add_argument("--dry-run", action="store_true", help="report changes without writing")
    patch.add_argument(
        "--product-name",
        help="name to use in the strings (default: read from the bundle's Info.plist)",
    )
    patch.set_defaults(func=cmd_patch)

    verify = subparsers.add_parser(
        "verify", help="exit non-zero if a bundle is missing a purpose string"
    )
    verify.add_argument("bundle", help="path to a .app bundle or an Info.plist")
    verify.set_defaults(func=cmd_verify)

    return parser


def main(argv):
    parser = build_parser()
    args = parser.parse_args(argv)
    if not getattr(args, "func", None):
        parser.print_help()
        return 2
    try:
        return args.func(args)
    except PackagingError as error:
        print("error: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
