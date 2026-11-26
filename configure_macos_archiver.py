"""Keep an ESP cross-toolchain on PATH from producing non-Darwin archives."""
import sys

Import("env")

if sys.platform == "darwin" and env.subst("$PIOPLATFORM") == "native":
    env.Replace(AR="/usr/bin/ar", RANLIB="/usr/bin/ranlib")
