#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
test_binary="$(mktemp "${TMPDIR:-/tmp}/crosspoint-simulator-compat.XXXXXX")"
trap 'rm -f "$test_binary"' EXIT

compile=(
  "${CXX:-c++}"
  -std=gnu++20
  -Wno-deprecated-declarations
  "-I$repo_root/src"
  "$repo_root/tests/host_compat_self_test.cpp"
  "$repo_root/src/HalClock.cpp"
)
if [[ -n "${LDFLAGS:-}" ]]; then
  read -r -a extra_link_flags <<< "$LDFLAGS"
  compile+=("${extra_link_flags[@]}")
fi
if [[ "$(uname -s)" == "Linux" ]]; then
  compile+=(-lcrypto)
fi
compile+=(-o "$test_binary")

"${compile[@]}"
"$test_binary"
printf 'host compatibility self-test passed\n'
