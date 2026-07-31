#!/usr/bin/env bash
# Pull the freeze post-mortem off the SummerCart64 SD card and resolve its
# raw addresses to symbols, so a hard freeze goes from "black screen" to a
# named function in one command.
#
#   ./tools/resolve_freeze.sh                # download from the cart + resolve
#   ./tools/resolve_freeze.sh freeze.txt     # resolve an already-saved report
#
# Requirements: the console powered OFF for the download (it owns the SD card
# while running), and build/mine64-deployed.out -- the symbol archive that
# live-load/perma-load write at deploy time, matching exactly the ROM that
# froze.  Rebuilding is fine; redeploying overwrites the archive, so resolve
# before the next load.
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_IMAGE="${MINE64_BUILD_IMAGE:-mine64-nusys-build:local}"
DOCKER_PLATFORM="${MINE64_DOCKER_PLATFORM:-linux/amd64}"
SC64_DEPLOYER="${SC64_DEPLOYER:-$PROJECT_DIR/../N64FlashcartMenu/tools/sc64/sc64deployer}"
SYMBOLS="$PROJECT_DIR/build/mine64-deployed.out"

if [[ ! -f "$SYMBOLS" ]]; then
    echo "No symbol archive at $SYMBOLS." >&2
    echo "./live-load and ./perma-load write it at deploy time; a report can" >&2
    echo "only be resolved against the exact binary that froze." >&2
    exit 1
fi

if [[ -n "${1:-}" ]]; then
    cp "$1" "$PROJECT_DIR/build/freeze.txt"
else
    echo "Downloading mine64/freeze.txt from the SummerCart64 SD card..."
    "$SC64_DEPLOYER" sd download mine64/freeze.txt "$PROJECT_DIR/build/freeze.txt"
fi

echo
docker run --rm --platform "$DOCKER_PLATFORM" \
    -v "$PROJECT_DIR:/work" -w /work \
    "$BUILD_IMAGE" sh -c '
mips-n64-nm -n build/mine64-deployed.out > /tmp/syms.txt &&
python3 tools/resolve_freeze_report.py build/freeze.txt /tmp/syms.txt'
