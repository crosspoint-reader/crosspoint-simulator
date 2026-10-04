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

Import("env", "projenv")
import os
import builtins
import re

RUN_SIMULATOR_TARGET_KEY = "_crosspoint_run_simulator_target_registered"
RUN_SIMULATOR_TARGET_OWNER_OPTION = "custom_run_simulator_target_owner"
SIMULATOR_HTTP_PORT_OPTION = "custom_simulator_http_port"
env.AppendUnique(CPPDEFINES=[("ARDUINOJSON_ENABLE_ARDUINO_STRING", 1)])
projenv.AppendUnique(CPPDEFINES=[("ARDUINOJSON_ENABLE_ARDUINO_STRING", 1)])

# The web manager uses the SDK's header-only crypto interface and base64 helpers.
content_headers = os.path.join(
    env.subst("$PROJECT_DIR"), "freeink-sdk", "libs", "book",
    "ContentProtection", "include"
)
if os.path.isdir(content_headers):
    env.AppendUnique(CPPPATH=[content_headers])


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
