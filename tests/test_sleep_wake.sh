#!/usr/bin/env bash
# Headless regression test: a FAST synthetic POWER tap must wake the
# deep-sleeping simulator.
#
# The bug this pins down: HalGPIO::startDeepSleep polled syntheticButtonDown[]
# as a LEVEL once per 10 ms iteration. A tap whose down and up edges are both
# applied inside one processSyntheticEvents() call (e.g. a 1 ms scripted tap,
# or iOS delivering a queued FINGER_DOWN+FINGER_UP burst after unlock) leaves
# no level behind to observe -- the wake is silently missed and the device
# sleeps forever. The fix latches an edge in injectButtonDown that the sleep
# loop consumes.
#
# Scenario (all times from process start):
#   2500ms POWER held 700ms  -> firmware's own sleep path (threshold 400ms;
#                               main.cpp only allows sleep after boot+2000ms,
#                               so the hold must start beyond that)
#   6000ms POWER tapped 1ms  -> must wake; wake = process relaunch, which
#                               promotes *_AFTER_WAKE schedules
#   after-wake: screenshot proves the relaunched process rendered, then QUIT.
#
# Pass: exits 0 with the after-wake screenshot present. Fail: the 1ms tap is
# missed, no relaunch happens, the timeout kills a still-sleeping process.
#
# Usage: tests/test_sleep_wake.sh <firmware-checkout-dir>

set -u
FIRMWARE_DIR="${1:?usage: test_sleep_wake.sh <firmware-checkout-dir>}"
BIN="$FIRMWARE_DIR/.pio/build/simulator/program"
[ -x "$BIN" ] || { echo "SKIP: simulator binary not built at $BIN"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SHOT="$WORK/after-wake.bmp"

cd "$FIRMWARE_DIR"
CROSSPOINT_SIM_INPUT_SCRIPT='2500:POWER:700;6000:POWER:1' \
CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE='1500:QUIT' \
CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE="1000:$SHOT" \
"$BIN" >"$WORK/log.txt" 2>&1 &
PID=$!

# 60s ceiling: normal pass completes in ~9s (sleep at ~2.9s, tap at 6s,
# relaunch + 1.5s after-wake script), but the FIRST exec of a freshly built
# binary can eat 20s+ in macOS Gatekeeper verification before main() runs.
for _ in $(seq 1 120); do
  kill -0 "$PID" 2>/dev/null || break
  sleep 0.5
done
if kill -0 "$PID" 2>/dev/null; then
  kill -9 "$PID" 2>/dev/null
  echo "FAIL: simulator still running after 30s -- the 1ms wake tap was missed"
  exit 1
fi
wait "$PID"; RC=$?

if [ ! -s "$SHOT" ]; then
  echo "FAIL: process exited (rc=$RC) but no after-wake screenshot -- it never relaunched as a wake"
  tail -5 "$WORK/log.txt"
  exit 1
fi
echo "PASS: fast tap woke the device (after-wake screenshot captured, rc=$RC)"
exit 0
