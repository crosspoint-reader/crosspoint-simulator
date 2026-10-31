#!/usr/bin/env python3
"""Run the simulator with one SD plugin installed on a fresh SD card.

Run from the firmware project after `pio run -e simulator`:

    python3 ../crosspoint-simulator/tools/run_plugin.py path/to/my.crosspoint-plugin \\
        --config my-config.json --open

The SD card is a temporary folder deleted on exit, so a token in the config
does not stay on disk; pass --sd to keep a card between runs. See README.md.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Home > Plugins > the only plugin > the first simulated Wi-Fi network. Each
# step waits for the activity log line of the screen it leads to.
OPEN_SCRIPT = (
    "0:WAIT:Entering activity: Home;300:DOWN;600:DOWN;900:ENTER;"
    "900:WAIT:Entering activity: PluginCatalog;400:ENTER;"
    "400:WAIT:Entering activity: WifiSelection;600:ENTER;"
    "600:WAIT:Exiting activity: WifiSelection"
)
PLUGIN_SETTINGS = {"pluginsEnabled": 1, "pluginHubPromptHidden": 1}


def plugin_name(plugin_dir):
    manifest = plugin_dir / "manifest.json"
    if manifest.is_file():
        name = json.loads(manifest.read_text(encoding="utf-8")).get("name")
        if name:
            return name
    return plugin_dir.name.removesuffix(".crosspoint-plugin")


def config_file_name(plugin_dir):
    device = plugin_dir / "device.json"
    if device.is_file():
        config = json.loads(device.read_text(encoding="utf-8")).get("config")
        if isinstance(config, dict) and config.get("file"):
            return config["file"]
    return "config.json"


def prepare_sd(sd, plugin_dir, config):
    crosspoint = sd / ".crosspoint"
    crosspoint.mkdir(parents=True, exist_ok=True)
    (sd / "books").mkdir(exist_ok=True)

    settings_path = crosspoint / "settings.json"
    settings = {}
    if settings_path.is_file():
        settings = json.loads(settings_path.read_text(encoding="utf-8"))
    settings.update(PLUGIN_SETTINGS)
    settings_path.write_text(json.dumps(settings), encoding="utf-8")

    name = plugin_name(plugin_dir)
    target = crosspoint / "plugins" / name
    # Keep what the plugin wrote on a kept card (downloads, queued events);
    # replace only the files that come from the plugin folder.
    shutil.copytree(plugin_dir, target, dirs_exist_ok=True)
    if config:
        # As in the firmware: an absolute path is on the card, a relative one is
        # in the plugin's folder.
        name_on_card = config_file_name(plugin_dir)
        if name_on_card.startswith("/"):
            config_path = sd / name_on_card.lstrip("/")
        else:
            config_path = target / name_on_card
        config_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(config, config_path)
    return name


def build_script(open_plugin, script):
    parts = [OPEN_SCRIPT] if open_plugin else []
    if script:
        parts.append(script)
    return ";".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("plugin", type=Path, help="plugin folder (with manifest.json and device.json)")
    parser.add_argument("--program", type=Path, default=Path(".pio/build/simulator/program"),
                        help="simulator binary (default: %(default)s)")
    parser.add_argument("--config", type=Path, help="JSON file installed as the plugin's config file")
    parser.add_argument("--sd", type=Path, help="keep the SD card in this folder instead of a temporary one")
    parser.add_argument("--open", action="store_true",
                        help="go to the plugin and through Wi-Fi before the script starts")
    parser.add_argument("--script", default="",
                        help="CROSSPOINT_SIM_INPUT_SCRIPT steps; with --open, times count from the plugin opening")
    parser.add_argument("--routes", type=Path, help="CROSSPOINT_SIM_HTTP_ROUTES file")
    parser.add_argument("--http-log", help="CROSSPOINT_SIM_HTTP_LOG target (a file, or - for stderr)")
    parser.add_argument("--no-screen-text", action="store_true", help="do not print [SIM] SCREEN lines")
    parser.add_argument("--headless", action="store_true", help="no window (SDL dummy video driver)")
    parser.add_argument("--port", help="CROSSPOINT_SIM_HTTP_PORT for the firmware web server")
    args = parser.parse_args()

    plugin_dir = args.plugin.resolve()
    if not plugin_dir.is_dir():
        parser.error(f"plugin folder not found: {args.plugin}")
    program = args.program.resolve()
    if not program.is_file():
        parser.error(f"simulator binary not found: {args.program} (build with: pio run -e simulator)")
    if args.config and not args.config.is_file():
        parser.error(f"config file not found: {args.config}")

    env = os.environ.copy()
    script = build_script(args.open, args.script)
    if script:
        env["CROSSPOINT_SIM_INPUT_SCRIPT"] = script
    if args.routes:
        env["CROSSPOINT_SIM_HTTP_ROUTES"] = str(args.routes.resolve())
    if args.http_log:
        env["CROSSPOINT_SIM_HTTP_LOG"] = args.http_log if args.http_log == "-" else str(Path(args.http_log).resolve())
    if not args.no_screen_text:
        env["CROSSPOINT_SIM_SCREEN_TEXT"] = "1"
    if args.headless:
        env["SDL_VIDEODRIVER"] = "dummy"
    if args.port:
        env["CROSSPOINT_SIM_HTTP_PORT"] = args.port

    with tempfile.TemporaryDirectory(prefix="crosspoint-plugin-sd-") as temp:
        sd = args.sd.resolve() if args.sd else Path(temp) / "sd"
        name = prepare_sd(sd, plugin_dir, args.config)
        env["CROSSPOINT_SIM_SD"] = str(sd)
        print(f"[run_plugin] {name} on SD {sd}" + ("" if args.sd else " (deleted on exit)"), file=sys.stderr)
        return subprocess.run([str(program)], env=env).returncode


if __name__ == "__main__":
    sys.exit(main())
