#!/usr/bin/env bash
# Build the Windows host/join launcher. Run from the repository root.
#   Native on Windows (MSYS2 MINGW32):  bash launcher/build-windows.sh
#   Cross from Linux (mingw-w64):        CC=i686-w64-mingw32-gcc bash launcher/build-windows.sh
#
# The launcher links ENet so its waiting room can ask the town server who is
# in a town before the game starts.
set -euo pipefail
CC=${CC:-gcc}
OUT=${1:-AnimalCrossingOnline.exe}
ENET=pc/lib/enet
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

for f in callbacks compress host list packet peer protocol win32; do
    "$CC" -O2 -w -I"$ENET/include" -c "$ENET/$f.c" -o "$TMP/$f.o"
done
"$CC" -O2 -Wall -mwindows -Inet -I"$ENET/include" \
    launcher/launcher.c "$TMP"/*.o \
    -o "$OUT" -lws2_32 -lwinmm -lshell32 -luser32 -lgdi32
echo "built $OUT"
