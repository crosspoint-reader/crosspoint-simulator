"""
PlatformIO library build script for the Crosspoint Simulator.

Handles two things automatically when this lib is included as a lib_dep:

1. Patches BookMetadataCache -- SpineEntry::cumulativeSize and its fast read
   path can use size_t, which is 8 bytes on 64-bit hosts (macOS/Linux) but
   4 bytes on ESP32-C3. This mismatch breaks binary cache serialization in the
   simulator. Replaced with uint32_t, which is the correct explicit size on both
   platforms. Applied idempotently -- safe to run on every build.

2. Patches GfxRenderer::setOrientation so simulator builds notify HalDisplay
   when the logical orientation changes. Without this, the framebuffer content
   can rotate while the SDL window keeps its startup portrait/landscape shape.

3. Registers a backward-compatible "run_simulator" custom target.

This file can be loaded more than once in the same PlatformIO process:
- once from this library's `library.json` build hook
- again indirectly when a consuming firmware repo adds the separate
  `run_simulator_project.py` helper for IDE task exposure

Use a process-wide sentinel so the custom target is registered only once even
when multiple registration paths exist.
"""

Import("env")
import os
import builtins
import re

RUN_SIMULATOR_TARGET_KEY = "_crosspoint_run_simulator_target_registered"
RUN_SIMULATOR_TARGET_OWNER_OPTION = "custom_run_simulator_target_owner"
SIMULATOR_HTTP_PORT_OPTION = "custom_simulator_http_port"

PACKAGE_MACOS_APP_TARGET_KEY = "_crosspoint_package_macos_app_target_registered"
MACOS_APP_BUNDLE_ID_OPTION = "custom_macos_app_bundle_id"
MACOS_APP_VERSION_OPTION = "custom_macos_app_version"
MACOS_APP_BUILD_OPTION = "custom_macos_app_build"
MACOS_APP_OUTPUT_DIR_OPTION = "custom_macos_app_output_dir"
MACOS_APP_ICON_OPTION = "custom_macos_app_icon"

PACKAGE_SCRIPT_RELPATH = os.path.join("packaging", "macos", "package_macos_app.py")


# --- run_simulator custom target ---

def _run_simulator(source, target, env):
    import subprocess

    binary = env.subst("$BUILD_DIR/program")
    runtime_env = os.environ.copy()
    configured_http_port = env.GetProjectOption(
        SIMULATOR_HTTP_PORT_OPTION, ""
    ).strip()
    if configured_http_port:
        runtime_env["CROSSPOINT_SIM_HTTP_PORT"] = configured_http_port
    subprocess.run([binary], cwd=os.getcwd(), env=runtime_env)


target_owner = env.GetProjectOption(RUN_SIMULATOR_TARGET_OWNER_OPTION, "").strip().lower()

if target_owner != "project" and not getattr(builtins, RUN_SIMULATOR_TARGET_KEY, False):
    setattr(builtins, RUN_SIMULATOR_TARGET_KEY, True)
    env.AddCustomTarget(
        name="run_simulator",
        dependencies="$PROGPATH",
        actions=_run_simulator,
        title="Run Simulator",
        description="Build and run the desktop simulator",
        always_build=True,
    )


# --- package_macos_app custom target ---
#
# Wraps the built binary in a .app bundle whose Info.plist carries the privacy
# purpose strings the Mac App Store requires. Everything below resolves paths
# inside the target action, never at script-load time, so a packaging problem
# can never break an ordinary simulator build.

def _simulator_library_dir(env):
    """Locate this library's checkout, wherever PlatformIO put it."""
    candidates = []

    script_path = globals().get("__file__")
    if script_path:
        candidates.append(os.path.dirname(os.path.abspath(script_path)))

    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
    pioenv = env.subst("$PIOENV")
    if libdeps_dir and pioenv:
        env_libdeps = os.path.join(libdeps_dir, pioenv)
        candidates.append(os.path.join(env_libdeps, "simulator"))
        if os.path.isdir(env_libdeps):
            candidates.extend(
                os.path.join(env_libdeps, name) for name in sorted(os.listdir(env_libdeps))
            )

    for candidate in candidates:
        if os.path.isfile(os.path.join(candidate, PACKAGE_SCRIPT_RELPATH)):
            return candidate
    return None


def _simulator_device(env):
    """Map the environment's build flags back to a packaging device profile."""
    names = set()
    for define in env.get("CPPDEFINES", []):
        names.add(str(define[0]) if isinstance(define, (list, tuple)) else str(define))
    if "SIMULATOR_DEVICE_X3" in names:
        return "x3"
    if "SIMULATOR_DEVICE_X4_PRO" in names:
        return "x4-pro"
    return "x4"


def _package_macos_app(source, target, env):
    import subprocess
    import sys

    library_dir = _simulator_library_dir(env)
    if library_dir is None:
        print(
            "[SIM] cannot find %s; run it directly from the simulator checkout"
            % PACKAGE_SCRIPT_RELPATH
        )
        env.Exit(1)
        return

    command = [
        sys.executable,
        os.path.join(library_dir, PACKAGE_SCRIPT_RELPATH),
        "build",
        "--binary",
        env.subst("$BUILD_DIR/program"),
        "--device",
        _simulator_device(env),
        "--output-dir",
        env.GetProjectOption(MACOS_APP_OUTPUT_DIR_OPTION, "dist").strip() or "dist",
    ]
    for option, flag in (
        (MACOS_APP_BUNDLE_ID_OPTION, "--bundle-id"),
        (MACOS_APP_VERSION_OPTION, "--version"),
        (MACOS_APP_BUILD_OPTION, "--build"),
        (MACOS_APP_ICON_OPTION, "--icon"),
    ):
        value = env.GetProjectOption(option, "").strip()
        if value:
            command.extend([flag, value])

    result = subprocess.run(command, cwd=os.getcwd())
    if result.returncode != 0:
        env.Exit(result.returncode)


if not getattr(builtins, PACKAGE_MACOS_APP_TARGET_KEY, False):
    setattr(builtins, PACKAGE_MACOS_APP_TARGET_KEY, True)
    env.AddCustomTarget(
        name="package_macos_app",
        dependencies="$PROGPATH",
        actions=_package_macos_app,
        title="Package macOS App",
        description="Wrap the built simulator binary in a .app bundle for the Mac App Store",
        always_build=True,
    )
