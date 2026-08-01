#!/bin/zsh
# Build the scripted input plugin against the mupen64plus headers from Homebrew.
set -e
HERE="${0:A:h}"
PREFIX="${HOMEBREW_PREFIX:-/opt/homebrew}"
clang -O2 -Wall -Wextra -Wno-unused-parameter -fPIC -shared \
  -I"$PREFIX/include" \
  -o "$HERE/mupen64plus-input-mine64.dylib" "$HERE/mine64_input.c"
echo "built $HERE/mupen64plus-input-mine64.dylib"
