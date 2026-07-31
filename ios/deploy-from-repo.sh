#!/bin/zsh
# Wrapper: run the TestFlight deploy from the repo root with fresh code,
# regardless of the invoking shell's cwd.
#
# Exists because Terminal `do script` tabs open in $HOME, and inline `cd &&`
# kept getting stripped from osascript invocations (crds-ios, 2026-06-09).
# deploy.applescript calls this; phones fire deploy.applescript over SSH.
#
# Pulls before building so a phone-fired deploy ships what was just merged,
# not whatever the Mac checkout happened to have. --ff-only so a diverged
# checkout stops loudly instead of merging silently.
cd "$HOME/src/crosspoint-simulator" || exit 1
echo "== branch: $(git branch --show-current)"
git pull --ff-only || { echo "git pull failed — resolve on the Mac"; exit 1; }
exec ./ios/testflight.sh "$@"
