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

4. Compiles a patched copy of the firmware's GfxRenderer.cpp in which
   drawText() reports its strings to the simulator (CROSSPOINT_SIM_SCREEN_TEXT).
   The firmware checkout is not modified.

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


# --- screen text hook ---

DRAW_TEXT_START = "void GfxRenderer::drawText("
# After the empty-string and font-cache-scan returns, before glyphs are drawn.
DRAW_TEXT_ANCHOR = "  const auto fontIt = fontMap.find(resolvedFontId);"
SCREEN_TEXT_HOOK = "  simulatorScreenText(x, y, text);\n"


def _patch_gfx_renderer(lib_env, node):
    source = node.srcnode().get_abspath()
    with open(source, encoding="utf-8") as f:
        text = f.read()
    start = text.find(DRAW_TEXT_START)
    anchor = text.find(DRAW_TEXT_ANCHOR, start) if start >= 0 else -1
    if anchor < 0:
        print("Simulator: GfxRenderer::drawText changed; screen text is off")
        return node
    patched = (
        '#include "SimScreenText.h"\n'
        + "#line 1 \"%s\"\n" % source.replace("\\", "/")
        + text[:anchor]
        + SCREEN_TEXT_HOOK
        + "#line %d\n" % (text.count("\n", 0, anchor) + 1)
        + text[anchor:]
    )
    out_dir = os.path.join(lib_env.subst("$BUILD_DIR"), "simulator_patched")
    out = os.path.join(out_dir, "GfxRenderer.cpp")
    os.makedirs(out_dir, exist_ok=True)
    try:
        with open(out, encoding="utf-8") as f:
            current = f.read()
    except OSError:
        current = None
    # Rewrite only on change, so the object is not rebuilt every time.
    if current != patched:
        with open(out, "w", encoding="utf-8") as f:
            f.write(patched)
    # The quoted includes of the original resolve through the library folder.
    lib_env.AppendUnique(CPPPATH=[os.path.dirname(source)])
    return lib_env.File(out)


def _add_screen_text_hook():
    from SCons.Script import DefaultEnvironment

    # Library build scripts run while PlatformIO creates the library builders;
    # the project's lib/GfxRenderer is created before this library in libdeps.
    for lb in DefaultEnvironment().get("__PIO_LIB_BUILDERS", None) or []:
        if os.path.basename(lb.path) == "GfxRenderer":
            # Register once even if this script runs again in the same build.
            if not getattr(lb, "_crosspoint_screen_text", False):
                lb._crosspoint_screen_text = True
                lb.env.AddBuildMiddleware(_patch_gfx_renderer, "*GfxRenderer.cpp")
            return
    print("Simulator: GfxRenderer library not found; screen text is off")


_add_screen_text_hook()


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
