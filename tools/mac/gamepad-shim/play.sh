#!/bin/sh
# Start an installed OpenCNC3D with the game controller shim loaded.
#
#   tools/mac/gamepad-shim/play.sh [--launcher] [game arguments...]
#
#   OPENCNC3D_DIR      the game folder (default ~/Applications/OpenCNC3D)
#   OPENCNC3D_GAMEPAD_DEBUG=1   log every pad event to the game's log
#
# Without --launcher the game itself is started, which is the quick way in. With it the
# launcher is, and it hands the environment, and so the shim, to the game it starts.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SHIM="$HERE/libopencnc3d-gamepad.dylib"
GAME_DIR="${OPENCNC3D_DIR:-$HOME/Applications/OpenCNC3D}"
[ -f "$SHIM" ] || "$HERE/build.sh" "$GAME_DIR"

PROG=./opencnc3d
if [ "$1" = "--launcher" ]; then
    shift
    PROG=./opencnc3d-launcher
    set -- -- "$@"
fi
cd "$GAME_DIR"
DYLD_INSERT_LIBRARIES="$SHIM" exec "$PROG" "$@"
