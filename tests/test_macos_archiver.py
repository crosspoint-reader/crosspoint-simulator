"""Run with python3 -m unittest discover -s tests -p test_macos_archiver.py."""
from pathlib import Path
import runpy
import sys
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "configure_macos_archiver.py"


class Environment(dict):
    def subst(self, value):
        if value != "$PIOPLATFORM":
            raise AssertionError(value)
        return self["PIOPLATFORM"]

    def Replace(self, **values):
        self.update(values)


class MacosArchiverTest(unittest.TestCase):
    def apply(self, host, platform):
        env = Environment(PIOPLATFORM=platform, AR="cross-ar", RANLIB="cross-ranlib", CXX="custom-c++")
        with patch.object(sys, "platform", host):
            runpy.run_path(str(SCRIPT), init_globals={"env": env, "Import": lambda *_: None})
        return env

    def test_native_macos_uses_darwin_archive_tools(self):
        env = self.apply("darwin", "native")
        self.assertEqual(env["AR"], "/usr/bin/ar")
        self.assertEqual(env["RANLIB"], "/usr/bin/ranlib")
        self.assertEqual(env["CXX"], "custom-c++")

    def test_linux_native_is_unchanged(self):
        env = self.apply("linux", "native")
        self.assertEqual(env["AR"], "cross-ar")
        self.assertEqual(env["RANLIB"], "cross-ranlib")

    def test_embedded_build_on_macos_is_unchanged(self):
        env = self.apply("darwin", "espressif32")
        self.assertEqual(env["AR"], "cross-ar")
        self.assertEqual(env["RANLIB"], "cross-ranlib")

    def test_windows_native_is_unchanged(self):
        env = self.apply("win32", "native")
        self.assertEqual(env["AR"], "cross-ar")
        self.assertEqual(env["RANLIB"], "cross-ranlib")


if __name__ == "__main__":
    unittest.main()
