#!/bin/sh
# Compile the player-edit pool for the host and check its invariants.
#
# Same shape as tools/gentest/run.sh, and it shares that harness's stubs: the
# pool is pure arithmetic over world.c's block window, so it runs here even
# though Mine64 itself cannot.  The one stub it does not take is the edit
# overlay -- that is the thing under test.
set -e

cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

${CC:-cc} -std=c89 -Wall -Wextra -Wno-unused-parameter -O1 \
  -funsigned-char -fno-builtin \
  -I tools/gentest/shim -I include \
  -o "$out/edittest" \
  tools/edittest/main.c tools/gentest/stubs.c \
  src/edits.c src/home.c src/world.c src/noise.c src/math.c src/mods.c -lm

"$out/edittest"
