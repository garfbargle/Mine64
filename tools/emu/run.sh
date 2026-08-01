#!/bin/zsh
# Run Mine64 under mupen64plus with a scripted controller, capturing screenshots.
#
#   tools/emu/run.sh <script> [shotdir]
#
# <script> is a timeline for the mine64 input plugin (see mine64_input.c for the
# grammar); every `shot` in it writes a PNG into <shotdir>, default build/shots.
# The emulator still opens a window -- the GL video plugin needs one -- but it
# needs no focus and no keyboard, so the run is reproducible and unattended.

set -e

HERE="${0:A:h}"
ROOT="${HERE:h:h}"
SCRIPT="${1:?usage: run.sh <script> [shotdir]}"
SHOTDIR="${2:-$ROOT/build/shots}"
ROM="${ROM:-$ROOT/build/mine64.n64}"
PLUGIN="$HERE/mupen64plus-input-mine64.dylib"

[[ -f "$PLUGIN" ]] || { echo "building input plugin..."; "$HERE/build.sh"; }
[[ -f "$ROM" ]] || { echo "no ROM at $ROM -- run make first" >&2; exit 1 }

rm -rf "$SHOTDIR"
mkdir -p "$SHOTDIR"

# 320x240 is the game's own framebuffer (osViModeNtscLan1), so captures come out
# 1:1 instead of upscaled.  RES=640x480 doubles them for a closer look, but that
# is the video plugin resampling, not extra detail.
MINE64_INPUT_SCRIPT="${SCRIPT:A}" mupen64plus \
  --noosd --nosaveoptions --windowed --resolution "${RES:-320x240}" \
  --gfx mupen64plus-video-glide64mk2 \
  --audio dummy \
  --rsp mupen64plus-rsp-hle \
  --input "$PLUGIN" \
  --sshotdir "$SHOTDIR" \
  "$ROM" 2>&1 | grep -viE "^(Input|Video) Warning|rumble" || true

# The core numbers screenshots in capture order, so a `shot <label>` in the
# script can name its own PNG by matching them up again afterwards.
labels=("${(@f)$(sed -e 's/#.*//' "$SCRIPT" | awk 'tolower($1)=="shot" {print ($2 == "" ? "shot" : $2)}')}")
i=1
for png in "$SHOTDIR"/*.png(N); do
  label="${labels[$i]}"
  [[ -n "$label" ]] && mv "$png" "$SHOTDIR/$(printf '%02d' $i)-$label.png"
  (( i++ ))
done

echo "--- $SHOTDIR ---"
ls -1 "$SHOTDIR" 2>/dev/null || echo "(nothing captured)"
