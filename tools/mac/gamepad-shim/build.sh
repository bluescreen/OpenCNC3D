#!/bin/sh
# Build the game controller shim for an installed OpenCNC3D.
#
#   tools/mac/gamepad-shim/build.sh [GAME_DIR]
#
# GAME_DIR (default ~/Applications/OpenCNC3D) is where the game's own libSDL2-2.0.0.dylib
# is linked from. Its install name is @executable_path/libSDL2-2.0.0.dylib, so the shim
# binds to whichever copy the game it is loaded into carries, never to a second SDL.
set -e
cd "$(dirname "$0")"
GAME_DIR="${1:-$HOME/Applications/OpenCNC3D}"
SDL_LIB="$GAME_DIR/libSDL2-2.0.0.dylib"
[ -f "$SDL_LIB" ] || { echo "no $SDL_LIB: pass the game folder" >&2; exit 1; }
command -v sdl2-config >/dev/null || { echo "sdl2-config not found (only its headers are used)" >&2; exit 1; }

. ../deployment-target.sh

# Both CPUs, because the game is universal and a Mac may run it under Rosetta.
cc -arch arm64 -arch x86_64 -mmacosx-version-min="$OPENCNC3D_MACOS_MIN" -O2 -Wall -Wextra \
   -dynamiclib -o libopencnc3d-gamepad.dylib \
   shim.c ../../../input/gamepad.c ../../../input/gamepad_draw.c ../../../input/gamepad_mac.m -fobjc-arc \
   -framework OpenGL -framework GameController -framework Foundation \
   $(sdl2-config --cflags) "$SDL_LIB" \
   -install_name @rpath/libopencnc3d-gamepad.dylib
codesign --force -s - libopencnc3d-gamepad.dylib
echo "built $(pwd)/libopencnc3d-gamepad.dylib ($(lipo -archs libopencnc3d-gamepad.dylib))"
